/*
 * Note: the main purpose of this repl implementation is to run
 * (our fork of) the wasm3 testsuite:
 * https://github.com/yamt/wasm3/blob/toywasm-test/test/run-spec-test.py
 *
 * eg.
 * ./run-spec-test.py --exec ".../main_bin --repl --repl-prompt wasm3"
 */

#define _GNU_SOURCE      /* strdup */
#define _DARWIN_C_SOURCE /* strdup */
#define _NETBSD_SOURCE   /* strdup */

#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "endian.h"
#include "exec_context.h"
#include "fileio.h"
#include "instance.h"
#include "load_context.h"
#include "module.h"
#include "module_writer.h"
#include "nbio.h"
#include "repl.h"
#include "report.h"
#include "suspend.h"
#include "timeutil.h"
#include "toywasm_version.h"
#include "type.h"
#include "usched.h"
#if defined(TOYWASM_ENABLE_WASI)
#include "wasi.h"
#endif
#if defined(TOYWASM_ENABLE_WASI_THREADS)
#include "wasi_threads.h"
#endif
#include "xlog.h"

/*
 * Note: ref_is_null.wast distinguishes "ref.extern 0" and "ref.null extern"
 * while this implementation uses 0/NULL to represent "ref.null extern".
 *
 * wast                our representation
 * -----------------   -------------------
 * "ref.extern 0"      EXTERNREF_0
 * "ref.null extern"   NULL
 *
 * cf.
 * https://webassembly.github.io/spec/core/syntax/types.html#reference-types
 * > The type externref denotes the infinite union of all references to
 * > objects owned by the embedder and that can be passed into WebAssembly
 * > under this type.
 */
#define EXTERNREF_0 ((uintptr_t)-1)

int
str_to_uint(const char *s, int base, uintmax_t *resultp)
{
        uintmax_t v;
        char *ep;
        errno = 0;
        v = strtoumax(s, &ep, base);
        if (s == ep) {
                return EINVAL;
        }
        if (*ep != 0) {
                return EINVAL;
        }
        if (errno != 0) {
                return errno;
        }
        *resultp = v;
        return 0;
}

int
str_to_ptr(const char *s, int base, uintmax_t *resultp)
{
        if (!strcmp(s, "null")) {
                *resultp = 0;
                return 0;
        }
        int ret;
        ret = str_to_uint(s, base, resultp);
        if (ret != 0) {
                return ret;
        }
        if (*resultp == 0) {
                *resultp = EXTERNREF_0;
        }
        return 0;
}

/* read something like: "aabbcc\n" */
int
read_hex_from_stdin(uint8_t *p, size_t left)
{
        char buf[3];
        size_t sz;
        while (left > 0) {
                sz = fread(buf, 2, 1, stdin);
                if (sz == 0) {
                        return EIO;
                }
                buf[2] = 0;
                uintmax_t v;
#if defined(__GNUC__) && !defined(__clang__)
                v = 0;
#endif
                int ret = str_to_uint(buf, 16, &v);
                if (ret != 0) {
                        return ret;
                }
                *p++ = (uint8_t)v;
                left--;
        }
        sz = fread(buf, 1, 1, stdin);
        if (sz == 0) {
                return EIO;
        }
        if (buf[0] != '\n') {
                return EPROTO;
        }
        return 0;
}

static void
repl_unload(struct repl_module_state *mod)
{
        if (mod->inst != NULL) {
                instance_destroy(mod->inst);
                mod->inst = NULL;
        }
        if (mod->module != NULL) {
                module_destroy(mod->module);
                mod->module = NULL;
        }
        if (mod->buf != NULL) {
                if (mod->buf_mapped) {
                        unmap_file(mod->buf, mod->bufsize);
                } else {
                        free(mod->buf);
                }
                mod->buf = NULL;
        }
        if (mod->name != NULL) {
                free(mod->name);
                mod->name = NULL;
        }
#if defined(TOYWASM_ENABLE_WASI_THREADS)
        if (mod->extra_import != NULL) {
                import_object_destroy(mod->extra_import);
                mod->extra_import = NULL;
        }
#endif
}

static void
repl_unload_u(struct repl_state *state, struct repl_module_state_u *mod_u)
{
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                struct dyld *d = &mod_u->u.dyld;
                dyld_clear(d);
                return;
        }
#endif
        struct repl_module_state *mod = &mod_u->u.repl;
        repl_unload(mod);
}

void
toywasm_repl_reset(struct repl_state *state)
{
        uint32_t n = 0;
        while (state->imports != NULL) {
                struct import_object *im = state->imports;
                state->imports = im->next;
                import_object_destroy(im);
                n++;
        }
        while (state->nregister > 0) {
                struct registered_name *rname = state->registered_names;
                assert(rname != NULL);
                state->registered_names = rname->next;
                state->nregister--;
                free((void *)rname->name.data);
                free(rname);
                n--;
        }
        while (state->nmodules > 0) {
                repl_unload_u(state, &state->modules[--state->nmodules]);
        }
        free(state->param);
        state->param = NULL;
        free(state->result);
        state->result = NULL;
        free(state->modules);
        state->modules = NULL;
        assert(state->registered_names == NULL);

#if defined(TOYWASM_ENABLE_WASI_THREADS)
        if (state->wasi_threads != NULL) {
                wasi_threads_instance_destroy(state->wasi_threads);
                state->wasi_threads = NULL;
                n--;
        }
#endif
#if defined(TOYWASM_ENABLE_WASI)
        if (state->wasi != NULL) {
                wasi_instance_destroy(state->wasi);
                state->wasi = NULL;
                n--;
        }
#endif
        assert(n == 0);
}

