/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Evariops.
 *   All rights reserved.
 */

/* Minimal cross-module query surface: the raid module publishes per-member
 * epoch facts in its own get_bdevs output, which is what the control-plane
 * observes. No cbt internals are exposed here; the raid module is the only
 * intended consumer. Symbols resolve via --whole-archive at app link. */

#ifndef SPDK_VBDEV_CBT_QUERY_H
#define SPDK_VBDEV_CBT_QUERY_H

#include "spdk/stdinc.h"

struct vbdev_cbt_epoch_facts {
	/* Control-plane nonce; empty when the opener supplied none. */
	char		nonce[32];
	/* Static string: open|frozen|rebuilding|completed|invalid. */
	const char	*state;
	/* The bitmap does not cover the whole device (the base bdev grew): the
	 * delta is incomplete, only a full rebuild is safe. */
	bool		truncated;
};

/**
 * Fill \c out with the most recently opened live epoch of the cbt bdev named
 * \c bdev_name.
 *
 * \return 0 on success; -ENOENT if the cbt currently tracks no epoch;
 *         -ENODEV if \c bdev_name is not a cbt bdev. App thread only.
 */
int vbdev_cbt_query_latest_epoch(const char *bdev_name, struct vbdev_cbt_epoch_facts *out);

/**
 * Open a delta epoch bounding an unplanned member loss. The raid module calls
 * this on each surviving member's cbt when a member leaves, so the missing
 * writes stay a delta instead of degrading to a full rebuild. The generated
 * epoch id and nonce are reported through get_bdevs, which is how the
 * control-plane adopts the round.
 *
 * \return 0 on success; -EEXIST if an OPEN epoch already tracks the round
 *         (never take over implicitly); -ENODEV if \c bdev_name is not a cbt
 *         bdev; other negative errno from the epoch machinery. App thread only.
 */
int vbdev_cbt_auto_epoch_open(const char *bdev_name, const char *stale_backend_id);

#endif /* SPDK_VBDEV_CBT_QUERY_H */
