#  SPDX-License-Identifier: BSD-3-Clause
#  Copyright (c) 2026 Evariops.
#  All rights reserved.

# SPDK_ROOT_DIR: use parent if module is in-tree, else use vendor/ path.
SPDK_ROOT_DIR ?= $(abspath $(CURDIR)/../../..)
ifeq ($(wildcard $(SPDK_ROOT_DIR)/mk/spdk.common.mk),)
  SPDK_ROOT_DIR := $(abspath $(CURDIR)/../../../vendor/spdk)
endif
include $(SPDK_ROOT_DIR)/mk/spdk.common.mk

SO_VER := 1
SO_MINOR := 0

CFLAGS += -I$(SPDK_ROOT_DIR)/lib/bdev/

C_SRCS = vbdev_cbt.c vbdev_cbt_rpc.c
LIBNAME = bdev_cbt

SPDK_MAP_FILE = $(SPDK_ROOT_DIR)/mk/spdk_blank.map

include $(SPDK_ROOT_DIR)/mk/spdk.lib.mk