int
toywasm_repl_load_wasi(struct repl_state *state)
{
#if defined(TOYWASM_ENABLE_WASI)
        if (state->wasi != NULL) {
                xlog_error("wasi is already loaded");
                return EPROTO;
        }
        int ret;
        ret = wasi_instance_create(&state->wasi);
        if (ret != 0) {
                goto fail;
        }
        ret = wasi_instance_populate_stdio_with_hostfd(state->wasi);
        if (ret != 0) {
                goto undo_wasi_create;
        }
        struct import_object *im;
        ret = import_object_create_for_wasi(state->wasi, &im);
        if (ret != 0) {
                goto undo_wasi_create;
        }
        im->next = state->imports;
        state->imports = im;
#endif
#if defined(TOYWASM_ENABLE_WASI_THREADS)
        assert(state->wasi_threads == NULL);
        ret = wasi_threads_instance_create(&state->wasi_threads);
        if (ret != 0) {
                goto undo_wasi;
        }
        ret = import_object_create_for_wasi_threads(state->wasi_threads, &im);
        if (ret != 0) {
                goto undo_wasi_threads_create;
        }
        im->next = state->imports;
        state->imports = im;
#endif
        return 0;

#if defined(TOYWASM_ENABLE_WASI_THREADS)
undo_wasi_threads_create:
        wasi_threads_instance_destroy(state->wasi_threads);
        state->wasi_threads = NULL;
undo_wasi:
        assert(state->wasi != NULL);
        im = state->imports;
        state->imports = im->next;
        import_object_destroy(im);
#endif
#if defined(TOYWASM_ENABLE_WASI)
undo_wasi_create:
        wasi_instance_destroy(state->wasi);
        state->wasi = NULL;
fail:
        xlog_error("failed to load wasi");
        return ret;
#endif
}

#if defined(TOYWASM_ENABLE_WASI)
int
toywasm_repl_set_wasi_args(struct repl_state *state, int argc,
                           const char *const *argv)
{
        if (state->wasi == NULL) {
                return EPROTO;
        }
        wasi_instance_set_args(state->wasi, argc, argv);
        return 0;
}

int
toywasm_repl_set_wasi_environ(struct repl_state *state, int nenvs,
                              const char *const *envs)
{
        if (state->wasi == NULL) {
                return EPROTO;
        }
        wasi_instance_set_environ(state->wasi, nenvs, envs);
        return 0;
}

int
toywasm_repl_set_wasi_prestat(struct repl_state *state, const char *path)
{
        if (state->wasi == NULL) {
                return EPROTO;
        }
        return wasi_instance_prestat_add(state->wasi, path);
}

int
toywasm_repl_set_wasi_prestat_mapdir(struct repl_state *state,
                                     const char *path)
{
        if (state->wasi == NULL) {
                return EPROTO;
        }
        return wasi_instance_prestat_add_mapdir(state->wasi, path);
}
#endif

int
find_mod_u(struct repl_state *state, const char *modname,
           struct repl_module_state_u **modp)
{
        if (state->nmodules == 0) {
                xlog_printf("no module loaded\n");
                return EPROTO;
        }
        if (modname == NULL) {
                *modp = &state->modules[state->nmodules - 1];
                return 0;
        }
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                return ENOTSUP;
        }
#endif
        uint32_t i;
        for (i = 0; i < state->nmodules; i++) {
                struct repl_module_state_u *mod_u = &state->modules[i];
                struct repl_module_state *mod = &mod_u->u.repl;
                if (mod->name != NULL && !strcmp(modname, mod->name)) {
                        *modp = mod_u;
                        return 0;
                }
        }
        return ENOENT;
}

int
find_mod(struct repl_state *state, const char *modname,
         struct repl_module_state **modp)
{
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                return ENOTSUP;
        }
#endif
        struct repl_module_state_u *mod_u;
        int ret = find_mod_u(state, modname, &mod_u);
        if (ret != 0) {
                return ret;
        }
        *modp = &mod_u->u.repl;
        return 0;
}

