/**
 * Support for Samsung SGH I900 MSM6K Shared Memory
 *
 * Copyright (C) 2009 Mustafa Ozsakalli <ozsakalli@hotmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/completion.h>

#include <mach/hardware.h>
#include <mach/sgh_msm6k.h>

#include "devices.h"

static unsigned mmio;
static int smd_initialized;

static DEFINE_SPINLOCK(smd_lock);

#define MEMP(x) (void *)(mmio + x)
#define MEMW(x) *((unsigned short *)(mmio + x))
#define MEML(x) *((unsigned long *)(mmio + x))

#define HEAD(c) MEMW(c.head)
#define TAIL(c) MEMW(c.tail)
#define HEADPTR(c) MEMP(c.base + HEAD(c))
#define TAILPTR(c) MEMP(c.base + TAIL(c))
#define SETTAIL(c,t) TAIL(c)=t; TAIL(c)=t; TAIL(c)=t
#define SETHEAD(c,h) HEAD(c)=h; HEAD(c)=h; HEAD(c)=h
#define AVAIL(h,t,s) t<=h ? h-t : s - (t-h)

struct smd_half_channel {
	unsigned head;
	unsigned tail;
	unsigned base;
	unsigned size;
};

struct smd_channel {
	struct smd_half_channel send;
	struct smd_half_channel recv;
	unsigned head_mask;
	unsigned tail_mask;
	unsigned data_mask;
	unsigned ex_mask;
	wait_queue_head_t wait_recv;
	wait_queue_head_t wait_send;
};

static struct smd_channel smd_channels[] = {
		//msm6k rpc channel
		{

		.send = {
				.head = 0x4,
				.tail = 0x6,
				.base = 0x8,
				.size = 0x3fc,
		},

		.recv = {
				.head = 0x1298,
				.tail = 0x129a,
				.base = 0x129c,
				.size = 0x3fc,
		},

		.head_mask = 0x2,
		.tail_mask = 0x8,
		.data_mask = 0x20,

		},

		{

		.send = {
				.head = 0x404,
				.tail = 0x406,
				.base = 0x408,
				.size = 0xe90,
		},

		.recv = {
				.head = 0x1698,
				.tail = 0x169a,
				.base = 0x169c,
				.size = 0x2950,
		},

		.head_mask = 0x1,
		.tail_mask = 0x4,
		.data_mask = 0x10,

		},

};

void smd_phone_power(int on) {
	if(on){
		gpio_set_value(0x66,1);
		gpio_set_value(0x51,1);
		mdelay(500);
		gpio_set_value(0x51,0);
	} else {
		gpio_set_value(0x66,0);
		mdelay(500);
		gpio_set_value(0x66,1);

	}
}


void smd_init_mem(void)
{
	int i;

	if(smd_initialized)
		return;

	MEML(0x20) = 0;
	MEMW(0x3ffe) = 0x00C1;
	MEML(0x20) = 0;

	MEMW(0x2) = 0;
	MEMW(0x4) = 0;
	MEMW(0x6) = 0;

	for(i = 8; i < 0x404; i += 2)
		MEMW(i) = 0x1111;

	MEMW(0x404) = 0;
	MEMW(0x406) = 0;

    for(i = 0x408; i < 0x1298; i += 2)
    	MEMW(i) = 0x2222;

    MEMW(0x1298) = 0;
    MEMW(0x129A) = 0;

    for(i = 0x129C; i < 0x1698; i += 2)
    	MEMW(i) = 0x3333;

    MEMW(0x1698) = 0;
    MEMW(0x169A) = 0;

    for(i = 0x169C; i < 0x3FEC; i += 2)
    	MEMW(i) = 0x4444;

    if(MEML(0x18) == 0) {
    	MEMW(0) = 0x00AA;
    	MEMW(2) = 0x0001;
     }

    MEMW(0x3ffe) = 0x00C2;
	MEML(0x20) = 0;

	smd_initialized = 1;

    printk("SMD: Initialize Completed\n");
}

static int smd_write_and_check(unsigned adr, void* data, int len) {
	int try;

	for(try=0; try<3; try++){
		memcpy(MEMP(adr), data, len);
		if(memcmp(MEMP(adr), data, len)==0) break;
	}

	if(adr == 0x3FFE)
		MEML(0x20) = 0;

	return try<3 ? 1 : 0;

}

static int smd_read_and_check(unsigned adr, void *data, int len) {
	int try;


	for(try=0;try<3;try++){
		memcpy(data,MEMP(adr),len);
		if(memcmp(data,MEMP(adr),len)==0) break;
	}

	if(try > 2 || adr == 0x3ffc)
		MEML(0x20) = 0;

	//synch problem
	if(try>2) return 0;

	return 1;

}

static int smd_get_mask() {
	unsigned short mask;

	smd_read_and_check(0x3ffc, &mask, 2);

	return mask;
}

static void smd_set_mask(short mask) {
	smd_write_and_check(0x3ffe, &mask, 2);
}

irqreturn_t smd_irq_handler(int irq, void *dev_id){
	unsigned long flags;
	int mask,i;

	//printk("SMD: IRQ fired\n");

	spin_lock_irqsave(&smd_lock, flags);

	mask = smd_get_mask();

	if(!(mask&0x80)) goto done;

	switch(mask & ~0x80){
	case 0x48 :	//initialize
		smd_init_mem();
		break;

	case 0x4A :
		printk("SMD: Phone Deep Sleep??\n");
		/*fire PhoneDeepSleepEvent?*/
		break;
	}


	for(i=0;i<2;i++){
		struct smd_channel *c = &smd_channels[i];
		if(HEAD(c->recv) != TAIL(c->recv))
			mask |= c->head_mask;
	}

	if((mask & 0x2a) != 0)
		smd_channels[0].ex_mask = mask;

	if((mask & 0x15) != 0)
		smd_channels[1].ex_mask = mask;


	for(i=0;i<2;i++){
		struct smd_channel *c = &smd_channels[i];

		if(HEAD(c->recv) != TAIL(c->recv)){
			wake_up(&c->wait_recv);
		}

		if((mask & (0x80 | c->tail_mask)) != 0)
			wake_up(&c->wait_send);
	}

