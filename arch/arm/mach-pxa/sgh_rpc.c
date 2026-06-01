/**
 * Samsung SGH I900 RPC Driver for MSM6K
 *
 * Copyright (C) 2009 Mustafa Ozsakalli <ozsakalli@hotmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/cdev.h>

#include <asm/uaccess.h>

#include <mach/hardware.h>
#include <mach/sgh_msm6k.h>


#include "devices.h"

static DEFINE_SPINLOCK(msgs_lock);
static DEFINE_SPINLOCK(crc_lock);

static void do_read_data(struct work_struct *work);
static DECLARE_WORK(work_read, do_read_data);
static struct workqueue_struct *workqueue;

struct class *sgh_rpc_class;
dev_t sgh_rpc_devno;

static struct cdev rpc_cdev;
static struct device *rpc_device;

struct __rpc_msg {
	int command;
	int type;
	int index;
	int len;
	int crc;
	char *data;

	struct __rpc_msg *next;
};

static struct __rpc_msg *msgs_head = NULL;
static struct __rpc_msg *msgs_tail = NULL;

static void *rpc_malloc(unsigned sz) {
	 void *ptr = kmalloc(sz, GFP_KERNEL);

	 if(ptr)
		 return ptr;

	 printk(KERN_ERR "sgh_rpc: kmalloc of %d failed, retrying...\n", sz);

	 do {
		 ptr = kmalloc(sz, GFP_KERNEL);
	 } while (!ptr);

	 return ptr;
}

static void do_read_data(struct work_struct *work) {
	struct __rpc_msg *msg;
	int pktlen, rpclen;
	unsigned char end_flag;
	char buf[11];
	unsigned long flags=0;

	msg = (struct __rpc_msg *)rpc_malloc(sizeof(struct __rpc_msg));
	msg->data = NULL;

	if(smd_read(CH_RPC, buf,11) == 0) {
		if(buf[0] != 0x7f) {
			goto cleanup;
		}

		pktlen = (buf[2]<<8)|(buf[1]);
		msg->crc = buf[3];
		rpclen = (buf[5]<<8)|(buf[4]);
		msg->len = rpclen - 7;
		msg->index = (buf[7]<<8)|(buf[6]);
		msg->command = (buf[8]<<8)|(buf[9]);
		msg->type = buf[10];

		if(msg->len > 0) {
			msg->data = (char *)rpc_malloc(msg->len);
			if(smd_read(CH_RPC, msg->data, msg->len) != 0) {
				goto cleanup;
			}
		}

		if(smd_read(CH_RPC, &end_flag,1)!=0 || end_flag!=0x7e){
			goto cleanup;
		}

		spin_lock_irqsave(&msgs_lock, flags);
		if(msgs_tail != NULL)
			msgs_tail->next = msg;
		msgs_tail = msg;
		msgs_tail->next = NULL;
		if(msgs_head == NULL)
			msgs_head = msg;
		spin_unlock_irqrestore(&msgs_lock, flags);

		goto success;

	}

cleanup:
	if(msg->data)
		kfree(msg->data);

	kfree(msg);

success:
	queue_work(workqueue, &work_read);
}

static int write_index = 0xff00;

static char __crc;

static char calc_crc() {
	int64_t m;
	int u, rc;

	m = (__crc+1) * -2130574327; //0x81020409
	u = m>>32;
	u += __crc+1;
	u >>= 6;

	u += ((unsigned)u>>31);
	u += ((unsigned)u<<7);
	u = __crc+1 - u;

	rc = __crc;
	__crc = u;

	return rc;
}

static int rpc_write(unsigned cmd, unsigned type,void *data, unsigned len) {
	char *pkt;
	char *p;
	int crc;
	unsigned long flags=0;
	unsigned n;

	pkt = rpc_malloc(len+11);
	p = pkt;

	n = len + 10;

	*p++ = 0x7f;
	*p++ = n & 0xff;
	*p++ = (n>>8) & 0xff;
	spin_lock_irqsave(&crc_lock, flags);
	crc = calc_crc();
	spin_unlock_irqrestore(&crc_lock, flags);
	*p++ = crc;

	n = len + 7;
	*p++ = (n) & 0xff;
	*p++ = (n>>8) & 0xff;
	*p++ = write_index++ & 0xff; //index
	*p++ = 0xff;
	*p++ = (cmd>>8) & 0xff;
	*p++ = cmd & 0xff;
	*p++ = type & 0xff;

	if(len > 0 && data != NULL) {
		copy_from_user(p, data, len);
		p += len;
	}

	*p++ = 0x7e;

	smd_write(CH_RPC, pkt, (unsigned)(p - pkt));

	kfree(pkt);

	return len;
}


static int rpc_ops_open(struct inode *inode, struct file *filp) {
	int rc;

	rc = nonseekable_open(inode, filp);
	if (rc < 0)
		return rc;

	return 0;
}

static int rpc_ops_release(struct inode *inode, struct file *filp) {
	return 0;
}

static ssize_t rpc_ops_read(struct file *filp, char __user *buf,size_t count, loff_t *ppos) {
	unsigned long flags = 0;
	struct __rpc_msg *msg = NULL;
	int len = 0;


	msg = msgs_head;
	if(msg == NULL) return -EIO;

	spin_lock_irqsave(&msgs_lock, flags);
	msgs_head = msgs_head->next;
	if(msgs_head == NULL)
		msgs_tail = NULL;
	spin_unlock_irqrestore(&msgs_lock, flags);

	if(msg->data != NULL && msg->len > 0) {
		len = count > msg->len ? msg->len : count;
		if(copy_to_user(buf, msg->data, len)!=0)
			len = -EIO;
	}
	if(msg->data != NULL)
		kfree(msg->data);
	kfree(msg);

	return len;
}

static ssize_t rpc_ops_write(struct file *filp, const char __user *buf,size_t count, loff_t *ppos) {
	char h[6];
	short *sh;

	if(copy_from_user(h, buf, 6))
		return 0;

	buf += 6;

	sh = (short *)h;

	return rpc_write(sh[0], sh[1], sh[2]>0 ? buf : NULL, sh[2]);
}

static unsigned int rpc_ops_poll(struct file *filp, struct poll_table_struct *wait) {
	unsigned mask = 0;

	return mask;
}

static long rpc_ops_ioctl(struct file *filp, unsigned int cmd,unsigned long arg) {
	struct __rpc_msg *msg;

	msg = msgs_head;
	if(msg == NULL)
		return -EIO;

	return copy_to_user((void *)arg, msg, 20);
}

static struct file_operations rpc_fops = {
	.owner	 = THIS_MODULE,
	.open	 = rpc_ops_open,
	.release = rpc_ops_release,
	.read	 = rpc_ops_read,
	.write	 = rpc_ops_write,
	//.poll    = rpc_ops_poll,
	.unlocked_ioctl	 = rpc_ops_ioctl,

};


void rpc_init(void) {
	int rc;
	int major;

	smd_init();

	/* Create the device nodes */
	sgh_rpc_class = class_create(THIS_MODULE, "sghrpc");
	if (IS_ERR(sgh_rpc_class)) {
		rc = -ENOMEM;
		printk(KERN_ERR
			       "sgh_rpc: failed to create sghrpc class\n");
		return;
	}

	rc = alloc_chrdev_region(&sgh_rpc_devno, 0, 1, "sghrpc");
	if (rc < 0) {
		printk(KERN_ERR
		       "rpcrouter: Failed to alloc chardev region (%d)\n", rc);
		goto fail_destroy_class;
	}

	major = MAJOR(sgh_rpc_devno);
	rpc_device = device_create(sgh_rpc_class, NULL,
						 sgh_rpc_devno, NULL, "sghrpc%d:%d",
						 0, 0);
	if (IS_ERR(rpc_device)) {
		rc = -ENOMEM;
		goto fail_unregister_cdev_region;
	}

	cdev_init(&rpc_cdev, &rpc_fops);
	rpc_cdev.owner = THIS_MODULE;

	rc = cdev_add(&rpc_cdev, sgh_rpc_devno, 1);
	if (rc < 0)
		goto fail_destroy_device;

	workqueue = create_singlethread_workqueue("sgh-rpc");
	queue_work(workqueue, &work_read);

	return;

fail_destroy_device:
	device_destroy(sgh_rpc_class, sgh_rpc_devno);
fail_unregister_cdev_region:
	unregister_chrdev_region(sgh_rpc_devno, 1);
fail_destroy_class:
	class_destroy(sgh_rpc_class);
}
