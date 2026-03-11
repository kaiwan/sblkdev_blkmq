// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) KBUILD_MODNAME ":%s() " fmt, __func__
#include <linux/hdreg.h>	/* for HDIO_GETGEO */
#include <linux/cdrom.h>	/* for CDROM_GET_CAPABILITY */
#include "device.h"

#ifdef CONFIG_SBLKDEV_REQUESTS_BASED

static inline int process_request(struct request *rq, unsigned int *nr_bytes)
{
	int ret = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	struct sblkdev_device *dev = rq->q->queuedata;
	loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
	loff_t dev_size = (dev->capacity << SECTOR_SHIFT);

	rq_for_each_segment(bvec, rq, iter) {
		unsigned long len = bvec.bv_len;
		void *buf = page_address(bvec.bv_page) + bvec.bv_offset;

		if ((pos + len) > dev_size)
			len = (unsigned long)(dev_size - pos);

		if (rq_data_dir(rq)) {
			memcpy(dev->data + pos, buf, len);	/* WRITE */
			pr_debug("WRITE at sector %llu\n", pos >> SECTOR_SHIFT);
		} else {
			memcpy(buf, dev->data + pos, len);	/* READ */
			pr_debug("READ at sector %llu\n", pos >> SECTOR_SHIFT);
		}

		pos += len;
		*nr_bytes += len;
	}

	return ret;
}

/*
 * This routine is the heart of the request-based blk-mq driver:
 *  'Queue a new request from block IO'.
 * This function is called by the block layer (blk-mq) when a new request is
 * ready for the hardware. We're expected to process the request, first calling
 * blk_mq_start_request(), process it, and complete it by calling
 * blk_mq_end_request() with the appropriate status.
 * 
 * This routine is called and runs in an atomic context! Don't sleep.
 */
static blk_status_t _queue_rq(struct blk_mq_hw_ctx *hctx,
			      const struct blk_mq_queue_data *bd)
{
	unsigned int nr_bytes = 0;
	blk_status_t status = BLK_STS_OK;
	struct request *rq = bd->rq;

	//might_sleep(); // as opposed to...
	cant_sleep();		/* cannot use any locks or blocking calls that may make the thread sleep */

	blk_mq_start_request(rq);

	if (process_request(rq, &nr_bytes))
		status = BLK_STS_IOERR;

	pr_debug("request %llu:%d processed\n", blk_rq_pos(rq), nr_bytes);

	blk_mq_end_request(rq, status);

	return status;
}

static struct blk_mq_ops mq_ops = {
	.queue_rq = _queue_rq,
};

#else				/* CONFIG_SBLKDEV_REQUESTS_BASED */

static inline void process_bio(struct sblkdev_device *dev, struct bio *bio)
{
	struct bio_vec bvec;
	struct bvec_iter iter;
	loff_t pos = bio->bi_iter.bi_sector << SECTOR_SHIFT;
	loff_t dev_size = (dev->capacity << SECTOR_SHIFT);
	unsigned long start_time;

	start_time = bio_start_io_acct(bio);
	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;
		void *buf = page_address(bvec.bv_page) + bvec.bv_offset;

		if ((pos + len) > dev_size) {
			/* len = (unsigned long)(dev_size - pos); */
			bio->bi_status = BLK_STS_IOERR;
			break;
		}

		if (rq_data_dir(rq)) {
			memcpy(dev->data + pos, buf, len);	/* WRITE */
			pr_debug("WRITE at sector %llu\n", pos >> SECTOR_SHIFT);
		} else {
			memcpy(buf, dev->data + pos, len);	/* READ */
			pr_debug("READ at sector %llu\n", pos >> SECTOR_SHIFT);
		}

		pos += len;
	}
	bio_end_io_acct(bio, start_time);
	bio_endio(bio);
}

/*
 * NOTE: submit_bio()'s only invoked for the simpler bio-based drivers. In the
 * modern blk-mq style request-based blk-mq model, _queue_rq() is used instead.
 * (In fact the #ifdef ... covers it)
 */
