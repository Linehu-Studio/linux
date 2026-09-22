// SPDX-License-Identifier: GPL-2.0
/*
 * Deshab-style Dedicated Recovery Root (DRR-lite)
 *
 * EXPERIMENTAL: port of the Deshab-OS DRR design.  A boot-reserved physical
 * area survives warm reboots (same trick as pstore/ramoops).  Kernel memory
 * regions registered via drr_register_region() can be checkpointed into A/B
 * slots (each slot owns its own payload half), protected by CRC32 and a
 * monotonically increasing sequence number that makes the slot flip atomic
 * from the next boot's point of view.  On panic a notifier stamps the
 * active slot; the next boot detects "recovery pending" and rolls the
 * registered regions back automatically.
 *
 * Layout of the reserved area:
 *
 *   [slot A meta][slot B meta][payload half A][payload half B]
 *
 * Payload half layout (per slot):
 *
 *   [nr * drr_region_desc][nr * 24-byte names][region copies...]
 */

#define pr_fmt(fmt) "DRR: " fmt

#include <linux/drr.h>
#include <linux/crc32.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/memblock.h>
#include <linux/module.h>
#include <linux/notifier.h>
#include <linux/panic_notifier.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define DRR_NAME_LEN	24

static phys_addr_t drr_base_phys;
static size_t drr_area_size;
static bool drr_enabled;
static bool drr_reserved;

static struct drr_slot_meta *slot_a;
static struct drr_slot_meta *slot_b;
static void *payload_base;		/* start of payload halves */
static size_t payload_half;		/* bytes per slot payload half */

struct drr_region {
	struct list_head list;
	void *base;
	size_t len;
	char name[DRR_NAME_LEN];
};

static LIST_HEAD(drr_regions);
static int drr_nr_regions;
static DEFINE_MUTEX(drr_lock);

/* cached boot-time slot decision; authority on disk is the metadata */
static int active_slot;
static u64 drr_seq;

/* ------------------------------------------------------------------ */
/* slot helpers */

static struct drr_slot_meta *drr_slot_ptr(int i)
{
	return i ? slot_b : slot_a;
}

static void *drr_slot_payload(int i)
{
	return payload_base + (size_t)i * payload_half;
}

static bool drr_slot_valid(struct drr_slot_meta *meta)
{
	u32 crc;

	if (le32_to_cpu(meta->magic) != DRR_MAGIC)
		return false;

	crc = crc32_le(~0, drr_slot_payload(meta == slot_b), payload_half);
	return crc == le32_to_cpu(meta->crc);
}

static void drr_find_active(void)
{
	int i;

	active_slot = 0;
	drr_seq = 0;
	for (i = 0; i < 2; i++) {
		struct drr_slot_meta *meta = drr_slot_ptr(i);
		u64 seq = le64_to_cpu(meta->seq);

		if (drr_slot_valid(meta) && seq > drr_seq) {
			active_slot = i;
			drr_seq = seq;
		}
	}
}

/* ------------------------------------------------------------------ */
/* checkpoint: capture all registered regions into the inactive slot */

int drr_checkpoint(void)
{
	struct drr_slot_meta *meta;
	struct drr_region_desc *descs;
	char *names;
	void *copy_area;
	void *dst;
	struct drr_region *r;
	size_t used;
	int i, idx = 0;
	u32 crc;

	if (!drr_reserved)
		return -ENODEV;

	mutex_lock(&drr_lock);
	if (drr_nr_regions == 0) {
		mutex_unlock(&drr_lock);
		return -ENODATA;
	}

	i = active_slot ^ 1;
	descs = drr_slot_payload(i);
	names = (char *)descs + drr_nr_regions * sizeof(*descs);
	copy_area = names + (size_t)drr_nr_regions * DRR_NAME_LEN;
	used = (size_t)drr_nr_regions * (sizeof(*descs) + DRR_NAME_LEN);

	/* sanity: everything must fit in the payload half */
	list_for_each_entry(r, &drr_regions, list)
		used += r->len;
	if (used > payload_half) {
		mutex_unlock(&drr_lock);
		return -ENOSPC;
	}

	dst = copy_area;
	list_for_each_entry(r, &drr_regions, list) {
		size_t off = (size_t)idx * DRR_NAME_LEN;

		descs[idx].base = cpu_to_le64((unsigned long)r->base);
		descs[idx].len = cpu_to_le64(r->len);
		descs[idx].name_off = cpu_to_le32(off);
		descs[idx].pad = 0;
		memcpy(names + off, r->name, DRR_NAME_LEN);
		memcpy(dst, r->base, r->len);
		dst += r->len;
		idx++;
	}

	/* fill the inactive slot, then flip by publishing its seq */
	meta = drr_slot_ptr(i);
	memset(meta, 0, sizeof(*meta));
	meta->magic = cpu_to_le32(DRR_MAGIC);
	meta->state = cpu_to_le32(DRR_STATE_OK);
	meta->seq = cpu_to_le64(drr_seq + 1);
	meta->nr_regions = cpu_to_le32(drr_nr_regions);
	meta->crash_time_ns = 0;
	crc = crc32_le(~0, drr_slot_payload(i), payload_half);
	meta->crc = cpu_to_le32(crc);
	/* publish the new slot: payload and meta precede the seq bump */
	smp_wmb();

	active_slot = i;
	drr_seq++;
	mutex_unlock(&drr_lock);

	pr_info("checkpoint seq %llu slot %d: %d regions, %zu bytes\n",
		drr_seq, i, idx, used);
	return 0;
}
EXPORT_SYMBOL_GPL(drr_checkpoint);