void
print_trap(const struct exec_context *ctx, const struct trap_info *trap)
{
        /* the messages here are aimed to match assert_trap in wast */
        enum trapid id = trap->trapid;
        const char *msg = "unknown";
        const char *trapmsg = ctx->report->msg;
        switch (id) {
        case TRAP_DIV_BY_ZERO:
                msg = "integer divide by zero";
                break;
        case TRAP_INTEGER_OVERFLOW:
                msg = "integer overflow";
                break;
        case TRAP_OUT_OF_BOUNDS_MEMORY_ACCESS:
        case TRAP_OUT_OF_BOUNDS_DATA_ACCESS:
                msg = "out of bounds memory access";
                break;
        case TRAP_OUT_OF_BOUNDS_TABLE_ACCESS:
        case TRAP_OUT_OF_BOUNDS_ELEMENT_ACCESS:
                msg = "out of bounds table access";
                break;
        case TRAP_CALL_INDIRECT_NULL_FUNCREF:
                msg = "uninitialized element";
                break;
        case TRAP_TOO_MANY_FRAMES:
        case TRAP_TOO_MANY_STACKCELLS:
                msg = "stack overflow";
                break;
        case TRAP_CALL_INDIRECT_OUT_OF_BOUNDS_TABLE_ACCESS:
                msg = "undefined element";
                break;
        case TRAP_CALL_INDIRECT_FUNCTYPE_MISMATCH:
                msg = "indirect call type mismatch";
                break;
        case TRAP_UNREACHABLE:
                msg = "unreachable executed";
                break;
        case TRAP_INVALID_CONVERSION_TO_INTEGER:
                msg = "invalid conversion to integer";
                break;
        case TRAP_ATOMIC_WAIT_ON_NON_SHARED_MEMORY:
                msg = "expected shared memory";
                break;
        case TRAP_UNALIGNED_ATOMIC_OPERATION:
                msg = "unaligned atomic";
                break;
        default:
                break;
        }
        if (trapmsg == NULL) {
                trapmsg = "no message";
        }
        nbio_printf("Error: [trap] %s (%u): %s\n", msg, id, trapmsg);
}

static int
repl_exec_init(struct repl_module_state *mod, bool trap_ok)
{
        struct exec_context ctx0;
        struct exec_context *ctx = &ctx0;
        int ret;
        exec_context_init(ctx, mod->inst);
        ret = instance_create_execute_init(mod->inst, ctx);
        if (ret == ETOYWASMTRAP) {
                assert(ctx->trapped);
                print_trap(ctx, &ctx->trap);
                if (trap_ok) {
                        ret = 0;
                }
        }
        exec_context_clear(ctx);
        return ret;
}

static int
repl_load_from_buf(struct repl_state *state, const char *modname,
                   struct repl_module_state *mod, bool trap_ok)
{
        int ret;
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                return ENOTSUP;
        }
#endif
        struct load_context ctx;
        load_context_init(&ctx);
        ctx.options = state->opts.load_options;
        ret = module_create(&mod->module, mod->buf, mod->buf + mod->bufsize,
                            &ctx);
        if (ctx.report.msg != NULL) {
                xlog_error("load/validation error: %s", ctx.report.msg);
                nbio_printf("load/validation error: %s\n", ctx.report.msg);
        } else if (ret != 0) {
                nbio_printf("load/validation error: no message\n");
        }
        load_context_clear(&ctx);
        if (ret != 0) {
                xlog_printf("module_load failed\n");
                goto fail;
        }

        struct import_object *imports = state->imports;
#if defined(TOYWASM_ENABLE_WASI_THREADS)
        if (state->wasi_threads != NULL) {
                assert(mod->extra_import == NULL);
                /* create matching shared memory automatically */
                struct import_object *imo;
                ret = create_satisfying_shared_memories(mod->module, &imo);
                if (ret != 0) {
                        goto fail;
                }
                mod->extra_import = imo;
                imo->next = imports;
                imports = imo;
        }
#endif

        struct report report;
        report_init(&report);
        ret = instance_create_no_init(mod->module, &mod->inst, imports,
                                      &report);
        if (report.msg != NULL) {
                xlog_error("instance_create: %s", report.msg);
                nbio_printf("instantiation error: %s\n", report.msg);
        } else if (ret != 0) {
                nbio_printf("instantiation error: no message\n");
        }
        report_clear(&report);
        if (ret != 0) {
                xlog_printf("instance_create_no_init failed with %d\n", ret);
                goto fail;
        }
        ret = repl_exec_init(mod, trap_ok);
        if (ret != 0) {
                xlog_printf("repl_exec_init failed\n");
                goto fail;
        }
        if (modname != NULL) {
                mod->name = strdup(modname);
                if (mod->name == NULL) {
                        ret = ENOMEM;
                        goto fail;
                }
        }
        ret = 0;
fail:
        return ret;
}

int
toywasm_repl_load(struct repl_state *state, const char *modname,
                  const char *filename)
{
        int ret;
        ret = resize_array((void **)&state->modules, sizeof(*state->modules),
                           state->nmodules + 1);
        if (ret != 0) {
                return ret;
        }
        struct repl_module_state_u *mod_u = &state->modules[state->nmodules];
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                struct dyld *d = &mod_u->u.dyld;
                dyld_init(d);
                d->opts = state->opts.dyld_options;
                d->opts.base_import_obj = state->imports;
                ret = dyld_load(d, filename);
                if (ret != 0) {
                        return ret;
                }
                state->nmodules++;
                return 0;
        }
