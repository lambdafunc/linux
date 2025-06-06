// SPDX-License-Identifier: GPL-2.0-or-later
#include <linux/compat.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uacce.h>

static dev_t uacce_devt;
static DEFINE_XARRAY_ALLOC(uacce_xa);

static const struct class uacce_class = {
	.name = UACCE_NAME,
};

/*
 * If the parent driver or the device disappears, the queue state is invalid and
 * ops are not usable anymore.
 */
static bool uacce_queue_is_valid(struct uacce_queue *q)
{
	return q->state == UACCE_Q_INIT || q->state == UACCE_Q_STARTED;
}

static int uacce_start_queue(struct uacce_queue *q)
{
	int ret;

	if (q->state != UACCE_Q_INIT)
		return -EINVAL;

	if (q->uacce->ops->start_queue) {
		ret = q->uacce->ops->start_queue(q);
		if (ret < 0)
			return ret;
	}

	q->state = UACCE_Q_STARTED;
	return 0;
}

static int uacce_put_queue(struct uacce_queue *q)
{
	struct uacce_device *uacce = q->uacce;

	if ((q->state == UACCE_Q_STARTED) && uacce->ops->stop_queue)
		uacce->ops->stop_queue(q);

	if ((q->state == UACCE_Q_INIT || q->state == UACCE_Q_STARTED) &&
	     uacce->ops->put_queue)
		uacce->ops->put_queue(q);

	q->state = UACCE_Q_ZOMBIE;

	return 0;
}

static long uacce_fops_unl_ioctl(struct file *filep,
				 unsigned int cmd, unsigned long arg)
{
	struct uacce_queue *q = filep->private_data;
	struct uacce_device *uacce = q->uacce;
	long ret = -ENXIO;

	/*
	 * uacce->ops->ioctl() may take the mmap_lock when copying arg to/from
	 * user. Avoid a circular lock dependency with uacce_fops_mmap(), which
	 * gets called with mmap_lock held, by taking uacce->mutex instead of
	 * q->mutex. Doing this in uacce_fops_mmap() is not possible because
	 * uacce_fops_open() calls iommu_sva_bind_device(), which takes
	 * mmap_lock, while holding uacce->mutex.
	 */
	mutex_lock(&uacce->mutex);
	if (!uacce_queue_is_valid(q))
		goto out_unlock;

	switch (cmd) {
	case UACCE_CMD_START_Q:
		ret = uacce_start_queue(q);
		break;
	case UACCE_CMD_PUT_Q:
		ret = uacce_put_queue(q);
		break;
	default:
		if (uacce->ops->ioctl)
			ret = uacce->ops->ioctl(q, cmd, arg);
		else
			ret = -EINVAL;
	}
out_unlock:
	mutex_unlock(&uacce->mutex);
	return ret;
}

#ifdef CONFIG_COMPAT
static long uacce_fops_compat_ioctl(struct file *filep,
				   unsigned int cmd, unsigned long arg)
{
	arg = (unsigned long)compat_ptr(arg);

	return uacce_fops_unl_ioctl(filep, cmd, arg);
}
#endif

static int uacce_bind_queue(struct uacce_device *uacce, struct uacce_queue *q)
{
	u32 pasid;
	struct iommu_sva *handle;

	if (!(uacce->flags & UACCE_DEV_SVA))
		return 0;

	handle = iommu_sva_bind_device(uacce->parent, current->mm);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	pasid = iommu_sva_get_pasid(handle);
	if (pasid == IOMMU_PASID_INVALID) {
		iommu_sva_unbind_device(handle);
		return -ENODEV;
	}

	q->handle = handle;
	q->pasid = pasid;
	return 0;
}

static void uacce_unbind_queue(struct uacce_queue *q)
{
	if (!q->handle)
		return;
	iommu_sva_unbind_device(q->handle);
	q->handle = NULL;
}

