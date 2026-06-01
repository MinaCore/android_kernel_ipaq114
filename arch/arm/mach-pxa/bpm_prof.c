/*
 * PXA3xx IPM Profiler
 *
 * Copyright (C) 2008 Borqs Ltd.
 * Emichael Li <emichael.li@borqs.com>
 *
 * Based on Marvell v6.5 release.
 *
 * Copyright (C) 2008 Marvell Corporation
 * Haojian Zhuang <haojian.zhuang@marvell.com>
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2008 Marvell International Ltd.
 * All Rights Reserved
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/tick.h>
#include <linux/timer.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <mach/hardware.h>
#include <mach/mspm_prof.h>
#include <asm/arch/ipmc.h>
#ifdef CONFIG_PXA3xx_DVFM
#include <asm/arch/dvfm.h>
#include <asm/arch/pxa3xx_dvfm.h>
#endif

extern int (*pipm_start_pmu)(struct ipm_profiler_arg *arg);
extern int (*pipm_stop_pmu)(void);

/* IDLE profiler tune OP with MIPS feature */
#define MSPM_IDLE_PROF_MIPS	0	

#undef MAX_OP_NUM
#define MAX_OP_NUM	10

struct mspm_op_stats {
	int		op;
	int		idle;
	unsigned int	timestamp;
	unsigned int 	jiffies;
};

struct mspm_mips {
	int	mips;
	int	h_thres;	/* high threshold */
	int	l_thres;	/* low threshold */
};

/* Store costed time in run_op_time[] & idle_op_time[] */
static int run_op_time[MAX_OP_NUM], idle_op_time[MAX_OP_NUM];

/*
 * Store OP's MIPS in op_mips[].
 * The lowest frequency OP is the first entry.
 */
static struct mspm_mips op_mips[MAX_OP_NUM];

/* Store the calculated MIPS of last sample window */
static int last_mips;

/*
 * Store the first timestamp of sample window in first_stats
 * Store the current timestamp of sample window in cur_stats
 */
static struct mspm_op_stats first_stats, cur_stats;

/* OP numbers used in IPM IDLE Profiler */
static int mspm_op_num = 0;

static struct timer_list idle_prof_timer;

/* PMU result is stored in it */
static struct pmu_results sum_pmu_res;

static int mspm_prof_enabled = 0;
static int window_jif = 0;
static int mspm_pmu_id;

unsigned int prof_idle_time, prof_time;

static int mspm_prof_notifier_freq(struct notifier_block *nb,
				unsigned long val, void *data);
static struct notifier_block notifier_freq_block = {
	.notifier_call = mspm_prof_notifier_freq,
};

static unsigned int read_time(void)
{
#ifdef CONFIG_PXA_32KTIMER
	return OSCR4;
#else
	return OSCR0;
#endif
}


static int bpm_mod_timer(struct timer_list *timer, unsigned long expires)
{
#ifdef CONFIG_BPMD
	extern void timer_set_deferrable(struct timer_list *timer);
	extern void timer_clr_deferrable(struct timer_list *timer);
	extern int get_op_power_bonus(void);

	if (get_op_power_bonus())
		timer_set_deferrable(timer);
	else 
		timer_clr_deferrable(timer);
#endif	
	mod_timer(timer, expires);

	return 0;
}

/*
 * Record the OP index and RUN/IDLE state.
 */
int mspm_add_event(int op, int cpu_idle)
{
	unsigned int time;

	if (mspm_prof_enabled) {
		time = read_time();
		/* sum the current sample window */
		if (cpu_idle == CPU_STATE_IDLE)
			idle_op_time[cur_stats.op] +=
				time - cur_stats.timestamp;
		else if (cpu_idle == CPU_STATE_RUN)
			run_op_time[cur_stats.op] +=
				time - cur_stats.timestamp;
		/* update start point of current sample window */
		cur_stats.op = op;
		cur_stats.idle = cpu_idle;
		cur_stats.timestamp = time;
		cur_stats.jiffies = jiffies;
	}
	return 0;
}
EXPORT_SYMBOL(mspm_add_event);

/*
 * Prepare to do a new sample.
 * Clear the index in mspm_op_stats table.
 */
static int mspm_do_new_sample(void)
{
	/* clear previous sample window */
	memset(&run_op_time, 0, sizeof(int) * MAX_OP_NUM);
	memset(&idle_op_time, 0, sizeof(int) * MAX_OP_NUM);
	/* prepare for the new sample window */
	first_stats.op = cur_stats.op;
	first_stats.idle = cur_stats.idle;
	first_stats.timestamp = read_time();
	first_stats.jiffies = jiffies;

	prof_idle_time = 0;
	prof_time = read_time();
	return 0;
}