#endif
        struct repl_module_state *mod = &mod_u->u.repl;
        memset(mod, 0, sizeof(*mod));
        ret = map_file(filename, (void **)&mod->buf, &mod->bufsize);
        if (ret != 0) {
                xlog_error("failed to map %s (error %d)", filename, ret);
                goto fail;
        }
        mod->buf_mapped = true;
        ret = repl_load_from_buf(state, modname, mod, true);
        if (ret != 0) {
                goto fail;
        }
        state->nmodules++;
        return 0;
fail:
        repl_unload(mod);
        return ret;
}

int
toywasm_repl_load_hex(struct repl_state *state, const char *modname,
                      const char *opt)
{
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                return ENOTSUP;
        }
#endif
        int ret;
        ret = resize_array((void **)&state->modules, sizeof(*state->modules),
                           state->nmodules + 1);
        if (ret != 0) {
                return ret;
        }
        struct repl_module_state_u *mod_u = &state->modules[state->nmodules];
        struct repl_module_state *mod = &mod_u->u.repl;
        memset(mod, 0, sizeof(*mod));
        size_t sz = atoi(opt);
        mod->bufsize = sz;
        mod->buf = malloc(mod->bufsize);
        if (mod->buf == NULL) {
                ret = ENOMEM;
                goto fail;
        }
        mod->buf_mapped = false;
        xlog_printf("reading %zu bytes from stdin\n", mod->bufsize);
        ret = read_hex_from_stdin(mod->buf, mod->bufsize);
        if (ret != 0) {
                xlog_printf("failed to read module from stdin\n");
                goto fail;
        }
        ret = repl_load_from_buf(state, modname, mod, true);
        if (ret != 0) {
                goto fail;
        }
        state->nmodules++;
        return 0;
fail:
        repl_unload(mod);
        return ret;
}

static int
repl_save(struct repl_state *state, const char *modname, const char *filename)
{
#if defined(TOYWASM_ENABLE_WRITER)
        if (state->nmodules == 0) {
                return EPROTO;
        }
        struct repl_module_state *mod;
        int ret;
        ret = find_mod(state, modname, &mod);
        if (ret != 0) {
                goto fail;
        }
        ret = module_write(filename, mod->module);
        if (ret != 0) {
                xlog_error("failed to write module %s (error %d)", filename,
                           ret);
                goto fail;
        }
        ret = 0;
fail:
        return ret;
#else
        return ENOTSUP;
#endif
}

int
toywasm_repl_register(struct repl_state *state, const char *modname,
                      const char *register_name)
{
        int ret;
        if (state->nmodules == 0) {
                return EPROTO;
        }
        struct repl_module_state *mod;
        ret = find_mod(state, modname, &mod);
        if (ret != 0) {
                goto fail;
        }
        struct instance *inst = mod->inst;
        assert(inst != NULL);
        struct import_object *im;
        char *register_modname1 = strdup(register_name);
        struct registered_name *rname = malloc(sizeof(*rname));
        if (register_modname1 == NULL || rname == NULL) {
                free(rname);
                free(register_modname1);
                ret = ENOMEM;
                goto fail;
        }
        struct name *name = &rname->name;
        set_name_cstr(name, register_modname1);
        ret = import_object_create_for_exports(inst, name, &im);
        if (ret != 0) {
                free(rname);
                free(register_modname1);
                goto fail;
        }
        im->next = state->imports;
        state->imports = im;
        rname->next = state->registered_names;
        state->registered_names = rname;
        state->nregister++;
        ret = 0;
fail:
        return ret;
}

#if defined(TOYWASM_ENABLE_WASM_SIMD)
int
parse_v128(const char *s, struct val *result)
{
        uint64_t upper;
        uint64_t lower;
        int ret;
        if (strlen(s) != 32) {
                return EINVAL;
        }
        ret = sscanf(s, "%016" SCNx64 "%016" SCNx64, &upper, &lower);
        if (ret != 2) {
                return EINVAL;
        }
        le64_encode(&result->u.v128.i64[1], upper);
        le64_encode(&result->u.v128.i64[0], lower);
        return 0;
}
#endif

int
arg_conv(enum valtype type, const char *s, struct val *result)
{
        uintmax_t u;
        int ret;
#if defined(__GNUC__) && !defined(__clang__)
        u = 0;
#endif
        memset(result, 0, sizeof(*result));
        switch (type) {
        case TYPE_i32:
        case TYPE_f32:
                ret = str_to_uint(s, 0, &u);
                if (ret == 0) {
                        result->u.i32 = u;
                }
                break;
        case TYPE_i64:
        case TYPE_f64:
                ret = str_to_uint(s, 0, &u);
                if (ret == 0) {
                        result->u.i64 = u;
                }
                break;
#if defined(TOYWASM_ENABLE_WASM_SIMD)
        case TYPE_v128:
                ret = parse_v128(s, result);
                break;
#endif
        case TYPE_FUNCREF:
                ret = str_to_ptr(s, 0, &u);
                if (ret != 0) {
                        break;
                }
                if (u > UINTPTR_MAX) {
                        ret = EINVAL;
                        break;
                }
                result->u.funcref.func = (void *)(uintptr_t)u;
                break;
        case TYPE_EXTERNREF:
                ret = str_to_ptr(s, 0, &u);
                if (ret != 0) {
                        break;
                }
                if (u > UINTPTR_MAX) {
                        ret = EINVAL;
                        break;
                }
                result->u.externref = (void *)(uintptr_t)u;
                break;
        default:
                xlog_printf("arg_conv: unimplementd type %02x\n", type);
                ret = ENOTSUP;
                break;
        }
        return ret;
}