#ifdef HAVE_QC_SUBMIT_BIO
blk_qc_t _submit_bio(struct bio *bio)
{
	blk_qc_t ret = BLK_QC_T_NONE;
#else
void _submit_bio(struct bio *bio)
{
#endif
#ifdef HAVE_BI_BDEV		// more recent
	struct sblkdev_device *dev = bio->bi_bdev->bd_disk->private_data;
#endif
#ifdef HAVE_BI_BDISK
	struct sblkdev_device *dev = bio->bi_disk->private_data;
#endif

	might_sleep();
	//cant_sleep(); /* cannot use any locks that make the thread sleep */

	process_bio(dev, bio);

#ifdef HAVE_QC_SUBMIT_BIO
	return ret;
}
#else
}
#endif

#endif				/* CONFIG_SBLKDEV_REQUESTS_BASED */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static int _open(struct gendisk *disk, blk_mode_t mode)
{
	struct sblkdev_device *dev = disk->private_data;
#else
static int _open(struct block_device *bdev, fmode_t mode)
{
	struct sblkdev_device *dev = bdev->bd_disk->private_data;
#endif

	if (!dev) {
		pr_err("Invalid disk private_data\n");
		return -ENXIO;
	}

	pr_debug("Device was opened\n");

	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
static void _release(struct gendisk *disk)
{
#else
static void _release(struct gendisk *disk, fmode_t mode)
{
#endif
	struct sblkdev_device *dev = disk->private_data;

	if (!dev) {
		pr_err("Invalid disk private_data\n");
		return;
	}

	pr_debug("Device was closed\n");
}

static inline int ioctl_hdio_getgeo(struct sblkdev_device *dev,
				    unsigned long arg)
{
	struct hd_geometry geo = { 0 };

	geo.start = 0;
	if (dev->capacity > 63) {
		sector_t quotient;

		geo.sectors = 63;
		quotient = (dev->capacity + (63 - 1)) / 63;

		if (quotient > 255) {
			geo.heads = 255;
			geo.cylinders = (unsigned short)
			    ((quotient + (255 - 1)) / 255);
		} else {
			geo.heads = (unsigned char)quotient;
			geo.cylinders = 1;
		}
	} else {
		geo.sectors = (unsigned char)dev->capacity;
		geo.cylinders = 1;
		geo.heads = 1;
	}

	if (copy_to_user((void *)arg, &geo, sizeof(geo)))
		return -EINVAL;

	return 0;
}

static int _ioctl(struct block_device *bdev, fmode_t mode, unsigned int cmd,
		  unsigned long arg)
{
	struct sblkdev_device *dev = bdev->bd_disk->private_data;

	pr_debug("contol command [0x%x] received\n", cmd);

	switch (cmd) {
	case HDIO_GETGEO:
		return ioctl_hdio_getgeo(dev, arg);
	case CDROM_GET_CAPABILITY:
		return -EINVAL;
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
static int _compat_ioctl(struct block_device *bdev, fmode_t mode,
			 unsigned int cmd, unsigned long arg)
{
	// CONFIG_COMPAT is to allow running 32-bit userspace code on a 64-bit kernel
	return -ENOTTY;		// not supported
}
#endif

static const struct block_device_operations fops = {
	.owner = THIS_MODULE,
	.open = _open,
	.release = _release,
	.ioctl = _ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = _compat_ioctl,
#endif
#ifndef CONFIG_SBLKDEV_REQUESTS_BASED
	.submit_bio = _submit_bio,
#endif
};

/*
 * sblkdev_remove() - Remove simple block device
 */
void sblkdev_remove(struct sblkdev_device *dev)
{
	del_gendisk(dev->disk);

#ifdef HAVE_BLK_MQ_ALLOC_DISK
#ifdef HAVE_BLK_CLEANUP_DISK
	blk_cleanup_disk(dev->disk);
#else
	put_disk(dev->disk);
#endif
#else
	blk_cleanup_queue(dev->disk->queue);
	put_disk(dev->disk);
#endif

#ifdef CONFIG_SBLKDEV_REQUESTS_BASED
	blk_mq_free_tag_set(&dev->tag_set);
#endif
	vfree(dev->data);

	kfree(dev);

	pr_info("simple block device was removed\n");
}

#ifdef CONFIG_SBLKDEV_REQUESTS_BASED
/* A critical setup function; here's where we inform the block layer (blk-mq,
 really) about our hardware's 'shape'.
 */
static inline int init_tag_set(struct blk_mq_tag_set *set, void *data)
{
	set->ops = &mq_ops;
	set->nr_hw_queues = 1;
	set->nr_maps = 1;
	set->queue_depth = 128;
	set->numa_node = NUMA_NO_NODE;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
	set->flags = BLK_MQ_F_STACKING;
#else
	set->flags = BLK_MQ_F_SHOULD_MERGE | BLK_MQ_F_STACKING;
#endif
	set->cmd_size = 0;
	set->driver_data = data;

	return blk_mq_alloc_tag_set(set);
}
#endif

//---------------------------------------------------------------------
/* 
 * The two functions below ONLY get used when the BIO-based driver's in play
 * We use the request-based modern blk-mq model by default
 * (can change in Makefile-standalone)
 */
#ifndef HAVE_BLK_MQ_ALLOC_DISK
static inline struct gendisk *blk_mq_alloc_disk(struct blk_mq_tag_set *set,
						void *queuedata)
{
	struct gendisk *disk;
	struct request_queue *queue;

	queue = blk_mq_init_queue(set);
	if (IS_ERR(queue)) {
		pr_err("Failed to allocate queue\n");
		return ERR_PTR(PTR_ERR(queue));
	}

	queue->queuedata = queuedata;
	blk_queue_bounce_limit(queue, BLK_BOUNCE_HIGH);

	disk = alloc_disk(1);
	if (!disk)
		pr_err("Failed to allocate disk\n");
	else
		disk->queue = queue;

	return disk;
}
#endif

#ifndef HAVE_BLK_ALLOC_DISK
static inline struct gendisk *sblkdev_blk_alloc_disk(int node)
{
	struct request_queue *q;
	struct gendisk *disk;

	q = blk_alloc_queue(node);
	if (!q)
		return NULL;

	disk = __alloc_disk_node(0, node);
	if (!disk) {
		blk_cleanup_queue(q);
		return NULL;
	}
	disk->queue = q;

	return disk;
}
#endif
//---------------------------------------------------------------------

/*
 * sblkdev_add() - Add simple block device
 */
struct sblkdev_device *sblkdev_add(int major, int minor, char *name,
				   sector_t capacity)
{
	struct sblkdev_device *dev = NULL;
	int ret = 0;
	struct gendisk *disk;

	pr_info("add device '%s' capacity %llu sectors\n", name, capacity);

	dev = kzalloc(sizeof(struct sblkdev_device), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto fail;
	}

	INIT_LIST_HEAD(&dev->link);
	dev->capacity = capacity;
	dev->data = __vmalloc(capacity << SECTOR_SHIFT, GFP_NOIO | __GFP_ZERO);
	if (!dev->data) {
		ret = -ENOMEM;
		goto fail_kfree;
	}

#ifdef CONFIG_SBLKDEV_REQUESTS_BASED	// true; defined in Makefile-standalone
	ret = init_tag_set(&dev->tag_set, dev);
	if (ret) {
		pr_err("Failed to allocate tag set\n");
		goto fail_vfree;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 9, 0)
	disk = blk_mq_alloc_disk(&dev->tag_set, NULL, dev);	// set the queue_limits to null, so that the default limits will be used
#else
	disk = blk_mq_alloc_disk(&dev->tag_set, dev);	// set the queue_limits to null, so that the default limits will be used
#endif
	if (unlikely(!disk)) {
		ret = -ENOMEM;
		pr_err("Failed to allocate disk\n");
		goto fail_free_tag_set;
	}
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		pr_err("Failed to allocate disk\n");
		goto fail_free_tag_set;
	}

#else
	disk = sblkdev_blk_alloc_disk(NUMA_NO_NODE);
	if (!disk) {
		pr_err("Failed to allocate disk\n");
		ret = -ENOMEM;
		goto fail_vfree;
	}
#endif
	dev->disk = disk;

	/* only one partition */
#ifdef GENHD_FL_NO_PART_SCAN
	disk->flags |= GENHD_FL_NO_PART_SCAN;
#else
	disk->flags |= GENHD_FL_NO_PART;
#endif

	/* removable device */
	/* disk->flags |= GENHD_FL_REMOVABLE; */

	disk->major = major;
	disk->first_minor = minor;	// inx passed via main.c:sblkdev_add()
	disk->minors = 1;

	disk->fops = &fops;

	disk->private_data = dev;

	sprintf(disk->disk_name, name);
	set_capacity(disk, dev->capacity);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
	{
		struct queue_limits lim =
		    queue_limits_start_update(disk->queue);

#ifdef CONFIG_SBLKDEV_BLOCK_SIZE
		lim.physical_block_size = CONFIG_SBLKDEV_BLOCK_SIZE;	// 4096, defined in Makefile-standalone
		lim.logical_block_size = CONFIG_SBLKDEV_BLOCK_SIZE;
		lim.io_min = CONFIG_SBLKDEV_BLOCK_SIZE;
		lim.io_opt = CONFIG_SBLKDEV_BLOCK_SIZE;
#else
		lim.physical_block_size = SECTOR_SIZE;
		lim.logical_block_size = SECTOR_SIZE;
#endif
		lim.max_hw_sectors = BLK_SAFE_MAX_SECTORS;
		queue_limits_commit_update(disk->queue, &lim);
	}
#else
#ifdef CONFIG_SBLKDEV_BLOCK_SIZE
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
	queue_physical_block_size(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);	// 4096, defined in Makefile-standalone
	queue_logical_block_size(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
	queue_io_min(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
	queue_io_opt(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
#else
	blk_queue_physical_block_size(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
	blk_queue_logical_block_size(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
	blk_queue_io_min(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
	blk_queue_io_opt(disk->queue, CONFIG_SBLKDEV_BLOCK_SIZE);
#endif
#else
#if KERNEL_VERSION(6, 11, 0) <= LINUX_VERSION_CODE
	queue_physical_block_size(disk->queue);
	queue_logical_block_size(disk->queue);
	queue_max_hw_sectors(disk->queue);
#else
	blk_queue_physical_block_size(disk->queue, SECTOR_SIZE);
	blk_queue_logical_block_size(disk->queue, SECTOR_SIZE);
	blk_queue_max_hw_sectors(disk->queue, BLK_DEF_MAX_SECTORS);
#endif
#endif
#endif
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, disk->queue);

	/* add_disk() makes the disk 'live'! Userspace can now access it */
#ifdef HAVE_ADD_DISK_RESULT
	ret = add_disk(disk);
	if (ret) {
		pr_err("Failed to add disk '%s'\n", disk->disk_name);
		goto fail_put_disk;
	}
#else
	add_disk(disk);
#endif

	pr_info("Simple block device [%d:%d] was added\n", major, minor);

	return dev;

#ifdef HAVE_ADD_DISK_RESULT
 fail_put_disk:
#ifdef HAVE_BLK_MQ_ALLOC_DISK
#ifdef HAVE_BLK_CLEANUP_DISK
	blk_cleanup_disk(dev->disk);
#else
	put_disk(dev->disk);
#endif
#else
	blk_cleanup_queue(dev->queue);
	put_disk(dev->disk);
#endif
#endif				/* HAVE_ADD_DISK_RESULT */

#ifdef CONFIG_SBLKDEV_REQUESTS_BASED
 fail_free_tag_set:
	blk_mq_free_tag_set(&dev->tag_set);
#endif
 fail_vfree:
	vfree(dev->data);
 fail_kfree:
	kfree(dev);
 fail:
	pr_err("Failed to add block device\n");

	return ERR_PTR(ret);
}
