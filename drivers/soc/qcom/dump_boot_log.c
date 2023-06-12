// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/memblock.h>
#include <linux/of_address.h>
#include <linux/kthread.h>
#include <linux/slab.h>

#define CHAR_WIDTH 8
#define TZ_SIZE ((CHAR_WIDTH * sizeof(uint32_t) - 1)/3 + 1)

static char *xbl_log_buf;
static size_t xbl_log_size;

static char *tz_start_buf;
static size_t tz_start_size;

static char *tz_end_buf;
static size_t tz_end_size;

static struct kobject *kobj;
static struct device_node *parent, *node;

static struct kobj_type xbl_log_kobj_type = {
	.sysfs_ops = &kobj_sysfs_ops,
};

static ssize_t xbl_log_show(struct file *fp,
			    struct kobject *kobj,
			    struct bin_attribute *bin_attr, char *buf,
			    loff_t offset, size_t count)
{
	unsigned start_len = strlen("TZ Start [ ]");
	unsigned stop_len = strlen("TZ End [ ]");
	size_t tz_count_len = start_len + stop_len + 2 * TZ_SIZE;
	size_t max_sz = min(xbl_log_size + tz_count_len - offset, count);
	size_t ret = 0;

	if (offset < xbl_log_size + 2 * TZ_SIZE) {
		ret = scnprintf(buf, max_sz, "%s", xbl_log_buf + offset);

		count -= (ret + 1);

		if (count && ret) {
			offset += ret;
			max_sz = (xbl_log_size + tz_count_len - offset, count);

			ret += scnprintf(buf + ret, max_sz,
					 "TZ Start [ %s]\nTZ End [ %s]",
					 tz_start_buf, tz_end_buf);
		}
	}

	return ret;
}

static struct bin_attribute attribute =
__BIN_ATTR(xbl_log, 0444, xbl_log_show, NULL, 0);

static void free_xbl_log_buf(phys_addr_t paddr, size_t size)
{
	unsigned long pfn_start = 0, pfn_end = 0, pfn_idx = 0;

	memblock_free((void *)paddr, size);
	pfn_start = paddr >> PAGE_SHIFT;
	pfn_end = (paddr + size) >> PAGE_SHIFT;
	for (pfn_idx = pfn_start; pfn_idx < pfn_end; pfn_idx++)
		free_reserved_page(pfn_to_page(pfn_idx));
}

static int map_addr_range_res_mem(const char *name, size_t * size, char **buf)
{
	void *addr;
	phys_addr_t paddr;
	struct resource res_log = { 0, };
	int ret = -ENODEV;

	node = of_find_node_by_name(parent, name);
	if (!node) {
		pr_err("%s node missing\n", name);
		goto node_fail;
	}

	if (of_address_to_resource(node, 0, &res_log))
		goto node_fail;

	paddr = res_log.start;

	if (!resource_size(&res_log))
		return -ENODEV;

	*size = resource_size(&res_log) - 1;

	addr = memremap(paddr, *size, MEMREMAP_WB);
	if (!addr) {
		pr_err("%s: memremap failed\n", name);
		ret = -ENOMEM;
		goto remap_fail;
	}

	*buf = kzalloc(*size, GFP_KERNEL);
	if (*buf) {
		memcpy(*buf, addr, *size);
		(*buf)[*size - 1] = '\0';
		memunmap(addr);
		if (*size)
			free_xbl_log_buf(paddr, *size);
		return 0;
	}

	pr_err("%s: kzalloc failed\n", name);
	ret = -ENODEV;
	memunmap(addr);
remap_fail:
	if (node)
		of_node_put(node);
node_fail:
	if (parent)
		of_node_put(parent);

	return ret;
}

static int map_addr_range_iomem(const char *name, size_t * size, char **buf)
{
	void __iomem *addr = NULL;
	int ret = -ENODEV;

	node = of_find_node_by_name(parent, name);
	if (!node) {
		pr_err("%s node missing\n", name);
		goto node_fail;
	}

	addr = of_iomap(node, 0);
	if (!addr) {
		pr_err("%s: memremap failed\n", name);
		ret = -ENOMEM;
		goto remap_fail;
	}
	*size = TZ_SIZE;

	*buf = kzalloc(*size, GFP_KERNEL);
	if (*buf) {
		scnprintf(*buf, TZ_SIZE, "%u", readl(addr));
		iounmap(addr);
		return 0;
	}

	pr_err("%s: kzalloc failed\n", name);
	ret = -ENODEV;
	iounmap(addr);
remap_fail:
	if (node)
		of_node_put(node);
node_fail:
	if (parent)
		of_node_put(parent);

	return ret;
}

static int __init dump_boot_log_init(void)
{
	int err = 1;

	kobj = kcalloc(1, sizeof(*kobj), GFP_KERNEL);
	if (!kobj)
		return 1;

	err = kobject_init_and_add(kobj, &xbl_log_kobj_type, kernel_kobj, "xbl_log");
	if (err) {
		pr_err("xbl_log: cannot create kobject\n");
		goto kobj_free;
	}

	kobject_get(kobj);
	if (IS_ERR_OR_NULL(kobj)) {
		err = PTR_ERR(kobj);
		goto kobj_free;
	}

	parent = of_find_node_by_path("/reserved-memory");
	if (!parent) {
		pr_err("xbl_log: reserved-memory node missing\n");
		goto kobj_put;
	}

	err = map_addr_range_res_mem("uefi-log", &xbl_log_size, &xbl_log_buf);
	if (err)
		goto kobj_put;

	err = map_addr_range_iomem("tz-start-count", &tz_start_size, &tz_start_buf);
	if (err)
		goto xbl_log_free;

	err = map_addr_range_iomem("tz-end-count", &tz_end_size, &tz_end_buf);
	if (err)
		goto tz_start_free;

	err = sysfs_create_bin_file(kobj, &attribute);
	if (err) {
		pr_err("xbl_log: sysfs entry creation failed\n");
		goto tz_end_free;
	}

	return 0;

tz_end_free:
	kfree(tz_end_buf);
tz_start_free:
	kfree(tz_start_buf);
xbl_log_free:
	kfree(xbl_log_buf);
kobj_put:
	kobject_put(kobj);
kobj_free:
	kobject_del(kobj);
	kfree(kobj);
	return 1;
}

static void __exit dump_boot_log_exit(void)
{
	kfree(xbl_log_buf);
	kobject_del(kobj);
	kfree(kobj);
	if (node)
		of_node_put(node);
	if (parent)
		of_node_put(parent);
}

module_init(dump_boot_log_init);
module_exit(dump_boot_log_exit);
MODULE_DESCRIPTION("dump xbl log");
MODULE_LICENSE("GPL v2");
