/*
 * https://github.com/WebAssembly/simd
 */

/*
 * MEM - num of bits in memory
 * STACK_TYPE - type on stack (eg. v128)
 * CP - CP(DST, SRC) to load and convert
 */
#define SIMD_LOADOP(NAME, MEM, STACK_TYPE, CP)                                \
        INSN_IMPL(NAME)                                                       \
        {                                                                     \
                const struct module *m = MODULE;                              \
                struct memarg memarg;                                         \
                int ret;                                                      \
                LOAD_PC;                                                      \
                READ_MEMARG(&memarg);                                         \
                CHECK(memarg.memidx < m->nimportedmems + m->nmems);           \
                CHECK(1 <= (MEM / 8) >>                                       \
                      memarg.align); /* 2 ** align <= N / 8 */                \
                POP_VAL(TYPE_i32, i);                                         \
                struct val val_c;                                             \
                if (EXECUTING) {                                              \
                        void *datap;                                          \
                        ret = memory_getptr(ECTX, memarg.memidx, val_i.u.i32, \
                                            memarg.offset, MEM / 8, &datap);  \
                        if (ret != 0) {                                       \
                                goto fail;                                    \
                        }                                                     \
                        CP(&val_c.u.STACK_TYPE, datap);                       \
                }                                                             \
                PUSH_VAL(TYPE_##STACK_TYPE, c);                               \
                SAVE_PC;                                                      \
                INSN_SUCCESS;                                                 \
fail:                                                                         \
                INSN_FAIL;                                                    \
        }

#define CP_V128(a, b) memcpy(a, b, 128 / 8)

#define EXTEND_s(B, S) (int##B##_t)(S)
#define EXTEND_u(B, S) (uint##B##_t)(S)

#define EXTEND1(S_OR_U, BDST, BSRC, a, b, I)                                  \
        le##BDST##_encode(                                                    \
                &(a)->i##BDST[I],                                             \
                EXTEND_##S_OR_U(BSRC, le##BSRC##_decode(&((                   \
                                              const uint##BSRC##_t *)b)[I])))

#define EXTEND1_8_s(a, b, I) EXTEND1(s, 16, 8, a, b, I)
#define EXTEND1_8_u(a, b, I) EXTEND1(u, 16, 8, a, b, I)
#define EXTEND1_16_s(a, b, I) EXTEND1(s, 32, 16, a, b, I)
#define EXTEND1_16_u(a, b, I) EXTEND1(u, 32, 16, a, b, I)
#define EXTEND1_32_s(a, b, I) EXTEND1(s, 64, 32, a, b, I)
#define EXTEND1_32_u(a, b, I) EXTEND1(u, 64, 32, a, b, I)

#define EXTEND_8x8_s(a, b) FOREACH_LANES(8, a, b, EXTEND1_8_s)
#define EXTEND_8x8_u(a, b) FOREACH_LANES(8, a, b, EXTEND1_8_u)
#define EXTEND_16x4_s(a, b) FOREACH_LANES(4, a, b, EXTEND1_16_s)
#define EXTEND_16x4_u(a, b) FOREACH_LANES(4, a, b, EXTEND1_16_u)
#define EXTEND_32x2_s(a, b) FOREACH_LANES(2, a, b, EXTEND1_32_s)
#define EXTEND_32x2_u(a, b) FOREACH_LANES(2, a, b, EXTEND1_32_u)

#define SPLAT1(LS, D, S, I) le##LS##_encode(&(D)->i##LS[I], S)

#define SPLAT1_8(D, S, I) SPLAT1(8, D, S, I)
#define SPLAT1_16(D, S, I) SPLAT1(16, D, S, I)
#define SPLAT1_32(D, S, I) SPLAT1(32, D, S, I)
#define SPLAT1_64(D, S, I) SPLAT1(64, D, S, I)

#define SPLAT_8(D, S) FOREACH_LANES(128 / 8, D, *(const uint8_t *)S, SPLAT1_8)
#define SPLAT_16(D, S)                                                        \
        FOREACH_LANES(128 / 16, D, *(const uint16_t *)S, SPLAT1_16)
#define SPLAT_32(D, S)                                                        \
        FOREACH_LANES(128 / 32, D, *(const uint32_t *)S, SPLAT1_32)
#define SPLAT_64(D, S)                                                        \
        FOREACH_LANES(128 / 64, D, *(const uint64_t *)S, SPLAT1_64)

#define FOREACH_LANES(NL, D, S, OP)                                           \
        do {                                                                  \
                unsigned int _i;                                              \
                for (_i = 0; _i < NL; _i++) {                                 \
                        OP(D, S, _i);                                         \
                }                                                             \
        } while (0)

SIMD_LOADOP(v128_load, 128, v128, CP_V128)
SIMD_LOADOP(v128_load8x8_s, 64, v128, EXTEND_8x8_s)
SIMD_LOADOP(v128_load8x8_u, 64, v128, EXTEND_8x8_u)
SIMD_LOADOP(v128_load16x4_s, 64, v128, EXTEND_16x4_s)
SIMD_LOADOP(v128_load16x4_u, 64, v128, EXTEND_16x4_u)
SIMD_LOADOP(v128_load32x2_s, 64, v128, EXTEND_32x2_s)
SIMD_LOADOP(v128_load32x2_u, 64, v128, EXTEND_32x2_u)
SIMD_LOADOP(v128_load8_splat, 8, v128, SPLAT_8)
SIMD_LOADOP(v128_load16_splat, 16, v128, SPLAT_16)
SIMD_LOADOP(v128_load32_splat, 32, v128, SPLAT_32)
SIMD_LOADOP(v128_load64_splat, 64, v128, SPLAT_64)

#define ZERO32(D, S)                                                          \
        le32_encode(&(D)->i32[0], *(const uint32_t *)(S));                    \
        (D)->i32[1] = 0;                                                      \
        (D)->i32[2] = 0;                                                      \
        (D)->i32[3] = 0

#define ZERO64(D, S)                                                          \
        le64_encode(&(D)->i64[0], *(const uint64_t *)(S));                    \
        (D)->i64[1] = 0

SIMD_LOADOP(v128_load32_zero, 32, v128, ZERO32)
SIMD_LOADOP(v128_load64_zero, 64, v128, ZERO64)
