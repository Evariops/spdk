/* Host-test mock of spdk/stdinc.h (T1) — standard C only. */
#ifndef SPDK_STDINC_MOCK_H
#define SPDK_STDINC_MOCK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/queue.h>

/* glibc's sys/queue.h lacks a few BSD macros; provide what the tier code uses. */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar)		\
	for ((var) = TAILQ_FIRST((head));			\
	     (var) && ((tvar) = TAILQ_NEXT((var), field), 1);	\
	     (var) = (tvar))
#endif

#endif
