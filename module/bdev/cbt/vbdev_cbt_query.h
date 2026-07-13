/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2026 Evariops.
 *   All rights reserved.
 */

/* Evariops 0014.6 (spdk-csi RPC-CONTRACT §3): minimal cross-module query
 * surface so the raid module can publish per-member epoch facts in its own
 * get_bdevs output — the control-plane's EpochObservation source. This header
 * deliberately exposes NO cbt internals; the raid patch (0017) is its only
 * intended consumer. Symbols resolve via --whole-archive at app link. */

#ifndef SPDK_VBDEV_CBT_QUERY_H
#define SPDK_VBDEV_CBT_QUERY_H

#include "spdk/stdinc.h"

struct vbdev_cbt_epoch_facts {
	/* CP-generated nonce (empty for pre-0014 epochs). */
	char		nonce[32];
	/* Static string: open|frozen|rebuilding|completed|invalid. */
	const char	*state;
	/* T-D6: the live bitmap does not cover a growth zone — delta is a lie. */
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

#endif /* SPDK_VBDEV_CBT_QUERY_H */