/*
 * Init MIPS of all OP
 */
static int mspm_init_mips(void)
{
	struct op_info *info = NULL;
	struct dvfm_md_opt *md_op = NULL;
	int i, ret;
	memset(&op_mips, 0, MAX_OP_NUM * sizeof(struct mspm_mips));
	mspm_op_num = dvfm_op_count();
#ifdef CONFIG_PXA3xx_DVFM
	for (i = 0; i < mspm_op_num; i++) {
		ret = dvfm_get_opinfo(i, &info);
		if (ret)
			continue;
		md_op = (struct dvfm_md_opt *)info->op;
		op_mips[i].mips = md_op->core;
		if (op_mips[i].mips) {
			op_mips[i].h_thres = DEF_HIGH_THRESHOLD;
			if (!strcmp(md_op->name, "D0CS"))
				op_mips[i].h_thres = 95;
		} else {
			mspm_op_num = i;
			break;
		}
	}
	for (i = 0; i < mspm_op_num - 1; i++)
		op_mips[i + 1].l_thres = op_mips[i].h_thres * op_mips[i].mips
				/ op_mips[i + 1].mips;
#endif
	return 0;
}

/*
 * Calculate the MIPS in sample window
 */
static int mspm_calc_mips(void)
{
	int i;
	unsigned int sum_time = 0, sum = 0;

	/* Calculate total time costed in sample window */
	for (i = 0; i < mspm_op_num; i++) {
		sum_time += run_op_time[i] + idle_op_time[i];
		sum += run_op_time[i] * op_mips[i].mips;
	}
	if (sum_time == 0)
		return 0;

	/*
	 * Calculate MIPS in sample window
	 * Formula: run_op_time[i] / sum_time * op_mips[i].mips
	 */
	return (sum / sum_time);
}

static int is_valid_sample_window(void)
{
	unsigned int time;
	/* The sample window isn't started */
	if (!mspm_prof_enabled)
		goto out;
	time = cur_stats.jiffies - first_stats.jiffies;
	time = jiffies_to_msecs(time);
	if (time >= MIN_SAMPLE_WINDOW)
		return 1;
out:
	return 0;
}

/*
 * When DVFM release one OP, it will invoke this func to get the prefered OP.
 */
static int mspm_get_mips(void)
{
	int ret;
	extern int cur_op;

	mspm_add_event(cur_op, CPU_STATE_RUN);

	if (!is_valid_sample_window()) {
		/* This sample window is invalide, use MIPS value of last
		 * sample window
		 */
		ret = last_mips;
		goto out_sample;
	}
	ret = mspm_calc_mips();
	if (ret < 0)
		goto out_calc;
	return ret;
out_calc:
	printk(KERN_WARNING "Can't calculate MIPS\n");
out_sample:
	return ret;
}

/*
 * Adjust to the most appropriate OP according to MIPS result of
 * sample window
 */
#if MSPM_IDLE_PROF_MIPS
int mspm_tune(void)
{
	int i, mips;
	if (mspm_prof_enabled) {
		for (i = mspm_op_num - 1; i >= 0; i--) {
			mips = mspm_get_mips();
			if (mips >= (op_mips[i].l_thres *
				op_mips[i].mips / 100))
				break;
		}
		dvfm_request_op(i);
	}
	return 0;
}
#else
int mspm_tune(void) { return 0; }
#endif
EXPORT_SYMBOL(mspm_tune);

/***************************************************************************
 * 			Idle Profiler
 ***************************************************************************
 */

static struct ipm_profiler_arg pmu_arg;
static int mspm_start_prof(struct ipm_profiler_arg *arg)
{
	struct pmu_results res;
	struct op_info *info = NULL;

	memset(&sum_pmu_res, 0, sizeof(struct pmu_results));

	/* pmu_arg.window_size stores the number of miliseconds.
	 * window_jif stores the number of jiffies.
	 */
	memset(&pmu_arg, 0, sizeof(struct ipm_profiler_arg));
	pmu_arg.flags = arg->flags;
	if (arg->window_size > 0)
		pmu_arg.window_size = arg->window_size;
	else
		pmu_arg.window_size = DEF_SAMPLE_WINDOW;
	window_jif = msecs_to_jiffies(pmu_arg.window_size);
	if ((mspm_pmu_id > 0) && (pmu_arg.flags & IPM_PMU_PROFILER)) {
		pmu_arg.pmn0 = arg->pmn0;
		pmu_arg.pmn1 = arg->pmn1;
		pmu_arg.pmn2 = arg->pmn2;
		pmu_arg.pmn3 = arg->pmn3;
		/* Collect PMU information */
		if (pmu_stop(&res))
			printk(KERN_WARNING
				"L:%d: pmu_stop failed!\n", __LINE__);
		if (pmu_start(pmu_arg.pmn0, pmu_arg.pmn1, pmu_arg.pmn2,
				pmu_arg.pmn3))
			printk(KERN_WARNING
				"L:%d: pmu_start failed!\n", __LINE__);
	}
	/* start next sample window */
	cur_stats.op = dvfm_get_op(&info);
	cur_stats.idle = CPU_STATE_RUN;
	cur_stats.timestamp = read_time();
	cur_stats.jiffies = jiffies;
	mspm_do_new_sample();
	bpm_mod_timer(&idle_prof_timer, jiffies + window_jif);
	mspm_prof_enabled = 1;
	return 0;
}

