/* Host-test mock of spdk/assert.h (T1). */
#ifndef SPDK_ASSERT_MOCK_H
#define SPDK_ASSERT_MOCK_H

#include <assert.h>

#define SPDK_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif
