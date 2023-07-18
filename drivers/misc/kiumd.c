/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sort.h>
#include <linux/of_platform.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/vfio.h>
#include <linux/hashtable.h>
#include <uapi/misc/kiumd.h>
#include <linux/iommu.h>
#include <linux/types.h>
#include <linux/dma-iommu.h>
#include <linux/iova.h>
#include <linux/adreno-smmu-priv.h>
#include <linux/io-pgtable.h>
#include <linux/kernel.h>
#include <linux/kvm_host.h>
#include <linux/cdev.h>
#include <linux/module.h>
#include <linux/qcom_scm.h>
#include <linux/iommu_iova_map.h>
#include <linux/sizes.h>

#include "../../drivers/iommu/arm/arm-smmu/arm-smmu.h"

extern int qcom_adreno_smmu_set_ttbr0_cfg(const void *cookie, const struct io_pgtable_cfg *pgtbl_cfg);

/*Global Data structures needed for buffer sharing */
static DEFINE_MUTEX(g_kiumd_lock);
//static DEFINE_HASHTABLE(g_dmabuf_kiumd_table, 10);

unsigned long *global_map = NULL;
unsigned long *perprocess_map = NULL;

#define KGSL_PT_MEM_SIZE (1024 * SZ_1M)
#define KGSL_PT_MEM_PAGES (KGSL_PT_MEM_SIZE >> PAGE_SHIFT)
#define KGSL_GLOBAL_PT_BASE_IOVA 0xFFFFFF8000000000
#define KGSL_PER_PROCESS_PT_BASE_IOVA 0x60000000
#define KGSL_GLOBAL_PT 1
#define KGSL_PER_PROCESS_PT 2

struct dmabuf_fd {
	struct dma_buf *kiumd_dmabuf; //Value
	uint32_t token;  //Key
	struct list_head *next;
};
struct dmabuf_fd dmabuf_tbl[1024];
static uint32_t g_counter = 0;

struct kiumd_dev {
	struct device* dev;
	struct miscdevice miscdev;
	int fd;
};

enum iommu_dma_cookie_type {
       IOMMU_DMA_IOVA_COOKIE,
       IOMMU_DMA_MSI_COOKIE,
};

struct kiumd_iommu_dma_cookie {
	enum iommu_dma_cookie_type      type;
	union {
        struct {
               struct iova_domain      iovad;
               struct iova_fq __percpu *fq;    /* Flush queue */
               atomic64_t              fq_flush_start_cnt;
               atomic64_t              fq_flush_finish_cnt;
               struct timer_list       fq_timer;
               atomic_t                fq_timer_on;
             };
               dma_addr_t              msi_iova;
         };
         struct list_head                msi_page_list;
         struct iommu_domain             *fq_domain;
};

static void _tlb_flush_all(void *cookie)
{
}

static void _tlb_flush_walk(unsigned long iova, size_t size,
		size_t granule, void *cookie)
{
}

static void _tlb_add_page(struct iommu_iotlb_gather *gather,
		unsigned long iova, size_t granule, void *cookie)
{
}

static const struct iommu_flush_ops kgsl_iopgtbl_tlb_ops = {
	.tlb_flush_all = _tlb_flush_all,
	.tlb_flush_walk = _tlb_flush_walk,
	.tlb_add_page = _tlb_add_page,
};

struct io_pgtable *pgtable;

int kiumd_perprocess_set_user_context(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;
	int cbindx, ret;
	void *cookie;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	file = fget(kismmu_pproc.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	iommu_dom = iommu_get_domain_for_dev(vfio_dev->dev);
	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom->pgtbl_ops == NULL){
		pr_err("%s:pagetable ops is NULL \n",__func__);
		return -ENOMEM;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if(pgtable == NULL) {
		pr_err("%s:pagetable is NULL \n",__func__);
		return -ENOMEM;
	}
	cbindx = smmu_dom->cfg.cbndx;
	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	kismmu_pproc.pgtbl_ops_ptr = (long int)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if(kismmu_pproc.pgtbl_ops_ptr == NULL) {
		pr_err("%s:failed to allocate pagetable ops \n",__func__);
		return -ENOMEM;
	}
	cookie = (void*)smmu_dom;
	qcom_adreno_smmu_set_ttbr0_cfg(cookie, &cfg);
	ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);

	if (ret) {
		pr_err("%s:Setting smmu aperture error \n",__func__);
		return ret;
	}
        fput(file);
	return 0;
}