static int uacce_fops_open(struct inode *inode, struct file *filep)
{
	struct uacce_device *uacce;
	struct uacce_queue *q;
	int ret;

	uacce = xa_load(&uacce_xa, iminor(inode));
	if (!uacce)
		return -ENODEV;

	q = kzalloc(sizeof(struct uacce_queue), GFP_KERNEL);
	if (!q)
		return -ENOMEM;

	mutex_lock(&uacce->mutex);

	if (!uacce->parent) {
		ret = -EINVAL;
		goto out_with_mem;
	}

	ret = uacce_bind_queue(uacce, q);
	if (ret)
		goto out_with_mem;

	q->uacce = uacce;

	if (uacce->ops->get_queue) {
		ret = uacce->ops->get_queue(uacce, q->pasid, q);
		if (ret < 0)
			goto out_with_bond;
	}

	init_waitqueue_head(&q->wait);
	filep->private_data = q;
	q->state = UACCE_Q_INIT;
	q->mapping = filep->f_mapping;
	mutex_init(&q->mutex);
	list_add(&q->list, &uacce->queues);
	mutex_unlock(&uacce->mutex);

	return 0;

out_with_bond:
	uacce_unbind_queue(q);
out_with_mem:
	kfree(q);
	mutex_unlock(&uacce->mutex);
	return ret;
}

static int uacce_fops_release(struct inode *inode, struct file *filep)
{
	struct uacce_queue *q = filep->private_data;
	struct uacce_device *uacce = q->uacce;

	mutex_lock(&uacce->mutex);
	uacce_put_queue(q);
	uacce_unbind_queue(q);
	list_del(&q->list);
	mutex_unlock(&uacce->mutex);
	kfree(q);

	return 0;
}

static void uacce_vma_close(struct vm_area_struct *vma)
{
	struct uacce_queue *q = vma->vm_private_data;

	if (vma->vm_pgoff < UACCE_MAX_REGION) {
		struct uacce_qfile_region *qfr = q->qfrs[vma->vm_pgoff];

		mutex_lock(&q->mutex);
		q->qfrs[vma->vm_pgoff] = NULL;
		mutex_unlock(&q->mutex);
		kfree(qfr);
	}
}

static const struct vm_operations_struct uacce_vm_ops = {
	.close = uacce_vma_close,
};

static int uacce_fops_mmap(struct file *filep, struct vm_area_struct *vma)
{
	struct uacce_queue *q = filep->private_data;
	struct uacce_device *uacce = q->uacce;
	struct uacce_qfile_region *qfr;
	enum uacce_qfrt type = UACCE_MAX_REGION;
	int ret = 0;

	if (vma->vm_pgoff < UACCE_MAX_REGION)
		type = vma->vm_pgoff;
	else
		return -EINVAL;

	qfr = kzalloc(sizeof(*qfr), GFP_KERNEL);
	if (!qfr)
		return -ENOMEM;

	vm_flags_set(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_WIPEONFORK);
	vma->vm_ops = &uacce_vm_ops;
	vma->vm_private_data = q;
	qfr->type = type;

	mutex_lock(&q->mutex);
	if (!uacce_queue_is_valid(q)) {
		ret = -ENXIO;
		goto out_with_lock;
	}

	if (q->qfrs[type]) {
		ret = -EEXIST;
		goto out_with_lock;
	}

	switch (type) {
	case UACCE_QFRT_MMIO:
	case UACCE_QFRT_DUS:
		if (!uacce->ops->mmap) {
			ret = -EINVAL;
			goto out_with_lock;
		}

		ret = uacce->ops->mmap(q, vma, qfr);
		if (ret)
			goto out_with_lock;
		break;

	default:
		ret = -EINVAL;
		goto out_with_lock;
	}

	q->qfrs[type] = qfr;
	mutex_unlock(&q->mutex);

	return ret;

out_with_lock:
	mutex_unlock(&q->mutex);
	kfree(qfr);
	return ret;
}

static __poll_t uacce_fops_poll(struct file *file, poll_table *wait)
{
	struct uacce_queue *q = file->private_data;
	struct uacce_device *uacce = q->uacce;
	__poll_t ret = 0;

	mutex_lock(&q->mutex);
	if (!uacce_queue_is_valid(q))
		goto out_unlock;

	poll_wait(file, &q->wait, wait);

	if (uacce->ops->is_q_updated && uacce->ops->is_q_updated(q))
		ret = EPOLLIN | EPOLLRDNORM;

out_unlock:
	mutex_unlock(&q->mutex);
	return ret;
}