static int mspm_stop_prof(void)
{
	struct pmu_results res;
	if ((mspm_pmu_id > 0) && (pmu_arg.flags & IPM_PMU_PROFILER)) {
		if (pmu_stop(&res))
			printk(KERN_WARNING
				"L:%d: pmu_stop failed!\n", __LINE__);
	}
	del_timer(&idle_prof_timer);
	mspm_prof_enabled = 0;
	return 0;
}

static int calc_pmu_res(struct pmu_results *res)
{
	if (res == NULL)
		return -EINVAL;
	sum_pmu_res.ccnt += res->ccnt;
	sum_pmu_res.pmn0 += res->pmn0;
	sum_pmu_res.pmn1 += res->pmn1;
	sum_pmu_res.pmn2 += res->pmn2;
	sum_pmu_res.pmn3 += res->pmn3;
	return 0;
}

/*
 * Pause idle profiler when system enter Low Power mode.
 * Continue it when system exit from Low Power mode.
 */
void set_idletimer(int enable)
{
	struct pmu_results res;
	if (enable && mspm_prof_enabled) {
		/*
		 * Restart the idle profiler because it's only disabled
		 * before entering low power mode.
		 * If we just continue the sample window with left jiffies,
		 * too much OS Timer wakeup exist in system.
		 * Just restart the sample window.
		 */ 
		bpm_mod_timer(&idle_prof_timer, jiffies + window_jif);
		tick_nohz_restart_sched_tick();

		first_stats.jiffies = jiffies;
		first_stats.timestamp = read_time();

		if (pmu_arg.flags & IPM_PMU_PROFILER) {
			if (pmu_start(pmu_arg.pmn0, pmu_arg.pmn1, pmu_arg.pmn2,
						pmu_arg.pmn3)) {
				printk(KERN_WARNING
						"L:%d: pmu_start failed!\n", __LINE__);
			}
		}
	} else if (!enable && mspm_prof_enabled) {
		del_timer(&idle_prof_timer);
		tick_nohz_stop_sched_tick(1);

		if (pmu_arg.flags & IPM_PMU_PROFILER) {
			if (pmu_stop(&res)) {
				printk(KERN_WARNING
						"L:%d: pmu_stop failed!\n", __LINE__);
			} else
				calc_pmu_res(&res);
		}
	}
}
EXPORT_SYMBOL(set_idletimer);

/*
 * Handler of IDLE PROFILER
 */
static void idle_prof_handler(unsigned long data)
{
	struct ipm_profiler_result out_res;
	struct pmu_results res;
	struct op_info *info = NULL;
	int ret, mips, op;

	if (!mspm_prof_enabled)
		return;

	ret = mspm_get_mips();
	if (ret >= 0)
		mips = ret;
	else
		mips = last_mips;
	if ((mspm_pmu_id > 0) && (pmu_arg.flags & IPM_PMU_PROFILER)) {
		if (pmu_stop(&res))
			printk(KERN_WARNING "pmu_stop failed %d\n", __LINE__);
		else
			calc_pmu_res(&res);
		if (pmu_start(pmu_arg.pmn0, pmu_arg.pmn1, pmu_arg.pmn2,
				pmu_arg.pmn3))
			printk(KERN_WARNING "pmu_start failed %d\n", __LINE__);
		memset(&out_res, 0, sizeof(struct ipm_profiler_result));
		out_res.pmu.ccnt = sum_pmu_res.ccnt;
		out_res.pmu.pmn0 = sum_pmu_res.pmn0;
		out_res.pmu.pmn1 = sum_pmu_res.pmn1;
		out_res.pmu.pmn2 = sum_pmu_res.pmn2;
		out_res.pmu.pmn3 = sum_pmu_res.pmn3;
	}
	op = dvfm_get_op(&info);

#if 0	
	/* When system is running, MIPS of current OP won't be zero. */
	out_res.busy_ratio = mips * 100 / op_mips[op].mips;
	out_res.window_size = jiffies_to_msecs(window_jif);
#endif

	prof_time = read_time() - prof_time;

	out_res.busy_ratio = 100 - 100 * prof_idle_time / prof_time;
	out_res.window_size = 0;	/* not used */
	out_res.mips = mips;

	/* send PMU result to policy maker in user space */
	bpm_event_notify(IPM_EVENT_PROFILER, pmu_arg.flags, &out_res,
			sizeof(struct ipm_profiler_result));

#if 0
	/* start next sample window */
	mspm_do_new_sample();
	bpm_mod_timer(&idle_prof_timer, jiffies + window_jif);
	memset(&sum_pmu_res, 0, sizeof(struct pmu_results));
#endif
	last_mips = mips;
}