int kiumd_perprocess_pt_alloc(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct iommu_domain *iommu_dom;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	file = fget(kismmu_pproc.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	iommu_dom = iommu_get_domain_for_dev(vfio_dev->dev);
	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom->pgtbl_ops == NULL) {
		pr_err("%s:pagetable ops is NULL \n",__func__);
		return -ENOMEM;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kismmu_pproc.asid = smmu_dom->cfg.asid;
	kismmu_pproc.pgtbl_ops_ptr = (long int)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	if(kismmu_pproc.pgtbl_ops_ptr == NULL) {
		pr_err("%s:failed to allocate pagetable ops \n",__func__);
		return -ENOMEM;
	}

	kismmu_pproc.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;
        fput(file);

	copy_to_user(arg, &kismmu_pproc, sizeof(kismmu_pproc));
	return 0;
}

int kiumd_perprocess_pgtble_set(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct vfio_group *vfio_grp;
	struct iommu_domain *iommu_dom;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable_ops *ki_pgtbl_ops;
	struct arm_smmu_cb *cb;
	struct arm_smmu_cfg *smmu_cfg;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	file = fget(kismmu_pproc.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	iommu_dom = iommu_get_domain_for_dev(vfio_dev->dev);
	if(iommu_dom == NULL) {
		pr_err("%s:IOMMU domain is NULL \n",__func__);
		return -ENOMEM;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom == NULL) {
		pr_err("%s:SMMU domain is NULL \n",__func__);
		return -ENOMEM;
	}

	/*Debug changes to validate per process mappings in test case.*/
/*	smmu_cfg = &smmu_dom->cfg;
	cb = &smmu_dom->smmu->cbs[smmu_cfg->cbndx];
	cb->ttbr[0] = kismmu_pproc.ttbr0;
	arm_smmu_write_context_bank(smmu_dom->smmu, cb->cfg->cbndx);*/
	ki_pgtbl_ops = (struct io_pgtable_ops*)kismmu_pproc.pgtbl_ops_ptr;
	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
        fput(file);

	return 0;
}

int kiumd_perprocess_pgtble_free(struct kiumd_dev *ki_dev, char __user *arg)
{
        struct kiumd_smmu_user kismmu_pproc;
        struct file *file;
        struct vfio_device *vfio_dev;
        struct io_pgtable_ops *ki_pgtbl_ops;

        if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
                return -EFAULT;

        file = fget(kismmu_pproc.vfio_fd);
        vfio_dev = (struct vfio_device *)file->private_data;
        if(vfio_dev == NULL) {
                pr_err("%s:vfio_dev is NULL \n",__func__);
                return -ENOTTY;
        }

        ki_pgtbl_ops = (struct io_pgtable_ops*)kismmu_pproc.pgtbl_ops_ptr;
        if(ki_pgtbl_ops == NULL) {
                pr_err("%s:pagegetable ops is NULL \n",__func__);
                return -ENOMEM;
        }

        free_io_pgtable_ops(ki_pgtbl_ops);
        fput(file);

        return 0;
}

int kiumd_dmabuf_custom_iova_init(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct file *file;
	struct kiumd_iommu_dma_cookie *cookie = NULL;
	struct iommu_domain *domain = NULL;
	struct iova_domain *iovad = NULL;
	struct iommu_resv_region *region;
	unsigned long lo, hi;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	file = fget(kiusr.vfio_fd);
	if (file == NULL) {
		pr_err("%s:Invalid vfio fd \n",__func__);
		return -EBADF;
	}

	vfio_dev = (struct vfio_device *)file->private_data;
	if (vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	domain = iommu_get_domain_for_dev(vfio_dev->dev);
	if (domain == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	cookie = (struct kiumd_iommu_dma_cookie*)domain->iova_cookie;
	iovad = &cookie->iovad;

	LIST_HEAD(resrvd);
	qcom_iommu_generate_resv_regions(vfio_dev->dev, &resrvd);
	list_for_each_entry(region, &resrvd, list) {
		lo = iova_pfn(iovad, region->start);
		hi = iova_pfn(iovad, region->start + region->length - 1);
		reserve_iova(iovad, lo, hi);
	}

	fput(file);
	return 0;
}

void clear_map_iova(u64 iova, u64 size, int ptselect)
{
	u64 bit;
	if(ptselect == KGSL_GLOBAL_PT) {
		bit = (iova & ~KGSL_GLOBAL_PT_BASE_IOVA) >> PAGE_SHIFT;
		bitmap_clear(global_map, bit, size >> PAGE_SHIFT);
	}
	else if (ptselect == KGSL_PER_PROCESS_PT) {
		bit = (iova - KGSL_PER_PROCESS_PT_BASE_IOVA) >> PAGE_SHIFT;
		bitmap_clear(perprocess_map, bit, size >> PAGE_SHIFT);
	}
}

u64 get_map_offset(u64 size, int ptselect)
{
	int start = 0;
	u64 bit, offset;
	if (ptselect == KGSL_GLOBAL_PT){
		if(global_map == NULL) {
			global_map = kcalloc(BITS_TO_LONGS(KGSL_PT_MEM_PAGES),
                                        sizeof(unsigned long), GFP_KERNEL);
			if (!global_map)
				return (u64) -ENOMEM;
		}
		bit = bitmap_find_next_zero_area(global_map, KGSL_PT_MEM_PAGES, start, size >> PAGE_SHIFT, 0);
		if(bit < 0) {
			pr_err( "Invalid next zero area in bitmap\n");
			return (u64) -ENOTTY;
		}
		bitmap_set(global_map, bit, size >> PAGE_SHIFT);
		offset = bit << PAGE_SHIFT;
	}
	else if (ptselect == KGSL_PER_PROCESS_PT)
	{
		if(perprocess_map == NULL) {
			perprocess_map = kcalloc(BITS_TO_LONGS(KGSL_PT_MEM_PAGES),
						sizeof(unsigned long), GFP_KERNEL);
			if (!perprocess_map)
				return (u64) -ENOMEM;
		}
		bit = bitmap_find_next_zero_area(perprocess_map, KGSL_PT_MEM_PAGES, start, size >> PAGE_SHIFT, 0);
		if(bit < 0) {
			pr_err("Invalid next zero area in bitmap\n");
			return (u64) -ENOTTY;
		}
		bitmap_set(perprocess_map, bit, size >> PAGE_SHIFT);
		offset = bit << PAGE_SHIFT;
	}
	else {
		pr_err("Invalid ptselect\n");
		return (u64) -EINVAL;
	}

	return offset;
}

int set_map_iova(u64 offset, struct vfio_device *vfio_dev, int ptselect)
{
	struct iommu_domain *domain = NULL;
	struct kiumd_iommu_dma_cookie *cookie = NULL;
	domain = iommu_get_domain_for_dev(vfio_dev->dev);
	cookie = (struct kiumd_iommu_dma_cookie*)domain->iova_cookie;
	if(!cookie){
		pr_err("kiumd_iova_ctrl: cookie not found\n");
		return -ENOMEM;
	}

	cookie->type = 1;
	if (ptselect == KGSL_GLOBAL_PT)
	        cookie->msi_iova = KGSL_GLOBAL_PT_BASE_IOVA + offset;
	else if (ptselect == KGSL_PER_PROCESS_PT)
		cookie->msi_iova = KGSL_PER_PROCESS_PT_BASE_IOVA + offset;

        return 0;
}

/**
 * kiumd_dmabuf_vfio_map(struct kiumd_dev *ki_dev, char __user *arg)
 *
 * This function facilitates the mapping of a DMA-BUF based buffer to a SMMU
 * backed device represented via a vfio_device.
 *
 * The function is called via IOCTL interface and input is provided via struct
 * kiumd_user from the userspace.
 *
 * return value is errno or 0 in case of successful mapping
 * */

int kiumd_dmabuf_vfio_map(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct vfio_device *vfio_dev;
	struct file *file;
	struct dma_buf *kiumd_dmabuf = NULL;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct sg_table *sgt = NULL;
	int kiumd_dma_direction, ret;
	u64 offset, size;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;


	file = fget(kiusr.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}
	// struct device st
	//dma_buf_get (fd ) -> dma_buf*

	kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);

	if(IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:kiumd_dmabuf is NULL \n",__func__);
		return -ENOTTY;
	}

	if ((kiusr.ptselect == KGSL_GLOBAL_PT) || (kiusr.ptselect == KGSL_PER_PROCESS_PT)) {
		size = kiumd_dmabuf->size;
		offset = get_map_offset(size, kiusr.ptselect);
		if(offset < 0) {
			 pr_err("%s:failed to get offset \n",__func__);
			 return -ENOMEM;
		}

		ret = set_map_iova(offset, vfio_dev, kiusr.ptselect);
		if(ret < 0) {
			pr_err("%s:failed to set offset \n",__func__);
			return -ENOMEM;
		}
	}

	//dma_buf_attach
	if (vfio_dev->dev != NULL)
		dmabufattach = dma_buf_attach(kiumd_dmabuf, vfio_dev->dev);
	if(dmabufattach == NULL) {
		pr_err("%s:dmabufattach is NULL \n",__func__);
		return -ENOTTY;
	}

	if(kiusr.dma_attr == DMA_ATTR_PRIVILEGED)
		dmabufattach->dma_map_attrs = kiusr.dma_attr;
	else
		dmabufattach->dma_map_attrs = 0;

	if(kiusr.dma_direction == 1 )
		kiumd_dma_direction = kiusr.dma_direction;
	else
		kiumd_dma_direction = 0;

        //dma_buf_map_attachment
	sgt = dma_buf_map_attachment(dmabufattach, kiumd_dma_direction);
	if(IS_ERR_OR_NULL(sgt)) {
		pr_err("%s:sgt is NULL \n",__func__);
		return (dmabufattach == NULL ? -ENOTTY: PTR_ERR(sgt));
	}

        fput(file);
	kiusr.sgt_ptr = sgt;
	kiusr.dmabufattach = dmabufattach;
	kiusr.dma_addr = sg_dma_address(sgt->sgl);
	kiusr.dmabuf_ptr = kiumd_dmabuf;
	copy_to_user(arg, &kiusr, sizeof(kiusr));

	return 0;
}

/**
 * kiumd_dmabuf_vfio_unmap(struct kiumd_dev *ki_dev, char __user *arg)
 *
 * This function facilitates the unmap the buffer mapped to SMMU backed device
 * also decrements the dma_buf kref count.
 *
 * return errno or 0 in case of success
 */
int kiumd_dmabuf_vfio_unmap(struct kiumd_dev *ki_dev, char __user *arg)
{

	struct kiumd_user kiusr;
	struct dma_buf_attachment *dmabufattach = NULL;
	struct dma_buf *kiumd_dmabuf = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	kiumd_dmabuf = (struct dma_buf *)kiusr.dmabuf_ptr;
	if(kiumd_dmabuf == NULL) {
		pr_err("%s:kiumd_dmabuf is NULL \n",__func__);
		return -ENOTTY;
	}

	dmabufattach = (struct dma_buf_attachment *)kiusr.dmabufattach;
	if(dmabufattach == NULL) {
		pr_err("%s:dmabufattach is NULL \n",__func__);
		return -ENOTTY;
	}

	if(kiusr.ptselect == KGSL_GLOBAL_PT || kiusr.ptselect == KGSL_PER_PROCESS_PT)
		clear_map_iova(kiusr.dma_addr, kiumd_dmabuf->size, kiusr.ptselect);

	dma_buf_unmap_attachment(dmabufattach, (struct sg_table *)kiusr.sgt_ptr,
							DMA_BIDIRECTIONAL);
	dma_buf_detach(kiumd_dmabuf, dmabufattach);
	dma_buf_put(kiumd_dmabuf);

	return 0;
}

int kiumd_export_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;
	// mutex_lock
	//dmabuf_tbl[g_counter].kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
	//dmabuf_tbl[g_counter].token = g_counter; // to be changed to hash(g_counter)
	//kiusr.buf_token =  g_counter;
	kiusr.dmabuf_ptr = dma_buf_get(kiusr.dma_buf_fd);
	pr_err("%s:kiusr.buf_token value... %x \n",__func__, kiusr.dmabuf_ptr);
	// mutex_unlock
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}

	return 0;
}

int kiumd_import_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct dma_buf *kiumd_dmabuf = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;

	if(kiumd_dmabuf == NULL) {
		pr_err("%s: find_dmabuf failed returned NULL buffer... \n",__func__);
		return -EFAULT;
	}
	kiusr.dma_buf_fd = dma_buf_fd(kiusr.dmabuf_ptr, (O_CLOEXEC));
	pr_err("%s: copy_to_user failed... FD %x \n",__func__, kiusr.dma_buf_fd);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}

	return 0;
}


