/* Host-test mock of spdk/string.h (T1). */
#ifndef SPDK_STRING_MOCK_H
#define SPDK_STRING_MOCK_H

#include "spdk/stdinc.h"

static inline const char *
spdk_strerror(int errnum)
{
	return strerror(errnum);
}

static inline bool
spdk_mem_all_zero(const void *data, size_t size)
{
	const uint8_t *p = data;

	while (size--) {
		if (*p++ != 0) {
			return false;
		}
	}
	return true;
}

#endif
