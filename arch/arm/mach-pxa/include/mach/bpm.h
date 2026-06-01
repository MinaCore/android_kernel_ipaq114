/*
 * Copyright (C) 2003-2004 Intel Corporation.
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html
 *
 *
 * (C) Copyright 2006 Marvell International Ltd.
 * All Rights Reserved
 *
 * (C) Copyright 2008 Borqs Corporation.
 * All Rights Reserved
 */

#ifndef __BPM_H__
#define __BPM_H__

#ifdef __KERNEL__

/* 10 BPM event max */
#define MAX_BPM_EVENT_NUM		10	
#define INFO_SIZE			128

struct bpm_event {
	int type;			/* What type of IPM events. */
	int kind;			/* What kind, or sub-type of events. */
	unsigned char info[INFO_SIZE];	/* events specific data. */
};

/* IPM events queue */
struct bpm_event_queue{
        int head;
        int tail;
        int len;
        struct bpm_event bpmes[MAX_BPM_EVENT_NUM];
        wait_queue_head_t waitq;
};

/* IPM event types. */
#define IPM_EVENT_PROFILER		0x7	/* Profiler events. */

#define IPM_EVENT_BLINK			(0xA0)

/* IPM event kinds. */
#define IPM_EVENT_IDLE_PROFILER		0x1
#define IPM_EVENT_PERF_PROFILER		0x2

#define IPM_EVENT_BLINK_SPEEDUP		(0x1)

/* IPM event infos, not defined yet. */
#define IPM_EVENT_NULLINFO		0x0

/* IPM functions */
extern int bpm_event_notify(int type, int kind, void *info, unsigned int info_len);
#endif

#endif