static int
repl_print_result(const struct resulttype *rt, const struct val *vals)
{
        const char *sep = "";
        uint32_t i;
        int ret = 0;
        if (rt->ntypes == 0) {
                nbio_printf("Result: <Empty Stack>\n");
                return 0;
        }
        nbio_printf("Result: ");
        for (i = 0; i < rt->ntypes; i++) {
                enum valtype type = rt->types[i];
                const struct val *val = &vals[i];
                switch (type) {
                case TYPE_i32:
                        nbio_printf("%s%" PRIu32 ":i32", sep, val->u.i32);
                        break;
                case TYPE_f32:
                        nbio_printf("%s%" PRIu32 ":f32", sep, val->u.i32);
                        break;
                case TYPE_i64:
                        nbio_printf("%s%" PRIu64 ":i64", sep, val->u.i64);
                        break;
                case TYPE_f64:
                        nbio_printf("%s%" PRIu64 ":f64", sep, val->u.i64);
                        break;
#if defined(TOYWASM_ENABLE_WASM_SIMD)
                case TYPE_v128:
                        nbio_printf("%s%016" PRIx64 "%016" PRIx64 ":v128", sep,
                                    le64_decode(&val->u.v128.i64[1]),
                                    le64_decode(&val->u.v128.i64[0]));
                        break;
#endif
                case TYPE_FUNCREF:
                        if (val->u.funcref.func == NULL) {
                                nbio_printf("%snull:funcref", sep);
                        } else {
                                nbio_printf("%s%" PRIuPTR ":funcref", sep,
                                            (uintptr_t)val->u.funcref.func);
                        }
                        break;
                case TYPE_EXTERNREF:
                        if ((uintptr_t)val->u.externref == EXTERNREF_0) {
                                nbio_printf("%s0:externref", sep);
                        } else if (val->u.externref == NULL) {
                                nbio_printf("%snull:externref", sep);
                        } else {
                                nbio_printf("%s%" PRIuPTR ":externref", sep,
                                            (uintptr_t)val->u.externref);
                        }
                        break;
                default:
                        xlog_printf("print_result: unimplementd type %02x\n",
                                    type);
                        ret = ENOTSUP;
                        break;
                }
                sep = ", ";
        }
        nbio_printf("\n");
        return ret;
}

int
unescape(char *p0, size_t *lenp)
{
        /*
         * unescape string like "\xe1\xba\x9b" in-place.
         *
         * Note: quote support here is an incomplete hack to allow
         * passing an empty name ("") on the repl prompt. (the spec
         * test has a case to examine zero-length name, which is
         * spec-wise valid.)
         * Because repl itself uses simple strtok to parse the input,
         * things like "a b" don't work on the prompt as you might
         * expect. "a\x20b" can work better.
         */
        bool in_quote = false;
        char *p = p0;
        char *wp = p;
        while (*p != 0) {
                if (in_quote) {
                        if (p[0] == '"') {
                                in_quote = false;
                                p++;
                                continue;
                        }
                } else {
                        if (p[0] == '"') {
                                in_quote = true;
                                p++;
                                continue;
                        }
                }
                if (p[0] == '\\') {
                        if (p[1] == 'x') {
                                p += 2;
                                char buf[3];
                                if ((buf[0] = *p++) == 0) {
                                        return EINVAL;
                                }
                                if ((buf[1] = *p++) == 0) {
                                        return EINVAL;
                                }
                                buf[2] = 0;
                                uintmax_t v;
#if defined(__GNUC__) && !defined(__clang__)
                                v = 0;
#endif
                                int ret = str_to_uint(buf, 16, &v);
                                if (ret != 0) {
                                        return ret;
                                }
                                *wp++ = (char)v;
                        } else {
                                return EINVAL;
                        }
                } else {
                        *wp++ = *p++;
                }
        }
        if (in_quote) {
                return EINVAL;
        }
        *lenp = wp - p0;
        *wp++ = 0;
        return 0;
}

