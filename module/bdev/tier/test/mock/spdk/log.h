/* Host-test mock of spdk/log.h (T1). */
#ifndef SPDK_LOG_MOCK_H
#define SPDK_LOG_MOCK_H

#include "spdk/stdinc.h"

#define SPDK_ERRLOG(...)    fprintf(stderr, "ERR: " __VA_ARGS__)
#define SPDK_WARNLOG(...)   fprintf(stderr, "WARN: " __VA_ARGS__)
#define SPDK_NOTICELOG(...) fprintf(stderr, "NOTICE: " __VA_ARGS__)

#endif
