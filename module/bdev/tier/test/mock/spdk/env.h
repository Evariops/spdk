/* Host-test mock of spdk/env.h (T1) — DMA alloc maps to plain heap. */
#ifndef SPDK_ENV_MOCK_H
#define SPDK_ENV_MOCK_H

#include "spdk/stdinc.h"

static inline void *
spdk_dma_zmalloc(size_t size, size_t align, uint64_t *phys_addr)
{
	(void)align;
	(void)phys_addr;
	return calloc(1, size);
}

static inline void *
spdk_dma_malloc(size_t size, size_t align, uint64_t *phys_addr)
{
	(void)align;
	(void)phys_addr;
	return malloc(size);
}

static inline void
spdk_dma_free(void *buf)
{
	free(buf);
}

#endif