static int
exec_func(struct exec_context *ctx, uint32_t funcidx,
          const struct resulttype *ptype, const struct resulttype *rtype,
          const struct val *param, struct val *result, int timeout_ms,
          const struct trap_info **trapp)
{
        struct timespec abstimeout;
        int ret;
        *trapp = NULL;
        if (timeout_ms > 0) {
                /*
                 * REVISIT: this timeout logic is a bit broken because it
                 * assumes that, when the main thread exits, other threads
                 * also exit soon.
                 * right now, it happens to be true because
                 * wasi_threads_complete_exec terminates other threads
                 * for proc_exit anyway.
                 * (see the comment in wasi_threads_instance_join.)
                 *
                 * possible fixes:
                 * a. make wasi_threads_instance_join check timeout expiration
                 * b. make the user interrutpt a cluster-wide event and handle
                 *    it in non-main threads as well
                 * c. give up implementing a timeout this way
                 */

                const static atomic_uint one = 1;
                ret = abstime_from_reltime_ms(CLOCK_MONOTONIC, &abstimeout,
                                              timeout_ms);
                if (ret != 0) {
                        goto fail;
                }
                /*
                 * keep the interrupt triggered so that we can check
                 * timeout in the execution loop below.
                 *
                 * Note: if a host environment has a nice timer functionality
                 * like alarm(3), you can make this more efficient by
                 * requesting an interrupt only after timeout_ms. we don't
                 * bother to make such an optimization here though because
                 * we aim to be portable to wasm32-wasi, which doesn't have
                 * signals.
                 */
                ctx->intrp = &one;
                /*
                 * a hack to avoid busy loop.
                 * maybe it's cleaner for us to provide a timer functionality
                 * by ourselves. but i feel it's too much for now.
                 */
                ctx->user_intr_delay = 1;
        }
        assert(ctx->stack.lsize == 0);
        ret = exec_push_vals(ctx, ptype, param);
        if (ret != 0) {
                goto fail;
        }
        ret = instance_execute_func(ctx, funcidx, ptype, rtype);
        do {
                if (ret == ETOYWASMUSERINTERRUPT) {
                        struct timespec now;
                        int ret1;
                        assert(timeout_ms > 0);
                        ret1 = timespec_now(CLOCK_MONOTONIC, &now);
                        if (ret1 != 0) {
                                ret = ret1;
                                goto fail;
                        }
                        if (timespec_cmp(&now, &abstimeout) > 0) {
                                xlog_error("execution timed out");
                                ret = ETIMEDOUT;
                                goto fail;
                        }
                }
                ret = instance_execute_handle_restart_once(ctx, ret);
        } while (IS_RESTARTABLE(ret));
        if (ret == ETOYWASMTRAP) {
                assert(ctx->trapped);
                const struct trap_info *trap = &ctx->trap;
                *trapp = trap;
        } else if (ret == 0) {
                exec_pop_vals(ctx, rtype, result);
                assert(ctx->stack.lsize == 0);
        }
fail:
        return ret;
}

/*
 * "cmd" is like "add 1 2"
 */
int
toywasm_repl_invoke(struct repl_state *state, const char *modname,
                    const char *cmd, int timeout_ms, uint32_t *exitcodep,
                    bool print_result)
{
        char *cmd1 = strdup(cmd);
        if (cmd1 == NULL) {
                return ENOMEM;
        }
        int ret;
        /* TODO handle quote */
        char *funcname = strtok(cmd1, " ");
        if (funcname == NULL) {
                xlog_printf("no func name\n");
                ret = EPROTO;
                goto fail;
        }
        xlog_trace("repl: invoke func %s", funcname);
        size_t len;
        ret = unescape(funcname, &len);
        if (ret != 0) {
                xlog_error("failed to unescape funcname");
                goto fail;
        }
        struct name funcname_name;
        funcname_name.data = funcname;
        funcname_name.nbytes = len;
        struct repl_module_state_u *mod_u;
        ret = find_mod_u(state, modname, &mod_u);
        if (ret != 0) {
                goto fail;
        }
        struct instance *inst;
#if defined(TOYWASM_ENABLE_DYLD)
        if (state->opts.enable_dyld) {
                struct dyld *d = &mod_u->u.dyld;
                inst = dyld_main_object_instance(d);
        } else
#endif
        {
                struct repl_module_state *mod = &mod_u->u.repl;
                inst = mod->inst;
        }
        const struct module *module = inst->module;
        assert(inst != NULL);
        assert(module != NULL);
        uint32_t funcidx;
        ret = module_find_export_func(module, &funcname_name, &funcidx);
        if (ret != 0) {
                /* TODO should print the name w/o unescape */
                xlog_error("module_find_export_func failed for %s", funcname);
                goto fail;
        }
        const struct functype *ft = module_functype(module, funcidx);
        const struct resulttype *ptype = &ft->parameter;
        const struct resulttype *rtype = &ft->result;
        ret = ARRAY_RESIZE(state->param, ptype->ntypes);
        if (ret != 0) {
                goto fail;
        }
        ret = ARRAY_RESIZE(state->result, rtype->ntypes);
        if (ret != 0) {
                goto fail;
        }
        struct val *param = state->param;
        struct val *result = state->result;
        uint32_t i;
        for (i = 0; i < ptype->ntypes; i++) {
                char *arg = strtok(NULL, " ");
                if (arg == NULL) {
                        xlog_printf("missing arg\n");
                        ret = EPROTO;
                        goto fail;
                }
                ret = arg_conv(ptype->types[i], arg, &param[i]);
                if (ret != 0) {
                        xlog_printf("arg_conv failed\n");
                        goto fail;
                }
        }
        if (strtok(NULL, " ") != NULL) {
                xlog_printf("extra arg\n");
                ret = EPROTO;
                goto fail;
        }
        struct exec_context ctx0;
        struct exec_context *ctx = &ctx0;
        exec_context_init(ctx, inst);
        ctx->options = state->opts.exec_options;
        const struct trap_info *trap;
#if defined(TOYWASM_ENABLE_WASI_THREADS)
        struct wasi_threads_instance *wasi_threads = state->wasi_threads;
        wasi_threads_setup_exec_context(wasi_threads, ctx);
#endif
        ret = exec_func(ctx, funcidx, ptype, rtype, param, result, timeout_ms,
                        &trap);
#if defined(TOYWASM_ENABLE_WASI_THREADS)
        wasi_threads_complete_exec(wasi_threads, &trap);
#endif
        if (state->opts.print_stats) {
                exec_context_print_stats(ctx);
                instance_print_stats(inst);
        }

        if (ret == ETOYWASMTRAP) {
                assert(trap != NULL);
#if defined(TOYWASM_ENABLE_WASI)
                if (trap->trapid == TRAP_VOLUNTARY_EXIT) {
                        struct wasi_instance *wasi = state->wasi;
                        /* Note: TRAP_VOLUNTARY_EXIT is only used by wasi */
                        assert(wasi != NULL);
                        uint32_t exit_code = wasi_instance_exit_code(wasi);
                        xlog_trace("voluntary exit (%" PRIu32 ")", exit_code);
                        if (exitcodep != NULL) {
                                *exitcodep = exit_code;
                                ret = 0;
                        } else {
                                ret = exit_code;
                        }
                        exec_context_clear(ctx);
                        goto fail;
                }
#endif
                print_trap(ctx, trap);
        }
        exec_context_clear(ctx);
        if (ret != 0) {
                xlog_printf("instance_execute_func failed with %d\n", ret);
                goto fail;
        }
        if (print_result) {
                ret = repl_print_result(rtype, result);
                if (ret != 0) {
                        xlog_printf("print_result failed\n");
                        goto fail;
                }
        }
        if (exitcodep != NULL) {
                *exitcodep = 0;
        }
        ret = 0;
fail:
        free(cmd1);
        return ret;
}