/*
 * Pause idle profiler when system enter Low Power mode.
 * Continue it when system exit from Low Power mode.
 */
static int mspm_prof_notifier_freq(struct notifier_block *nb,
				unsigned long val, void *data)
{
	struct dvfm_freqs *freqs = (struct dvfm_freqs *)data;
	struct op_info *info = &(freqs->new_info);
	struct dvfm_md_opt *md = NULL;
	struct pmu_results res;

	if (!mspm_prof_enabled)
		return 0;
	md = (struct dvfm_md_opt *)(info->op);
	if (md->power_mode == POWER_MODE_D1 ||
		md->power_mode == POWER_MODE_D2 ||
		md->power_mode == POWER_MODE_CG) {
		switch (val) {
		case DVFM_FREQ_PRECHANGE:
			del_timer(&idle_prof_timer);
			tick_nohz_stop_sched_tick(1);
			if (pmu_arg.flags & IPM_PMU_PROFILER) {
				if (pmu_stop(&res))
					printk(KERN_WARNING
						"L:%d: pmu_stop failed!\n",
						__LINE__);
				else
					calc_pmu_res(&res);
			}
			break;
		case DVFM_FREQ_POSTCHANGE:
			/* Update jiffies and touch watchdog process */
			tick_nohz_update_jiffies();
			/*
			 * Restart the idle profiler because it's only
			 * disabled before entering low power mode.
			 * If we just continue the sample window with
			 * left jiffies, too much OS Timer wakeup exist
			 * in system.
			 * Just restart the sample window.
			 */ 
			bpm_mod_timer(&idle_prof_timer, jiffies + window_jif);
			first_stats.jiffies = jiffies;
			first_stats.timestamp = read_time();
		
			if (pmu_arg.flags & IPM_PMU_PROFILER)
				if (pmu_start(pmu_arg.pmn0, pmu_arg.pmn1,
					pmu_arg.pmn2, pmu_arg.pmn3))
					printk(KERN_WARNING
						"L:%d: pmu_start failed!\n",
						__LINE__);
			break;
		}
	}
	return 0;
}

int __init mspm_prof_init(void)
{
	mspm_pmu_id = pmu_claim();

	memset(&pmu_arg, 0, sizeof(struct ipm_profiler_arg));
	pmu_arg.window_size = DEF_SAMPLE_WINDOW;
	pmu_arg.pmn0 = PMU_EVENT_POWER_SAVING;
	pmu_arg.pmn1 = PMU_EVENT_POWER_SAVING;
	pmu_arg.pmn2 = PMU_EVENT_POWER_SAVING;
	pmu_arg.pmn3 = PMU_EVENT_POWER_SAVING;
	window_jif = msecs_to_jiffies(pmu_arg.window_size);

	pipm_start_pmu = mspm_start_prof;
	pipm_stop_pmu = mspm_stop_prof;

	/* It's used to trigger sample window.
	 * If system is idle, the timer could be deferred.
	 */
	init_timer(&idle_prof_timer);
	idle_prof_timer.function = idle_prof_handler;
	idle_prof_timer.data = 0;

	mspm_init_mips();

	dvfm_register_notifier(&notifier_freq_block,
				DVFM_FREQUENCY_NOTIFIER);

	return 0;
}

void __exit mspm_prof_exit(void)
{
	dvfm_unregister_notifier(&notifier_freq_block,
				DVFM_FREQUENCY_NOTIFIER);

	if (mspm_pmu_id)
		pmu_release(mspm_pmu_id);

	pipm_start_pmu = NULL;
	pipm_stop_pmu = NULL;
}