int kiumd_dmabuf_fd(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	struct dma_buf *kiumd_dmabuf = NULL;

	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;
	// struct device st
	//dma_buf_get (fd ) -> dma_buf*


	// find new fd
	if (kiusr.flag == 1)
	{
	pr_err("%s:Calling  dma_buf_fd \n",__func__);
	pr_err("%s:Calling  dma_buf_get %x \n",__func__, kiusr.dmabuf_ptr);
	kiusr.heap_fd = dma_buf_fd(kiusr.dmabuf_ptr, (O_CLOEXEC));
	//importer_test(kiumd_dmabuf);
	}
	else {
			pr_err("%s:kiumd_dmabuf_fd %d   %d\n",__func__, kiusr.dma_buf_fd, kiusr.heap_fd);
		pr_err("%s:Calling  dma_buf_get \n",__func__);
		kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
		pr_err("%s:Calling  dma_buf_get %x \n",__func__, kiumd_dmabuf);
		//kiumd_dmabuf = dma_buf_get(kiusr.dma_buf_fd);
		if(IS_ERR_OR_NULL(kiumd_dmabuf)) {
		pr_err("%s:kiumd_dmabuf is NULL \n",__func__);
		return -ENOTTY;
		}
		kiusr.dmabuf_ptr = kiumd_dmabuf;
	}

	pr_err("%s:Calling  copy_to_user \n",__func__);
	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		//dma_buf_put(kiumd_dmabuf);
		pr_err("%s: copy_to_user failed... \n",__func__);
		return -EFAULT;
	}
	return 0;
}