static int
repl_global_get(struct repl_state *state, const char *modname,
                const char *name_cstr)
{
        char *name1 = strdup(name_cstr);
        if (name1 == NULL) {
                return ENOMEM;
        }
        size_t len;
        int ret;
        ret = unescape(name1, &len);
        if (ret != 0) {
                xlog_error("failed to unescape name");
                goto fail;
        }
        struct name name;
        name.data = name1;
        name.nbytes = len;
        struct repl_module_state *mod;
        ret = find_mod(state, modname, &mod);
        if (ret != 0) {
                goto fail;
        }
        struct instance *inst = mod->inst;
        struct module *module = mod->module;
        assert(inst != NULL);
        assert(module != NULL);
        uint32_t idx;
        ret = module_find_export(module, &name, EXTERNTYPE_GLOBAL, &idx);
        if (ret != 0) {
                xlog_error("module_find_export failed for %s", name_cstr);
                goto fail;
        }
        const struct globaltype *gt = module_globaltype(module, idx);
        enum valtype type = gt->t;
        const struct resulttype rtype = {
                .types = &type,
                .ntypes = 1,
        };
        struct val val = VEC_ELEM(inst->globals, idx)->val;
        ret = repl_print_result(&rtype, &val);
        if (ret != 0) {
                xlog_printf("print_result failed\n");
                goto fail;
        }
        ret = 0;
fail:
        free(name1);
        return ret;
}

