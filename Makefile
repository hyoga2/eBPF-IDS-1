# SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)

XDP_TARGETS  := xdp_prog_kern
USER_TARGETS := xdp_prog_user

SRC_DIR := src
TARGET_DIR := target

LIBBPF_DIR = ./ebpf/libbpf/src/
COMMON_DIR = ./common

COPY_LOADER := xdp_loader
COPY_STATS  := xdp_stats
EXTRA_DEPS := $(COMMON_DIR)/parsing_helpers.h

include $(COMMON_DIR)/common.mk