done:
	spin_unlock_irqrestore(&smd_lock, flags);

	return IRQ_HANDLED;
}

int smd_read_avail(struct smd_channel *c) {
	unsigned head, tail;

	if(!smd_initialized)
		return 0;

	head = HEAD(c->recv);
	tail = TAIL(c->recv);

	return AVAIL(head,tail,c->recv.size);
}

int ch_read(struct smd_channel *c, void *_buf, int len) {
	int n;
	int head, tail;
	int orig_len = len;
	unsigned char *buf = _buf;

	if(!smd_initialized) return 0;


	while(len > 0) {
		head = HEAD(c->recv);
		tail = TAIL(c->recv);

		n = tail<=head ? head - tail : c->recv.size - tail;
		if(n==0) break;

		if(n > len) n = len;

		memcpy(buf, TAILPTR(c->recv), n);

		buf += n;
		len -= n;

		tail = (tail + n) % (c->recv.size);
		SETTAIL(c->recv,tail);
	}

	if(orig_len!=len || HEAD(c->recv)==TAIL(c->recv)) {
		int mask = c->ex_mask;
		mask &= c->data_mask;
		if(mask != 0)
			smd_set_mask(0x80 | c->tail_mask);
	}

	return orig_len - len;
}

int ch_write(struct smd_channel *c, void *buf, int len) {
	unsigned head, tail ,mask;
	int n;

	head = HEAD(c->send);
	tail = TAIL(c->send);

	n = (head < tail) ? tail - head :
	    c->send.size - head;


	mask = 0x80;

	if(n > len) n = len;

	if(n > 0) {
		memcpy(HEADPTR(c->send), buf, n);
		head = (head + n) % c->send.size;
		SETHEAD(c->send, head);
		mask |= c->head_mask;
	}
	head = HEAD(c->send);
	tail = TAIL(c->send);

	if(n < len)
		mask |= c->data_mask;

	smd_set_mask(mask);

	return n;
}

struct smd_channel *smd_get_channel(int c) {
	return &smd_channels[c];
}

int smd_read(int ch, void *buf, int len) {
	struct smd_channel *c;
	unsigned long flags;
	int rc;

	c = smd_get_channel(ch);
	for(;;) {
		spin_lock_irqsave(&smd_lock, flags);
		if(smd_read_avail(c) >= len) {
			rc = ch_read(c, buf, len);
			spin_unlock_irqrestore(&smd_lock, flags);
			if(rc == len)
				return 0;
			else
				return -EIO;
		}

		spin_unlock_irqrestore(&smd_lock, flags);
		wait_event(c->wait_recv, smd_read_avail(c) >= len);
	}

	return 0;
}

int smd_write(int ch, void *_buf, int len) {
	struct smd_channel *c;
	unsigned long flags;
	int n;
	char *buf = _buf;

	c = smd_get_channel(ch);
	while(len > 0) {
		spin_lock_irqsave(&smd_lock, flags);
		n = ch_write(c, buf, len);
		spin_unlock_irqrestore(&smd_lock, flags);

		len -= n;
		buf += n;

		wait_event(c->wait_send, len <= 0);
	}

	return 0;
}


void smd_init(void) {
	struct resource *r;
	unsigned short *ram;
	int rc, i;

	gpio_request(0x6b,"dpram");
	gpio_request(0x46,"dpram");
	gpio_request(0x6e,"dpram");
	gpio_request(0x51,"dpram");
	gpio_request(0x66,"dpram");

	gpio_direction_output(0x6b,0);
	gpio_direction_output(0x46,0);
	gpio_direction_output(0x6e,0);
	gpio_direction_output(0x51,1);
	gpio_direction_output(0x66,1);

	smd_phone_power(0);

	gpio_set_value(0x46,0);
	gpio_set_value(0x6b,0);

	r = request_mem_region(0,0x4000,"dpram");
	if(r==NULL){
		printk("SMD: Can't get memory region!\n");
		return;
	}

	mmio = (unsigned long)ioremap(r->start,r->end-r->start+1);

	for(i=0;i<2;i++) {
		init_waitqueue_head(&smd_channels[i].wait_recv);
		init_waitqueue_head(&smd_channels[i].wait_send);
	}

	ram = ((unsigned short *)(mmio));
	//check dpram
	for(i=0;i<0x2000;i++)
		ram[i] = 0;

	ram[0] = 0xaa;
	ram[1] = 1;

	rc = request_irq(IRQ_GPIO(0x46), smd_irq_handler,
			 IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			 "SMD-17", NULL);

	smd_phone_power(0);
	smd_phone_power(0);
}
