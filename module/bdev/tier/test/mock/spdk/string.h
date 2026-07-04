/* Host-test mock of spdk/string.h (T1). */
#ifndef SPDK_STRING_MOCK_H
#define SPDK_STRING_MOCK_H

#include "spdk/stdinc.h"

static inline const char *
spdk_strerror(int errnum)
{
	return strerror(errnum);
}

#endif
