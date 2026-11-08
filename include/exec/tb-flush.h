/*
 * tb-flush prototype for use by the rest of the system.
 *
 * Copyright (c) 2022 Linaro Ltd
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _TB_FLUSH_H_
#define _TB_FLUSH_H_

#include "exec/vaddr.h"

/**
 * tb_flush() - flush all translation blocks
 * @cs: CPUState (must be valid, but treated as anonymous pointer)
 *
 * Used to flush all the translation blocks in the system. Sometimes
 * it is simpler to flush everything than work out which individual
 * translations are now invalid and ensure they are not called
 * anymore.
 *
 * tb_flush() takes care of running the flush in an exclusive context
 * if it is not already running in one. This means no guest code will
 * run until this complete.
 */
void tb_flush(CPUState *cs);

void tcg_flush_jmp_cache(CPUState *cs);

/*
 * Some targets translate a guest load whose address is known at translate
 * time, from plain RAM, into a load from the host pointer, without the
 * softmmu TLB. They record the guest addresses with tb_note_direct_load().
 * Anything that would change the outcome of such a load without flushing
 * the translations, such as a new watchpoint or a change to the guest's
 * memory protection, calls tb_flush_direct_loads() first. It flushes all
 * translations if any recorded address lies in [addr, addr + len), or if
 * len is 0 and any address is recorded. A full flush clears the record.
 */
void tb_note_direct_load(vaddr addr, vaddr len);
void tb_flush_direct_loads(CPUState *cs, vaddr addr, vaddr len);

#endif /* _TB_FLUSH_H_ */