int kiumd_iova_ctrl(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_iova iovausr;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct iommu_domain *domain = NULL;
	struct kiumd_iommu_dma_cookie *cookie = NULL;
	int cookie_type;
	dma_addr_t iova_usr;

	if (copy_from_user(&iovausr, arg, sizeof(struct kiumd_iova)))
		return -EFAULT;

	if(iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_CLEAR)
		cookie_type = 0;
	else
		cookie_type = 1;

	if(iovausr.iova_flag == KGSL_SMMU_GLOBALPT_FIXED_ADDR_SET) {
		cookie_type = 1;
		iova_usr = iovausr.iova;
	}

	file = fget(iovausr.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	domain = iommu_get_domain_for_dev(vfio_dev->dev);
	cookie = (struct kiumd_iommu_dma_cookie*)domain->iova_cookie;
	if(!cookie)	{
		pr_err("kiumd_iova_ctrl: cookie not found\n");
		return -ENOMEM;
	}
	cookie->type = cookie_type;
	cookie->msi_iova = iova_usr;
        fput(file);

	return 0;
}


DEFINE_IDR(idr);

int kiumd_fd_dmabuf_handler(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_user kiusr;
	static struct dma_buf *kiumd_dmabuf = NULL, *orig_buf;
        uint32_t local_id = 0;
	if (copy_from_user(&kiusr, arg, sizeof(struct kiumd_user)))
		return -EFAULT;


        if (kiusr.dma_buf_fd > 0 && kiusr.dmabuf_ptr == 0)
	{
	        //printk(KERN_DEBUG "%s:FD to HANDLE kiusr.dma_buf_fd:%d \n",__func__, kiusr.dma_buf_fd);
	        kiusr.dmabuf_ptr  = dma_buf_get(kiusr.dma_buf_fd);
                //orig_buf = kiusr.dmabuf_ptr;
                //idr_preload(GFP_KERNEL);
                //local_id = idr_alloc(&idr, kiusr.dmabuf_ptr, 1, 0, GFP_NOWAIT);
                //idr_preload_end();
                //kiusr.dmabuf_ptr = local_id;
                //printk(KERN_DEBUG "%s:Calling  dma_buf_get %x \n",__func__, kiusr.dmabuf_ptr);
        }
	else if (kiusr.dma_buf_fd == -1 )
        {
		//printk(KERN_DEBUG "%s:  HANDLE to FD  %pK \n",__func__, kiusr.dmabuf_ptr);
                //local_id = kiusr.dmabuf_ptr;
                //printk("%s:FD to HANDLE Local IDR number after allocation:%d \n", __func__, local_id );
                kiumd_dmabuf = (struct dma_buf *)kiusr.dmabuf_ptr;//idr_find(&idr,local_id);
	        if (kiumd_dmabuf != NULL)
	                kiusr.dma_buf_fd = dma_buf_fd((struct dma_buf *)kiumd_dmabuf, (O_CLOEXEC));
                else
                        kiusr.dma_buf_fd = -1;
		//printk(KERN_DEBUG "%s:dma_buf_fd %p \n",__func__, kiusr.dma_buf_fd );
	}
        else if (kiusr.dma_buf_fd == -2 && kiusr.dmabuf_ptr > 0)
        {
                //printk(KERN_DEBUG "%s:Closing out the buffer  %pK \n",__func__, kiusr.dmabuf_ptr);
                dma_buf_put((struct dma_buf *)kiusr.dmabuf_ptr);
                kiusr.dma_buf_fd = 0;
                //printk(KERN_DEBUG "%s:dma_buf_fd %p \n",__func__, kiusr.dma_buf_fd );
        }

	if (copy_to_user(arg, &kiusr, sizeof(kiusr))) {
		printk(KERN_DEBUG "%s: copy_to_user failed... \n",__func__);
                return -EFAULT;
	}

        return 0;

}

static int kiumd_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static long kiumd_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct kiumd_dev *ki_dev = (struct kiumd_dev *)file->private_data;
	char __user *argp = (char __user *)arg;
	int err;
	switch (cmd) {
	case KIUMD_SMMU_MAP_BUF:
		err = kiumd_dmabuf_vfio_map(ki_dev, argp);
		break;
	case KIUMD_SMMU_UNMAP_BUF:
		err = kiumd_dmabuf_vfio_unmap(ki_dev, argp);
		break;
	case KIUMD_EXPORT_DMABUF:
		err = kiumd_export_fd(ki_dev, argp);
		break;
	case KIUMD_IMPORT_DMABUF:
		err = kiumd_import_fd(ki_dev, argp);
		break;
	case KIUMD_IOVA_MAP_CTRL:
		err = kiumd_iova_ctrl(ki_dev, argp);
		break;
	case KIUMD_SET_USER_CONTEXT:
		err = kiumd_perprocess_set_user_context(ki_dev, argp);
		break;
	case KIUMD_PER_PROCESS_ALLOC:
		err = kiumd_perprocess_pt_alloc(ki_dev, argp);
		break;
        case KIUMD_PER_PROCESS_SET:
                err = kiumd_perprocess_pgtble_set(ki_dev, argp);
                break;
	case KIUMD_PER_PROCESS_FREE:
		err = kiumd_perprocess_pgtble_free(ki_dev, argp);
		break;
        case KIUMD_FD_DMABUF_HANDLE:
                err = kiumd_fd_dmabuf_handler(ki_dev, argp);
                break;
	case KIUMD_CUSTOM_IOVA_INIT:
		err = kiumd_dmabuf_custom_iova_init(ki_dev, argp);
		break;
        default:
		err = -ENOTTY;
		break;
	}

	return err;
}

static const struct file_operations kiumd_fops = {
	.open = kiumd_open,
	.unlocked_ioctl = kiumd_ioctl,
	.compat_ioctl = kiumd_ioctl,
};

static int kiumd_init(void)
{
	int err;
	char *devname = "kiumd";
	struct kiumd_dev *kidev = NULL;
	kidev = kzalloc(sizeof(struct kiumd_dev), GFP_KERNEL);
	if (!kidev)
		return -ENOMEM;
	kidev->miscdev.minor = MISC_DYNAMIC_MINOR;
	kidev->miscdev.name = devname;
	kidev->miscdev.fops = &kiumd_fops;
	err = misc_register(&kidev->miscdev);
	if (err) {
		pr_err("kiumd misc device creation failure\n");
		return err;
	}

	return 0;
}
module_init(kiumd_init);
MODULE_IMPORT_NS(DMA_BUF);
MODULE_DESCRIPTION("KIUMD");
MODULE_LICENSE("GPL v2");