/* ------------------------------------------------------------------ */
/* rollback: restore registered regions from the active slot */

int drr_rollback(void)
{
	struct drr_slot_meta *meta;
	struct drr_region_desc *descs;
	char *names;
	void *copy_area;
	void *src;
	int i, idx, restored = 0;

	if (!drr_reserved)
		return -ENODEV;

	i = active_slot;
	meta = drr_slot_ptr(i);
	if (!drr_slot_valid(meta))
		return -EINVAL;

	descs = drr_slot_payload(i);
	names = (char *)descs + le32_to_cpu(meta->nr_regions) * sizeof(*descs);
	copy_area = names + (size_t)le32_to_cpu(meta->nr_regions) * DRR_NAME_LEN;
	src = copy_area;

	mutex_lock(&drr_lock);
	for (idx = 0; idx < le32_to_cpu(meta->nr_regions); idx++) {
		struct drr_region_desc *d = &descs[idx];
		size_t len = le64_to_cpu(d->len);
		const char *name = names + le32_to_cpu(d->name_off);
		struct drr_region *r;
		bool found = false;

		list_for_each_entry(r, &drr_regions, list) {
			if (!strcmp(r->name, name)) {
				found = true;
				break;
			}
		}
		if (!found || r->len != len) {
			pr_warn("slot %d region '%s' mismatch, abort rollback\n",
				i, name);
			mutex_unlock(&drr_lock);
			return -EINVAL;
		}
		memcpy(r->base, src, len);
		src += len;
		restored++;
	}
	mutex_unlock(&drr_lock);

	pr_info("rollback from slot %d: %d regions restored\n", i, restored);
	return 0;
}
EXPORT_SYMBOL_GPL(drr_rollback);

/* ------------------------------------------------------------------ */
/* Panic hook: stamp the active slot so the next boot knows to recover.
 * Panic context: no allocation, no sleeping, minimal work. */
static int drr_panic_event(struct notifier_block *nb, unsigned long ev,
			   void *p)
{
	struct drr_slot_meta *meta = drr_slot_ptr(active_slot);

	if (!drr_reserved)
		return NOTIFY_DONE;

	meta->state = cpu_to_le32(DRR_STATE_CRASH_PENDING);
	meta->crash_time_ns = cpu_to_le64(ktime_get_real_ns());
	return NOTIFY_DONE;
}

static struct notifier_block drr_panic_nb = {
	.notifier_call = drr_panic_event,
	.priority = INT_MAX,	/* stamp as early as possible in panic */
};

/* ------------------------------------------------------------------ */
/* Region registry */
int drr_register_region(void *base, size_t len, const char *name)
{
	struct drr_region *r;

	if (!drr_reserved || !name || !len)
		return -EINVAL;

	mutex_lock(&drr_lock);
	list_for_each_entry(r, &drr_regions, list) {
		if (!strcmp(r->name, name)) {
			mutex_unlock(&drr_lock);
			return -EEXIST;
		}
	}
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r) {
		mutex_unlock(&drr_lock);
		return -ENOMEM;
	}
	r->base = base;
	r->len = len;
	strscpy(r->name, name, sizeof(r->name));
	list_add_tail(&r->list, &drr_regions);
	drr_nr_regions++;
	mutex_unlock(&drr_lock);

	pr_info("region '%s' registered: %p+%zu\n", name, base, len);
	return 0;
}
EXPORT_SYMBOL_GPL(drr_register_region);

/* ------------------------------------------------------------------ */
/* Boot: reserve the DRR area.  The physical address must be given on
 * the command line so it lands on the same memory after warm reboot. */
static int __init drr_reserve_setup(char *s)
{
	unsigned long long base, size_mb = 8;

	if (sscanf(s, "0x%llx:%llu", &base, &size_mb) != 2 &&
	    sscanf(s, "%llx:%llu", &base, &size_mb) != 2)
		return 0;

	drr_base_phys = base;
	drr_area_size = (size_t)size_mb * SZ_1M;
	drr_enabled = true;
	return 1;
}
__setup("drr_reserve=", drr_reserve_setup);

