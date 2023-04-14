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
#include <linux/qcom_scm.h>

#include "../../drivers/iommu/arm/arm-smmu/arm-smmu.h"

extern int qcom_adreno_smmu_set_ttbr0_cfg(const void *cookie, const struct io_pgtable_cfg *pgtbl_cfg);

/*Global Data structures needed for buffer sharing */
static DEFINE_MUTEX(g_kiumd_lock);
//static DEFINE_HASHTABLE(g_dmabuf_kiumd_table, 10);

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

int kiumd_perprocess_set_user_context(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
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

	iommu_dom = iommu_get_dma_domain(vfio_dev->dev);
	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom->pgtbl_ops == NULL){
		pr_err("%s:pagetable ops is NULL \n",__func__);
		return 0;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if(pgtable == NULL)
		pr_err("%s:pagetable is NULL \n",__func__);

	cbindx = smmu_dom->cfg.cbndx;
	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	/*Allocate a default pagetable for TTBR0 in case per process allocation fails*/
	kismmu_pproc.pgtbl_ops_ptr = (long int)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	cookie = (void*)smmu_dom;
	qcom_adreno_smmu_set_ttbr0_cfg(cookie, &cfg);
	ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);
	if (ret == -EBUSY)
		ret = qcom_scm_kgsl_set_smmu_aperture(cbindx);

	if (ret) {
		pr_err("%s:Setting smmu aperture error \n",__func__);
		return 0;
	}

	return 0;
}

int kiumd_perprocess_pt_alloc(struct kiumd_dev *ki_dev, char __user *arg)
{
	struct kiumd_smmu_user kismmu_pproc;
	struct file *file;
	struct vfio_device *vfio_dev;
	struct io_pgtable_cfg cfg;
	struct arm_smmu_domain *smmu_dom;
	struct io_pgtable *pgtable;
	struct iommu_domain *iommu_dom;

	if (copy_from_user(&kismmu_pproc, arg, sizeof(struct kiumd_smmu_user)))
		return -EFAULT;

	file = fget(kismmu_pproc.vfio_fd);
	vfio_dev = (struct vfio_device *)file->private_data;
	if(vfio_dev == NULL) {
		pr_err("%s:vfio_dev is NULL \n",__func__);
		return -ENOTTY;
	}

	iommu_dom = iommu_get_dma_domain(vfio_dev->dev);
	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom->pgtbl_ops == NULL) {
		pr_err("%s:pagetable ops is NULL \n",__func__);
		return 0;
	}

	pgtable = io_pgtable_ops_to_pgtable(smmu_dom->pgtbl_ops);
	if(pgtable == NULL) {
		pr_err("%s:pagetable is NULL \n",__func__);
		return 0;
	}

	memcpy(&cfg, &pgtable->cfg, sizeof(struct io_pgtable_cfg));
	cfg.quirks &= ~IO_PGTABLE_QUIRK_ARM_TTBR1;
	cfg.tlb = &kgsl_iopgtbl_tlb_ops;
	kismmu_pproc.asid = smmu_dom->cfg.asid;
	kismmu_pproc.pgtbl_ops_ptr = (long int)alloc_io_pgtable_ops(ARM_64_LPAE_S1, &cfg, NULL);
	kismmu_pproc.ttbr0 = cfg.arm_lpae_s1_cfg.ttbr;
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

	iommu_dom = iommu_get_dma_domain(vfio_dev->dev);
	if(iommu_dom == NULL) {
		pr_err("%s:IOMMU domain is NULL \n",__func__);
		return -ENOTTY;
	}

	smmu_dom = container_of(iommu_dom, struct arm_smmu_domain, domain);
	if(smmu_dom == NULL) {
		pr_err("%s:SMMU domain is NULL \n",__func__);
		return -ENOTTY;
	}

	/*Debug changes to validate per process mappings in test case.*/
/*	smmu_cfg = &smmu_dom->cfg;
	cb = &smmu_dom->smmu->cbs[smmu_cfg->cbndx];
	cb->ttbr[0] = kismmu_pproc.ttbr0;
	arm_smmu_write_context_bank(smmu_dom->smmu, cb->cfg->cbndx);*/
	ki_pgtbl_ops = (struct io_pgtable_ops*)kismmu_pproc.pgtbl_ops_ptr;
	smmu_dom->pgtbl_ops = ki_pgtbl_ops;
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
                return -ENOTTY;
        }

        free_io_pgtable_ops(ki_pgtbl_ops);
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
	int kiumd_dma_direction;

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
	if(sgt == NULL) {
		pr_err("%s:sgt is NULL \n",__func__);
		return -ENOTTY;
	}

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

	dmabufattach = (struct dma_buf_attachment *)kiusr.dmabufattach;
        dma_buf_unmap_attachment(dmabufattach, (struct sg_table *)kiusr.sgt_ptr,
							DMA_BIDIRECTIONAL);
	kiumd_dmabuf = (struct dma_buf *)kiusr.dmabuf_ptr;
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
	domain = iommu_get_dma_domain(vfio_dev->dev);
	cookie = (struct kiumd_iommu_dma_cookie*)domain->iova_cookie;
	if(!cookie)	{
		printk(KERN_INFO "kiumd_iova_ctrl: cookie not found\n");
		return -ENOMEM;
	}
	cookie->type = cookie_type;
	cookie->msi_iova = iova_usr;
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