void
toywasm_repl_print_version(void)
{
        nbio_printf("toywasm %s\n", TOYWASM_VERSION);
#if defined(__clang_version__)
        nbio_printf("__clang_version__ = %s\n", __clang_version__);
#endif
#if !defined(__clang__)
#if defined(__GNUC__)
        nbio_printf("__GNUC__ = %u\n", __GNUC__);
#endif
#if defined(__GNUC_MINOR__)
        nbio_printf("__GNUC_MINOR__ = %u\n", __GNUC_MINOR__);
#endif
#if defined(__GNUC_PATCHLEVEL__)
        nbio_printf("__GNUC_PATCHLEVEL__ = %u\n", __GNUC_PATCHLEVEL__);
#endif
#endif /* !defined(__clang__) */
#if defined(__BYTE_ORDER__)
        nbio_printf("__BYTE_ORDER__ is %u (__ORDER_LITTLE_ENDIAN__ is %u)\n",
                    __BYTE_ORDER__, __ORDER_LITTLE_ENDIAN__);
#endif
        nbio_printf("sizeof(void *) = %zu\n", sizeof(void *));
#if defined(__wasi__)
        nbio_printf("__wasi__ defined\n");
#endif
#if defined(__x86_64__)
        nbio_printf("__x86_64__ defined\n");
#endif
#if defined(__aarch64__)
        nbio_printf("__aarch64__ defined\n");
#endif
#if defined(__arm__)
        nbio_printf("__arm__ defined\n");
#endif
#if defined(__ppc__)
        nbio_printf("__ppc__ defined\n");
#endif
#if defined(__riscv)
        nbio_printf("__riscv defined\n");
#endif
#if defined(__s390x__)
        nbio_printf("__s390x__ defined\n");
#endif
#if defined(__s390__)
        nbio_printf("__s390__ defined\n");
#endif
#if defined(__wasm__)
        nbio_printf("__wasm__ defined\n");
#endif
#if defined(__wasm32__)
        nbio_printf("__wasm32__ defined\n");
#endif
#if defined(__wasm64__)
        nbio_printf("__wasm64__ defined\n");
#endif
#if defined(__APPLE__)
        nbio_printf("__APPLE__ defined\n");
#endif
#if defined(__NuttX__)
        nbio_printf("__NuttX__ defined\n");
#endif
#if defined(__linux__)
        nbio_printf("__linux__ defined\n");
#endif
        extern const char *toywasm_config_string;
        nbio_printf("Build-time options:\n%s", toywasm_config_string);
}

static int
repl_module_subcmd(struct repl_state *state, const char *cmd,
                   const char *modname, const char *opt)
{
        int ret;

        if (!strcmp(cmd, "load") && opt != NULL) {
                ret = toywasm_repl_load(state, modname, opt);
                if (ret != 0) {
                        goto fail;
                }
        } else if (!strcmp(cmd, "load-hex") && opt != NULL) {
                ret = toywasm_repl_load_hex(state, modname, opt);
                if (ret != 0) {
                        goto fail;
                }
        } else if (!strcmp(cmd, "invoke") && opt != NULL) {
                ret = toywasm_repl_invoke(state, modname, opt, -1, NULL, true);
                if (ret != 0) {
                        goto fail;
                }
        } else if (!strcmp(cmd, "register") && opt != NULL) {
                ret = toywasm_repl_register(state, modname, opt);
                if (ret != 0) {
                        goto fail;
                }
        } else if (!strcmp(cmd, "save") && opt != NULL) {
                ret = repl_save(state, modname, opt);
                if (ret != 0) {
                        goto fail;
                }
        } else if (!strcmp(cmd, "global-get") && opt != NULL) {
                ret = repl_global_get(state, modname, opt);
                if (ret != 0) {
                        goto fail;
                }
        } else {
                xlog_printf("Error: unknown command %s\n", cmd);
                ret = 0;
        }
fail:
        return ret;
}

static void
repl_options_init(struct repl_options *opts)
{
        opts->prompt = "toywasm";
        opts->print_stats = false;
        load_options_set_defaults(&opts->load_options);
        exec_options_set_defaults(&opts->exec_options);
#if defined(TOYWASM_ENABLE_DYLD)
        dyld_options_set_defaults(&opts->dyld_options);
#endif
}

void
toywasm_repl_state_init(struct repl_state *state)
{
        memset(state, 0, sizeof(*state));
        repl_options_init(&state->opts);
}

int
toywasm_repl(struct repl_state *state)
{
        char *line = NULL;
        size_t linecap = 0;
        int ret;
        while (true) {
                nbio_printf("%s> ", state->opts.prompt);
                fflush(stdout);
                ret = nbio_getline(&line, &linecap, stdin);
                if (ret == -1) {
                        break;
                }
                xlog_printf("repl cmd '%s'\n", line);
                char *cmd = strtok(line, " \n");
                if (cmd == NULL) {
                        continue;
                }
                char *opt = strtok(NULL, "\n");
                if (!strcmp(cmd, ":version")) {
                        toywasm_repl_print_version();
                } else if (!strcmp(cmd, ":init")) {
                        toywasm_repl_reset(state);
                } else if (!strcmp(cmd, ":exit")) {
                        /*
                         * Note: an explicit exit command is useful
                         * where VEOF is not available. eg. nuttx
                         */
                        break;
                } else if (!strcmp(cmd, ":module") && opt != NULL) {
                        char *modname = strtok(opt, " ");
                        if (modname == NULL) {
                                ret = EPROTO;
                                goto fail;
                        }
                        char *subcmd = strtok(NULL, " ");
                        if (subcmd == NULL) {
                                ret = EPROTO;
                                goto fail;
                        }
                        opt = strtok(NULL, "");
                        ret = repl_module_subcmd(state, subcmd, modname, opt);
                        if (ret != 0) {
                                goto fail;
                        }
                } else if (*cmd == ':') {
                        ret = repl_module_subcmd(state, cmd + 1, NULL, opt);
                        if (ret != 0) {
                                goto fail;
                        }
                }
                continue;
fail:
                xlog_printf("repl fail with %d\n", ret);
                nbio_printf("Error: command '%s' failed with %d\n", cmd, ret);
        }
        free(line);
        toywasm_repl_reset(state);
        return 0;
}
