/* Host-test mock of spdk/util.h (T1). */
#ifndef SPDK_UTIL_MOCK_H
#define SPDK_UTIL_MOCK_H

#include "spdk/stdinc.h"

#define SPDK_COUNTOF(a) (sizeof(a) / sizeof(*(a)))
#define spdk_divide_round_up(n, d) (((n) + (d) - 1) / (d))
#define spdk_min(a, b) (((a) < (b)) ? (a) : (b))
#define spdk_max(a, b) (((a) > (b)) ? (a) : (b))

#endif