static const struct file_operations uacce_fops = {
	.owner		= THIS_MODULE,
	.open		= uacce_fops_open,
	.release	= uacce_fops_release,
	.unlocked_ioctl	= uacce_fops_unl_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= uacce_fops_compat_ioctl,
#endif
	.mmap		= uacce_fops_mmap,
	.poll		= uacce_fops_poll,
};

#define to_uacce_device(dev) container_of(dev, struct uacce_device, dev)

static ssize_t api_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%s\n", uacce->api_ver);
}

static ssize_t flags_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%u\n", uacce->flags);
}

static ssize_t available_instances_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	if (!uacce->ops->get_available_instances)
		return -ENODEV;

	return sysfs_emit(buf, "%d\n",
		       uacce->ops->get_available_instances(uacce));
}

static ssize_t algorithms_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%s\n", uacce->algs);
}

static ssize_t region_mmio_size_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%lu\n",
		       uacce->qf_pg_num[UACCE_QFRT_MMIO] << PAGE_SHIFT);
}

static ssize_t region_dus_size_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%lu\n",
		       uacce->qf_pg_num[UACCE_QFRT_DUS] << PAGE_SHIFT);
}

static ssize_t isolate_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	return sysfs_emit(buf, "%d\n", uacce->ops->get_isolate_state(uacce));
}