static int __init drr_memblock_init(void)
{
	size_t meta_size = 2 * sizeof(struct drr_slot_meta);

	if (!drr_enabled) {
		pr_info("disabled (no drr_reserve= on cmdline)\n");
		return 0;
	}
	if (drr_area_size < meta_size * 2 + PAGE_SIZE) {
		pr_err("area too small\n");
		return 0;
	}
	if (memblock_reserve(drr_base_phys, drr_area_size)) {
		pr_err("failed to reserve %pa+%zu\n", &drr_base_phys,
		       drr_area_size);
		return 0;
	}
	drr_reserved = true;

	slot_a = phys_to_virt(drr_base_phys);
	slot_b = phys_to_virt(drr_base_phys + sizeof(struct drr_slot_meta));
	payload_base = phys_to_virt(drr_base_phys + meta_size);
	payload_half = (drr_area_size - meta_size) / 2;

	drr_find_active();
	atomic_notifier_chain_register(&panic_notifier_list, &drr_panic_nb);
	pr_info("reserved %pa+%zu, active slot %d seq %llu\n",
		&drr_base_phys, drr_area_size, active_slot, drr_seq);
	return 0;
}
arch_initcall(drr_memblock_init);

/* ------------------------------------------------------------------ */
/* Recovery, phase 1: detect "previous boot panicked" early.
 * Regions may not be registered yet (drivers init later), so the actual
 * rollback is deferred to late_initcall, after all built-in drivers ran. */
static bool recovery_pending;

static int __init drr_recover_init(void)
{
	struct drr_slot_meta *meta;

	if (!drr_reserved)
		return 0;

	meta = drr_slot_ptr(active_slot);
	if (!drr_slot_valid(meta)) {
		if (le32_to_cpu(meta->magic) == DRR_MAGIC)
			pr_warn("active slot %d corrupted, ignoring\n",
				active_slot);
		return 0;
	}
	if (le32_to_cpu(meta->state) != DRR_STATE_CRASH_PENDING)
		return 0;

	recovery_pending = true;
	pr_info("recovery pending from crash at %llu ns\n",
		le64_to_cpu(meta->crash_time_ns));
	return 0;
}
subsys_initcall(drr_recover_init);

/* ------------------------------------------------------------------ */
/* Recovery, phase 2: all built-in drivers have registered their regions;
 * roll back now. */
static int __init drr_late_recover(void)
{
	struct drr_slot_meta *meta;
	int ret;

	if (!drr_reserved || !recovery_pending)
		return 0;

	ret = drr_rollback();
	if (ret) {
		int other = active_slot ^ 1;

		pr_warn("rollback from slot %d failed (%d), trying slot %d\n",
			active_slot, ret, other);
		if (drr_slot_valid(drr_slot_ptr(other))) {
			active_slot = other;
			drr_seq = le64_to_cpu(drr_slot_ptr(other)->seq);
			ret = drr_rollback();
		}
	}
	if (ret) {
		pr_err("recovery root lost: no usable slot\n");
		recovery_pending = false;
		return 0;
	}

	meta = drr_slot_ptr(active_slot);
	meta->state = cpu_to_le32(DRR_STATE_OK);
	recovery_pending = false;
	pr_info("recovery complete: system state restored from checkpoint\n");
	return 0;
}
late_initcall(drr_late_recover);

/* ------------------------------------------------------------------ */
/* sysfs */
static ssize_t checkpoint_store(struct kobject *kobj,
				struct kobj_attribute *attr,
				const char *buf, size_t count)
{
	int ret = drr_checkpoint();

	return ret ? ret : count;
}
static struct kobj_attribute drr_checkpoint_attr =
	__ATTR(checkpoint, 0200, NULL, checkpoint_store);

static ssize_t rollback_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	int ret = drr_rollback();

	return ret ? ret : count;
}
static struct kobj_attribute drr_rollback_attr =
	__ATTR(rollback, 0200, NULL, rollback_store);

static ssize_t status_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	struct drr_slot_meta *meta = drr_slot_ptr(active_slot);
	ssize_t off = 0;
	struct drr_region *r;

	if (!drr_reserved)
		return sysfs_emit(buf, "disabled\n");

	off += sysfs_emit_at(buf, off, "base %pa size %zu\n",
			     &drr_base_phys, drr_area_size);
	off += sysfs_emit_at(buf, off, "active slot %d seq %llu state %u\n",
			     active_slot, drr_seq, le32_to_cpu(meta->state));
	list_for_each_entry(r, &drr_regions, list)
		off += sysfs_emit_at(buf, off, "region %s %p+%zu\n",
				     r->name, r->base, r->len);
	return off;
}
static struct kobj_attribute drr_status_attr = __ATTR_RO(status);

static struct attribute *drr_sysfs_attrs[] = {
	&drr_checkpoint_attr.attr,
	&drr_rollback_attr.attr,
	&drr_status_attr.attr,
	NULL,
};

static const struct attribute_group drr_sysfs_group = {
	.attrs = drr_sysfs_attrs,
};

static int __init drr_sysfs_init(void)
{
	struct kobject *drr_kobj;
	int ret;

	if (!drr_reserved)
		return 0;

	drr_kobj = kobject_create_and_add("drr", kernel_kobj);
	if (!drr_kobj)
		return -ENOMEM;

	ret = sysfs_create_group(drr_kobj, &drr_sysfs_group);
	if (ret)
		kobject_put(drr_kobj);
	return ret;
}
late_initcall(drr_sysfs_init);
