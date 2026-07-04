/* Host-test mock of spdk/crc32.h (T1) — real software CRC32C (Castagnoli),
 * bit-reflected, same convention as SPDK's spdk_crc32c_update (init ~0,
 * NOT post-inverted). Any self-consistent implementation validates the
 * serialize/validate roundtrip; using the actual CRC32C polynomial keeps the
 * fixture bytes meaningful. */
#ifndef SPDK_CRC32_MOCK_H
#define SPDK_CRC32_MOCK_H

#include "spdk/stdinc.h"

static inline uint32_t
spdk_crc32c_update(const void *buf, size_t len, uint32_t crc)
{
	const uint8_t *p = buf;
	size_t i;
	int b;

	for (i = 0; i < len; i++) {
		crc ^= p[i];
		for (b = 0; b < 8; b++) {
			crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
		}
	}
	return crc;
}

#endif
