/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Deshab-style Dedicated Recovery Root (DRR-lite)
 *
 * Experimental page-level checkpoint/rollback framework.
 * Design borrowed from Deshab-OS DRR: A/B slot metadata with CRC and
 * atomic slot flip, checkpoint storage independent of the page allocator,
 * panic-time crash stamp, early-boot recovery.
 */

#ifndef _LINUX_DRR_H
#define _LINUX_DRR_H

#include <linux/types.h>
#include <linux/list.h>

/*
 * DRR metadata lives at the start of the reserved DRR area and survives
 * warm reboots (same trick as pstore/ramoops).  Two copies (slot A/B);
 * a monotonically increasing sequence number decides which one is newer,
 * a single-word write of "active" performs the atomic slot flip.
 *
 * TODO(user): 如果布局需要调整（比如加 MAC root 对齐 Deshab），在这里改。
 */
#define DRR_MAGIC	0x44525231U	/* "DRR1" */

#define DRR_STATE_OK			0
#define DRR_STATE_CRASH_PENDING		1

struct drr_slot_meta {
	__le32 magic;		/* DRR_MAGIC, 0 = slot invalid */
	__le32 state;		/* DRR_STATE_* */
	__le64 seq;		/* monotonic slot sequence number */
	__le32 nr_regions;	/* regions captured in this slot */
	__le32 crc;		/* CRC32 over payload area */
	__le64 crash_time_ns;	/* filled by panic notifier */
} __packed;

struct drr_region_desc {
	__le64 base;		/* kernel virtual address of region */
	__le64 len;
	__le32 name_off;	/* name offset in payload area */
	__le32 pad;
} __packed;

/**
 * drr_register_region - protect a kernel memory range with DRR
 * @base: kernel virtual address (must be page-aligned)
 * @len: length in bytes
 * @name: short unique name, shown in the recovery log
 *
 * Regions must be registered before any checkpoint is taken and must not
 * be unregistered (v1 keeps it simple: no unregister).
 */
int drr_register_region(void *base, size_t len, const char *name);

/**
 * drr_checkpoint - capture all registered regions into the inactive slot
 * and atomically flip the active slot.
 */
int drr_checkpoint(void);

/**
 * drr_rollback - restore all registered regions from the active slot.
 * Aborts without touching state if the slot CRC is bad.
 */
int drr_rollback(void);

#endif /* _LINUX_DRR_H */
