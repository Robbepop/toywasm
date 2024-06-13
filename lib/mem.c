#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "mem.h"

void
mem_context_init(struct mem_context *ctx)
{
        ctx->allocated = 0;
        ctx->limit = SIZE_MAX;
}

void
mem_context_clear(struct mem_context *ctx)
{
        assert(ctx->allocated == 0);
}

void *
mem_alloc(struct mem_context *ctx, size_t sz)
{
        void *p = malloc(sz);
        if (p == NULL) {
                return NULL;
        }
        return p;
}

void *
mem_zalloc(struct mem_context *ctx, size_t sz)
{
	void *p = mem_alloc(ctx, sz);
    if (p != NULL) {
        memset(p, 0, sz);
    }
    return p;
}

void *
mem_calloc(struct mem_context *ctx, size_t a, size_t b)
{
	size_t sz = a * b;
    if (sz / a != b) {
        return NULL;
    }
    return mem_zalloc(ctx, sz);
}


static int
mem_reserve(struct mem_context *ctx, size_t diff)
{
        size_t ov;
        size_t nv;
        do {
                ov = ctx->allocated;
                assert(ov <= ctx->limit);
                if (ctx->limit - ov < diff) {
                        return ENOMEM;
                }
                nv = ov + diff;
        } while (!atomic_compare_exchange_weak(&ctx->allocated, &ov, nv));
        return 0;
}

static void
mem_unreserve(struct mem_context *ctx, size_t diff)
{
        assert(ctx->allocated <= ctx->limit);
        assert(ctx->allocated >= diff);
        size_t ov = atomic_fetch_sub(&ctx->allocated, diff);
        assert(ctx->allocated >= ov);
}

void
mem_free(struct mem_context *ctx, void *p, size_t sz)
{
        free(p);
        mem_unreserve(ctx, sz);
}

void *
mem_resize(struct mem_context *ctx, void *p, size_t oldsz, size_t newsz)
{
        if (oldsz < newsz) {
                size_t diff = newsz - oldsz;
                if (mem_reserve(ctx, diff)) {
                        return NULL;
                }
        } else {
                size_t diff = oldsz - newsz;
                mem_unreserve(ctx, diff);
        }
        void *np = realloc(p, newsz);
        if (np == NULL) {
                return NULL;
        }
        return np;
}