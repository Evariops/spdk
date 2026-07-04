/* Host-test mock of spdk/bdev.h (T1) — just enough surface for vbdev_tier.h and
 * vbdev_tier_sb.c to compile. I/O entry points are declared here and defined by
 * the test harness (test_tier_sb.c), which records or fails them as the
 * scenario requires. */
#ifndef SPDK_BDEV_MOCK_H
#define SPDK_BDEV_MOCK_H

#include "spdk/stdinc.h"

struct spdk_io_channel;
struct spdk_bdev_io;
struct spdk_bdev_fn_table;
struct spdk_bdev_module;
struct spdk_bdev_desc;
struct spdk_json_write_ctx;

struct spdk_bdev {
	char				*name;
	const char			*product_name;
	uint32_t			blocklen;
	uint64_t			blockcnt;
	int				write_cache;
	void				*ctxt;
	const struct spdk_bdev_fn_table	*fn_table;
	struct spdk_bdev_module		*module;
};

typedef void (*spdk_bdev_io_completion_cb)(struct spdk_bdev_io *bdev_io, bool success,
					   void *cb_arg);

int spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
int spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			  void *buf, uint64_t offset_blocks, uint64_t num_blocks,
			  spdk_bdev_io_completion_cb cb, void *cb_arg);
int spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
			   uint64_t offset_blocks, uint64_t num_blocks,
			   spdk_bdev_io_completion_cb cb, void *cb_arg);
void spdk_bdev_free_io(struct spdk_bdev_io *bdev_io);
struct spdk_io_channel *spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc);
void spdk_put_io_channel(struct spdk_io_channel *ch);

#endif
