// SPDX-License-Identifier: GPL-2.0
/*
 * DRR demo driver
 *
 * Registers a small region holding a counter and a string with DRR,
 * and provides sysfs knobs to exercise the full
 * checkpoint -> corrupt -> panic -> reboot -> rollback loop.
 */

#define pr_fmt(fmt) "drr_demo: " fmt

#include <linux/drr.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#define DEMO_MAGIC "DEMO-STATE-v1"

static struct {
	char magic[16];
	u32 counter;
	char note[64];
} demo_state;

static struct kobject *demo_kobj;

static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	return sysfs_emit(buf, "magic=%s counter=%u note=%s\n",
			  demo_state.magic, demo_state.counter,
			  demo_state.note);
}
static struct kobj_attribute state_attr = __ATTR_RO(state);

/* echo 42 > set  =>  counter=42 */
static ssize_t set_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned int v;
	int ret;

	ret = kstrtouint(buf, 0, &v);
	if (ret)
		return ret;

	demo_state.counter = v;
	pr_info("counter set to %u\n", v);
	return count;
}
static struct kobj_attribute set_attr = __ATTR(set, 0200, NULL, set_store);

static ssize_t panic_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	panic("drr_demo: triggered panic, DRR should roll back on next boot");
	return count;
}
static struct kobj_attribute panic_attr =
	__ATTR(panic, 0200, NULL, panic_store);

static struct attribute *demo_attrs[] = {
	&state_attr.attr,
	&set_attr.attr,
	&panic_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(demo);

static int __init drr_demo_init(void)
{
	int ret;

	strscpy(demo_state.magic, DEMO_MAGIC, sizeof(demo_state.magic));
	demo_state.counter = 5;
	strscpy(demo_state.note, "checkpointed state", sizeof(demo_state.note));

	ret = drr_register_region(&demo_state, sizeof(demo_state), "demo");
	if (ret) {
		pr_err("failed to register region: %d\n", ret);
		return ret;
	}

	demo_kobj = kobject_create_and_add("drr_demo", kernel_kobj);
	if (!demo_kobj)
		return -ENOMEM;

	ret = sysfs_create_groups(demo_kobj, demo_groups);
	if (ret)
		kobject_put(demo_kobj);
	return ret;
}
module_init(drr_demo_init);

static void __exit drr_demo_exit(void)
{
	kobject_put(demo_kobj);
}
module_exit(drr_demo_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DRR demo: page-level checkpoint/rollback loop");
