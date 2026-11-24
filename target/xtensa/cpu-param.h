/*
 * Xtensa cpu parameters for qemu.
 *
 * Copyright (c) 2011, Max Filippov, Open Source and Linux Lab.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef XTENSA_CPU_PARAM_H
#define XTENSA_CPU_PARAM_H

#define TARGET_LONG_BITS 32
#define TARGET_PAGE_BITS 12
#define TARGET_PHYS_ADDR_SPACE_BITS 32
#ifdef CONFIG_USER_ONLY
#define TARGET_VIRT_ADDR_SPACE_BITS 30
#else
#define TARGET_VIRT_ADDR_SPACE_BITS 32
#endif

/* Xtensa processors have a weak memory model */
#define TCG_GUEST_DEFAULT_MO      (0)

#define TB_JMP_CACHE_BITS         14
#define TB_JMP_CACHE_PCREL        0
#define TB_JMP_CACHE_PAGE_HASH    0
#define TB_LOOKUP_LOW_WORDS       1

/*
 * Lower bound of the dynamically sized softmmu TLB, per MMU index. The guest's
 * few boot-time flushes find the TLB lightly used and shrink it; with no later
 * flush it never grows back. The ESP32 memory map then puts hot pages in the
 * same slot (IRAM 0x40080000 and DPORT 0x3ff00000 both land in slot 0 of 64),
 * and every DPORT access refills. Keep enough entries to separate them.
 */
#define CPU_TLB_DYN_MIN_BITS 8
#define CPU_TLB_DYN_DEFAULT_BITS 8

#endif