static ssize_t isolate_strategy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct uacce_device *uacce = to_uacce_device(dev);
	u32 val;

	val = uacce->ops->isolate_err_threshold_read(uacce);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t isolate_strategy_store(struct device *dev, struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct uacce_device *uacce = to_uacce_device(dev);
	unsigned long val;
	int ret;

	if (kstrtoul(buf, 0, &val) < 0)
		return -EINVAL;

	if (val > UACCE_MAX_ERR_THRESHOLD)
		return -EINVAL;

	ret = uacce->ops->isolate_err_threshold_write(uacce, val);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_RO(api);
static DEVICE_ATTR_RO(flags);
static DEVICE_ATTR_RO(available_instances);
static DEVICE_ATTR_RO(algorithms);
static DEVICE_ATTR_RO(region_mmio_size);
static DEVICE_ATTR_RO(region_dus_size);
static DEVICE_ATTR_RO(isolate);
static DEVICE_ATTR_RW(isolate_strategy);

static struct attribute *uacce_dev_attrs[] = {
	&dev_attr_api.attr,
	&dev_attr_flags.attr,
	&dev_attr_available_instances.attr,
	&dev_attr_algorithms.attr,
	&dev_attr_region_mmio_size.attr,
	&dev_attr_region_dus_size.attr,
	&dev_attr_isolate.attr,
	&dev_attr_isolate_strategy.attr,
	NULL,
};

static umode_t uacce_dev_is_visible(struct kobject *kobj,
				    struct attribute *attr, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct uacce_device *uacce = to_uacce_device(dev);

	if (((attr == &dev_attr_region_mmio_size.attr) &&
	    (!uacce->qf_pg_num[UACCE_QFRT_MMIO])) ||
	    ((attr == &dev_attr_region_dus_size.attr) &&
	    (!uacce->qf_pg_num[UACCE_QFRT_DUS])))
		return 0;

	if (attr == &dev_attr_isolate_strategy.attr &&
	    (!uacce->ops->isolate_err_threshold_read &&
	     !uacce->ops->isolate_err_threshold_write))
		return 0;

	if (attr == &dev_attr_isolate.attr && !uacce->ops->get_isolate_state)
		return 0;

	return attr->mode;
}

static struct attribute_group uacce_dev_group = {
	.is_visible	= uacce_dev_is_visible,
	.attrs		= uacce_dev_attrs,
};

__ATTRIBUTE_GROUPS(uacce_dev);

static void uacce_release(struct device *dev)
{
	struct uacce_device *uacce = to_uacce_device(dev);

	kfree(uacce);
}

/**
 * uacce_alloc() - alloc an accelerator
 * @parent: pointer of uacce parent device
 * @interface: pointer of uacce_interface for register
 *
 * Returns uacce pointer if success and ERR_PTR if not
 * Need check returned negotiated uacce->flags
 */
struct uacce_device *uacce_alloc(struct device *parent,
				 struct uacce_interface *interface)
{
	unsigned int flags = interface->flags;
	struct uacce_device *uacce;
	int ret;

	uacce = kzalloc(sizeof(struct uacce_device), GFP_KERNEL);
	if (!uacce)
		return ERR_PTR(-ENOMEM);

	uacce->parent = parent;
	uacce->flags = flags;
	uacce->ops = interface->ops;

	ret = xa_alloc(&uacce_xa, &uacce->dev_id, uacce, xa_limit_32b,
		       GFP_KERNEL);
	if (ret < 0)
		goto err_with_uacce;

	INIT_LIST_HEAD(&uacce->queues);
	mutex_init(&uacce->mutex);
	device_initialize(&uacce->dev);
	uacce->dev.devt = MKDEV(MAJOR(uacce_devt), uacce->dev_id);
	uacce->dev.class = &uacce_class;
	uacce->dev.groups = uacce_dev_groups;
	uacce->dev.parent = uacce->parent;
	uacce->dev.release = uacce_release;
	dev_set_name(&uacce->dev, "%s-%d", interface->name, uacce->dev_id);

	return uacce;

err_with_uacce:
	kfree(uacce);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(uacce_alloc);

/**
 * uacce_register() - add the accelerator to cdev and export to user space
 * @uacce: The initialized uacce device
 *
 * Return 0 if register succeeded, or an error.
 */
int uacce_register(struct uacce_device *uacce)
{
	if (!uacce)
		return -ENODEV;

	uacce->cdev = cdev_alloc();
	if (!uacce->cdev)
		return -ENOMEM;

	uacce->cdev->ops = &uacce_fops;
	uacce->cdev->owner = THIS_MODULE;

	return cdev_device_add(uacce->cdev, &uacce->dev);
}
EXPORT_SYMBOL_GPL(uacce_register);

/**
 * uacce_remove() - remove the accelerator
 * @uacce: the accelerator to remove
 */
void uacce_remove(struct uacce_device *uacce)
{
	struct uacce_queue *q, *next_q;

	if (!uacce)
		return;

	/*
	 * uacce_fops_open() may be running concurrently, even after we remove
	 * the cdev. Holding uacce->mutex ensures that open() does not obtain a
	 * removed uacce device.
	 */
	mutex_lock(&uacce->mutex);
	/* ensure no open queue remains */
	list_for_each_entry_safe(q, next_q, &uacce->queues, list) {
		/*
		 * Taking q->mutex ensures that fops do not use the defunct
		 * uacce->ops after the queue is disabled.
		 */
		mutex_lock(&q->mutex);
		uacce_put_queue(q);
		mutex_unlock(&q->mutex);
		uacce_unbind_queue(q);

		/*
		 * unmap remaining mapping from user space, preventing user still
		 * access the mmaped area while parent device is already removed
		 */
		unmap_mapping_range(q->mapping, 0, 0, 1);
	}

	if (uacce->cdev)
		cdev_device_del(uacce->cdev, &uacce->dev);
	xa_erase(&uacce_xa, uacce->dev_id);
	/*
	 * uacce exists as long as there are open fds, but ops will be freed
	 * now. Ensure that bugs cause NULL deref rather than use-after-free.
	 */
	uacce->ops = NULL;
	uacce->parent = NULL;
	mutex_unlock(&uacce->mutex);
	put_device(&uacce->dev);
}
EXPORT_SYMBOL_GPL(uacce_remove);

static int __init uacce_init(void)
{
	int ret;

	ret = class_register(&uacce_class);
	if (ret)
		return ret;

	ret = alloc_chrdev_region(&uacce_devt, 0, MINORMASK, UACCE_NAME);
	if (ret)
		class_unregister(&uacce_class);

	return ret;
}

static __exit void uacce_exit(void)
{
	unregister_chrdev_region(uacce_devt, MINORMASK);
	class_unregister(&uacce_class);
}

subsys_initcall(uacce_init);
module_exit(uacce_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("HiSilicon Tech. Co., Ltd.");
MODULE_DESCRIPTION("Accelerator interface for Userland applications");
