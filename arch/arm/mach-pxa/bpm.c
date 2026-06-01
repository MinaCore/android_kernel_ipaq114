/*
 * linux/arch/arm/mach-pxa/bpm.c
 *
 * Provide bpm thread to scale system voltage & frequency dynamically.
 *
 * Copyright (C) 2008 Borqs Corporation.
 *
 * Author:  Emichael Li <emichael.li@borqs.com>
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html
 *
 */

#include <linux/kernel.h>
#include <mach/prm.h>
#include <mach/dvfm.h>
#include <mach/mspm_prof.h>
#include <linux/sysdev.h>
#include <linux/delay.h>
#include <mach/bpm.h>
#include <mach/hardware.h>
#include <mach/pxa3xx-regs.h>
#include <linux/list.h>
#include <asm/io.h>
#include <asm/mach-types.h>
#include <linux/freezer.h>
#include <mach/regs-ost.h>
#ifdef CONFIG_ANDROID_POWER
#include <linux/android_power.h>
#endif

#define DEBUG

#ifdef DEBUG
#define PM_BUG_ON(condition)						      \
	do { 								      \
		if (unlikely(condition)) {				      \
                        printk(KERN_ERR "BUG: failure at %s:%d/%s()!\n",      \
					__FILE__, __LINE__, __FUNCTION__);    \
			WARN_ON(1);  					      \
		}							      \
	} while(0)
#define DPRINTK(fmt,args...) 						      \
	do { 								      \
		if (g_bpm_log_level) 					      \
			printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args);  \
	} while (0)
#else
#define PM_BUG_ON(condition)                                                  \
        do {                                                                  \
                if (unlikely(condition)) {                                    \
                        printk(KERN_ERR "BUG: failure at %s:%d/%s()!\n",      \
                                        __FILE__, __LINE__, __FUNCTION__);    \
                }                                                             \
        } while(0)
#define DPRINTK(fmt,args...) 						      \
	do {} while (0)
#endif

/*****************************************************************************/
/*                                                                           */
/*                         Policy variables                                  */
/*                                                                           */
/*****************************************************************************/
#define REDUCE_624M_DUTYCYCLE			(1)

#define BPM_FREQ_POLICY_NUM			(3)
#define BPM_PROFILER_WINDOW			(100)
#define SYSTEM_BOOTUP_TIME			(15000)
#define BPM_MAX_OP_NUM  			(10)

struct bpm_freq_bonus_arg {
	int mips;
	int mem_stall;
};

struct bpm_freq_policy {
	int lower[BPM_FREQ_POLICY_NUM];
	int higher[BPM_FREQ_POLICY_NUM];
};

#define CONSTRAINT_ID_LEN			(32)
struct bpm_cons {
	struct list_head	list;
	char	sid[CONSTRAINT_ID_LEN];
	int	count;
	unsigned long 	ms;
	unsigned long 	tmp_ms;
	unsigned long 	tm;
};

struct bpm_cons_head {
        struct list_head        list;
};

/* manage all the ops which are supported by the hardware */
static struct dvfm_op g_dyn_ops[BPM_MAX_OP_NUM];
static spinlock_t g_dyn_ops_lock = SPIN_LOCK_UNLOCKED;

static struct bpm_cons_head g_bpm_cons[BPM_MAX_OP_NUM];

/* map the op from active ops to g_dyn_ops[] */
static int g_active_ops_map[BPM_MAX_OP_NUM];
static int g_active_ops_num;
static int g_active_cur_idx = -1;
static int g_prefer_op_idx;
static int g_active_bonus[BPM_MAX_OP_NUM][BPM_MAX_OP_NUM * 2 - 1];
struct bpm_freq_policy g_active_policy[BPM_MAX_OP_NUM];

/*****************************************************************************/
/*                                                                           */
/*                     Framework Supportted Variables                        */
/*                                                                           */
/*****************************************************************************/

int (*pipm_start_pmu) (void *) = NULL;
EXPORT_SYMBOL(pipm_start_pmu);
int (*pipm_stop_pmu)(void) = NULL;
EXPORT_SYMBOL(pipm_stop_pmu);

static int g_bpm_thread_exit;
int g_bpm_enabled;
static wait_queue_head_t g_bpm_enabled_waitq;

static int g_profiler_window = BPM_PROFILER_WINDOW;
static int g_bpm_log_level = 1;
struct completion g_bpm_thread_over;

extern struct sysdev_class cpu_sysdev_class;

static struct bpm_event_queue g_bpm_event_queue;
static spinlock_t g_bpm_event_queue_lock = SPIN_LOCK_UNLOCKED;

#ifdef CONFIG_TEST_BPMD
static int g_cpuload_mode;
#endif

static int dvfm_dev_idx;

extern int __dvfm_enable_op(int index, int dev_idx);
extern int __dvfm_disable_op2(int index, int dev_idx);
extern int cur_op;
extern struct info_head dvfm_trace_list;

extern int g_dvfm_disabled;

#ifdef CONFIG_MTD_NAND_HSS_FIX
extern atomic_t nand_in_cmd;
#endif
/*****************************************************************************/
/*                                                                           */
/*                          Blink Variables                                  */
/*                                                                           */
/*****************************************************************************/
#define DVFM_BLINK_OWNER_LEN (16)

struct dvfm_blink_info {
	int time;
	char name[DVFM_BLINK_OWNER_LEN];
};

static int g_dvfm_blink = 0;
static struct timer_list g_dvfm_blink_timer;
static struct dvfm_blink_info g_dvfm_binfo;
static unsigned long g_dvfm_blink_timeout = 0;

/*****************************************************************************/
/*                                                                           */
/*                          android power interface                          */
/*                                                                           */
/*****************************************************************************/
static int g_android_suspended = 0;

#ifdef CONFIG_ANDROID_POWER
void bpm_android_suspend_handler(android_early_suspend_t *h)
{
	unsigned long flags;
	local_irq_save(flags);
	g_android_suspended = 1;
	local_irq_restore(flags);
}

void bpm_android_resume_handler(android_early_suspend_t *h)
{
	unsigned long flags;
	local_irq_save(flags);
	g_android_suspended = 0;
	local_irq_restore(flags);
}

static android_early_suspend_t bpm_early_suspend = {
	.level = 98,
	.suspend = bpm_android_suspend_handler,
	.resume = bpm_android_resume_handler,
};
#endif

static inline int is_out_d0cs(void)
{
#ifdef CONFIG_PXA3xx_DVFM
	extern int out_d0cs;
	return out_d0cs;
#endif
	return 0;
}

/*****************************************************************************/
/*                                                                           */
/*                          BPMD Event Queue                                 */
/*                                                                           */
/*****************************************************************************/

static int bpmq_init(void)
{
	g_bpm_event_queue.head = g_bpm_event_queue.tail = 0;
	g_bpm_event_queue.len = 0;
	init_waitqueue_head(&g_bpm_event_queue.waitq);
	return 0;
}

static int bpmq_clear(void)
{
	unsigned long flag;

	spin_lock_irqsave(&g_bpm_event_queue_lock, flag);

	g_bpm_event_queue.head = g_bpm_event_queue.tail = 0;
	g_bpm_event_queue.len = 0;

	spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);

	return 0;
}

static int bpmq_get(struct bpm_event *e)
{
	unsigned long flag;

	spin_lock_irqsave(&g_bpm_event_queue_lock, flag);

	if (!g_bpm_event_queue.len) {
		spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);
		printk(KERN_ERR "Logic error, please check bpmq_empty()\n");
		return -1;
	}
	memcpy(e, g_bpm_event_queue.bpmes + g_bpm_event_queue.tail,
	       sizeof(struct bpm_event));
	g_bpm_event_queue.len--;
	g_bpm_event_queue.tail =
	    (g_bpm_event_queue.tail + 1) % MAX_BPM_EVENT_NUM;

	spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);

	return 0;
}

static int bpmq_put(struct bpm_event *e)
{
	unsigned long flag;
	static int err_cnt = 0;

	if (unlikely(0 == g_bpm_enabled))
		return 0;

	spin_lock_irqsave(&g_bpm_event_queue_lock, flag);

	if (g_bpm_event_queue.len == MAX_BPM_EVENT_NUM) {
		if (++err_cnt > 0) {
			printk(KERN_ERR "bpm queue over flow!\n");
			show_state();
			printk(KERN_ERR "send event many times instantly?");
			dump_stack();
		}
		spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);
		return -1;
	}
	memcpy(g_bpm_event_queue.bpmes + g_bpm_event_queue.head, e,
	       sizeof(struct bpm_event));
	g_bpm_event_queue.len++;
	g_bpm_event_queue.head =
	    (g_bpm_event_queue.head + 1) % MAX_BPM_EVENT_NUM;

	spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);

	wake_up_interruptible(&g_bpm_event_queue.waitq);

	return 0;
}

static __inline int bpmq_empty(void)
{
	return (g_bpm_event_queue.len > 0) ? 0 : 1;
}

int bpm_event_notify(int type, int kind, void *info, unsigned int info_len)
{
	struct bpm_event event;
	int len = 0;

	if (info_len > INFO_SIZE)
		len = INFO_SIZE;
	else if ((info_len < INFO_SIZE) && (info_len > 0))
		len = info_len;
	memset(&event, 0, sizeof(struct bpm_event));
	event.type = type;
	event.kind = kind;
	if ((len > 0) && (info != NULL)) {
		memcpy(event.info, info, len);
	}
	if (0 != bpmq_put(&event)) {
		len = -1;
	}

/*	DPRINTK("type: %d kind: %d, len(ret): %d\n", type, kind, len); */
	return len;
}

EXPORT_SYMBOL(bpm_event_notify);

/*****************************************************************************/
/*                                                                           */
/*                               BPMD PMU Interface                          */
/*                                                                           */
/*****************************************************************************/

static int bpm_start_pmu(void)
{
	int ret = -ENXIO;
	struct ipm_profiler_arg pmu_arg;

	if (pipm_start_pmu != NULL) {
		pmu_arg.size = sizeof(struct ipm_profiler_arg);
/*              pmu_arg.flags = IPM_IDLE_PROFILER | IPM_PMU_PROFILER; */
		pmu_arg.flags = IPM_IDLE_PROFILER;
		pmu_arg.window_size = g_profiler_window;

		pmu_arg.pmn0 = PXA3xx_EVENT_EXMEM;
		pmu_arg.pmn1 = PXA3xx_EVENT_DMC_NOT_EMPTY;
		pmu_arg.pmn2 = PMU_EVENT_POWER_SAVING;
		pmu_arg.pmn3 = PMU_EVENT_POWER_SAVING;

		ret = pipm_start_pmu(&pmu_arg);
	} else {
		printk(KERN_CRIT "No profiler\n");
		PM_BUG_ON(1);
	}

	return ret;
}

static int bpm_stop_pmu(void)
{
	pipm_stop_pmu();
	return 0;
}

/*****************************************************************************/
/*                                                                           */
/*                               BPMD POLICY                                 */
/*                                                                           */
/*****************************************************************************/

static int bpm_dump_policy(void)
{
#define TMP_BUF_SIZE (4096)
	int i, j;
	char *buf = kmalloc(TMP_BUF_SIZE, GFP_KERNEL);
	char *s = NULL;

	if (NULL == buf) {
		printk(KERN_ERR "Can not alloc memory\n");
		return 0;
	}

	s = buf;
	memset(s, 0, TMP_BUF_SIZE);

	s += sprintf(s, "--------------BPM DUMP POLICY BEGIN--------------\n");
	s += sprintf(s, "dyn_boot_op = %d\n", dvfm_get_defop());
	s += sprintf(s, "g_active_ops_maps:\n");

	for (i = 0; i < BPM_MAX_OP_NUM; ++i)
		s += sprintf(s, "%8d ", g_active_ops_map[i]);
	s += sprintf(s, "\n");

	s += sprintf(s, "g_active_ops_num: %d\n", g_active_ops_num);
	s += sprintf(s, "g_active_cur_idx: %d\n", g_active_cur_idx);

	s += sprintf(s, "g_active_policy:\n");
	for (i = 0; i < BPM_MAX_OP_NUM; ++i) {
		for (j = 0; j < BPM_FREQ_POLICY_NUM; ++j) {
			s += sprintf(s, "%8d ", g_active_policy[i].lower[j]);
		}

		for (j = 0; j < BPM_FREQ_POLICY_NUM; ++j) {
			s += sprintf(s, "%8d ", g_active_policy[i].higher[j]);
		}
		s += sprintf(s, "\n");
	}

	DPRINTK("%s", buf);

	s = buf;
	memset(s, 0, TMP_BUF_SIZE);

	s += sprintf(s, "g_active_bonus:\n");
	for (i = 0; i < BPM_MAX_OP_NUM; ++i) {
		for (j = 0; j < BPM_MAX_OP_NUM * 2 - 1; ++j) {
			s += sprintf(s, "%8d ", g_active_bonus[i][j]);
		}
		s += sprintf(s, "\n");
	}

	DPRINTK("%s", buf);

	s = buf;
	memset(s, 0, TMP_BUF_SIZE);

	s += sprintf(s, "g_dyn_ops num: %d\n",
		     sizeof(g_dyn_ops) / sizeof(struct dvfm_op));

	s += sprintf(s, "g_dyn_ops:\n");

	for (i = 0; i < sizeof(g_dyn_ops) / sizeof(struct dvfm_op); ++i) {
		s += sprintf(s, "%8d %8d %8d %s\n",
			     g_dyn_ops[i].index,
			     g_dyn_ops[i].count,
			     g_dyn_ops[i].cpu_freq, g_dyn_ops[i].name);
	}
	s += sprintf(s, "--------------BPM DUMP POLICY END----------------\n");

	DPRINTK("%s", buf);

	kfree(buf);
	return 0;
}

static int build_active_ops(void)
{
	int i, j;
	int pre_idx;
	int cur_idx;
	int pre_freq, cur_freq, pre_ratio;
	int m, n;

	memset(g_active_ops_map, -1, sizeof(g_active_ops_map));

	for (i = 0, j = 0; i < BPM_MAX_OP_NUM; ++i) {
		if (g_dyn_ops[i].count == 0 && g_dyn_ops[i].name != NULL
		    && !dvfm_check_active_op(g_dyn_ops[i].index))
			g_active_ops_map[j++] = i;
	}

	g_active_ops_num = j;
	g_active_cur_idx = -1;

	memset(g_active_bonus, -1, sizeof(g_active_bonus));
	memset(g_active_policy, -1, sizeof(g_active_policy));

	for (i = 0; i < g_active_ops_num; ++i) {
		g_active_policy[i].higher[0] = 80;
		g_active_policy[i].higher[1] = 95;
		g_active_policy[i].higher[2] = 100;

		if (i == 0) {
			memset(g_active_policy[i].lower, 0,
			       sizeof(g_active_policy[i].lower));
			cur_idx = g_active_ops_map[i];
			cur_freq = g_dyn_ops[cur_idx].cpu_freq;
			if (cur_freq == 60) {
				g_active_policy[i].higher[0] = 90;
			}
		} else {
			pre_idx = g_active_ops_map[i - 1];
			cur_idx = g_active_ops_map[i];
			pre_freq = g_dyn_ops[pre_idx].cpu_freq;
			cur_freq = g_dyn_ops[cur_idx].cpu_freq;
			pre_ratio = g_active_policy[i - 1].higher[0];

			g_active_policy[i].lower[2] = pre_freq * pre_ratio / cur_freq;

			if (i > 1) {
				pre_idx = g_active_ops_map[i - 2];
				pre_freq = g_dyn_ops[pre_idx].cpu_freq;
				pre_ratio = g_active_policy[i - 2].higher[0];

				g_active_policy[i].lower[1] = pre_freq * pre_ratio / cur_freq; 
			} else {
				g_active_policy[i].lower[1] = 0;	
			}

			g_active_policy[i].lower[0] = 0;
		}

		for (j = 0; j < g_active_ops_num - 1 - i; ++j) {
			g_active_bonus[i][j] = 0;
		}

		m = g_active_ops_num - 1;
		n = 0;
		for (j = m - i; j < 2 * g_active_ops_num - 1; ++j) {
			g_active_bonus[i][j] = n < m ? n : m;
			++n;
		}

	}

	g_active_policy[i - 1].higher[0] = 100;
	g_active_policy[i - 1].higher[1] = 100;
	g_active_policy[i - 1].higher[2] = 100;

#if REDUCE_624M_DUTYCYCLE
	cur_idx = g_active_ops_map[i - 1];
	cur_freq = g_dyn_ops[cur_idx].cpu_freq;
	if (cur_freq == 624) {
		if (i > 1) {
			g_active_policy[i - 2].higher[0] = 96;
			g_active_policy[i - 2].higher[1] = 100;

			pre_idx = g_active_ops_map[i - 2];
			pre_freq = g_dyn_ops[pre_idx].cpu_freq;
			pre_ratio = g_active_policy[i - 2].higher[0];

			g_active_policy[i - 1].lower[2] = pre_freq * pre_ratio / cur_freq; 
		}
		if (i > 2) {
			g_active_policy[i - 3].higher[1] = 100;

			pre_idx = g_active_ops_map[i - 3];
			pre_freq = g_dyn_ops[pre_idx].cpu_freq;
			pre_ratio = g_active_policy[i - 3].higher[0];

			g_active_policy[i - 1].lower[1] = pre_freq * pre_ratio / cur_freq; 
		}
	}
#endif
	return 0;
}

/*****************************************************************************/
/*                                                                           */
/*                               Platform Related                            */
/*                                                                           */
/*****************************************************************************/

int get_op_power_bonus(void)
{
	if (0 == g_active_cur_idx)
		return 1;
	else
		return 0;
}

static int build_dyn_ops(void)
{
	int i;
	int ret;
	int op_num = 0;
	int count, x;

	struct op_info *info = NULL;
	struct op_freq freq;

	op_num = dvfm_op_count();
	PM_BUG_ON(op_num > BPM_MAX_OP_NUM);

	memset(&g_dyn_ops, -1, sizeof(g_dyn_ops));

	for (i = 0; i < op_num; ++i) {
		ret = dvfm_get_opinfo(i, &info);

		PM_BUG_ON(ret);

		/* calculate how much bits is set in device word */
		x = info->device;
		for (count = 0; x; x = x & (x - 1), count++);

		g_dyn_ops[i].index = i;
		g_dyn_ops[i].count = count;

		ret = dvfm_get_op_freq(i, &freq);
		PM_BUG_ON(ret);

		g_dyn_ops[i].cpu_freq = freq.cpu_freq;

		g_dyn_ops[i].name = dvfm_get_op_name(i);

		PM_BUG_ON(!g_dyn_ops[i].name);

		INIT_LIST_HEAD(&(g_bpm_cons[i].list));
	}

	for (i = op_num; i < BPM_MAX_OP_NUM; ++i) {
		g_dyn_ops[i].index = -1;
		g_dyn_ops[i].count = 0;
		g_dyn_ops[i].cpu_freq = 0;
		g_dyn_ops[i].name = NULL;

		INIT_LIST_HEAD(&(g_bpm_cons[i].list));
	}

	return 0;
}

static int get_dyn_idx(int active_idx)
{
	int t;
	t = g_active_ops_map[active_idx];
	return g_dyn_ops[t].index;
}

static int get_cur_freq(void)
{
	PM_BUG_ON(g_active_cur_idx == -1);
	return g_dyn_ops[get_dyn_idx(g_active_cur_idx)].cpu_freq;
}

static int calc_new_idx(int bonus)
{
	int new_idx;

	new_idx =
	    g_active_bonus[g_active_cur_idx][bonus + g_active_ops_num - 1];

	return new_idx;
}

static int calc_bonus(struct bpm_freq_bonus_arg *parg)
{
	int i;
	int bonus = 0;
	int mem_stall = parg->mem_stall;
	int mipsload = parg->mips * 100 / get_cur_freq();
	int cpuload =  mipsload > 100 ? 100 : mipsload;

	PM_BUG_ON(cpuload > 100 || cpuload < 0);

	for (i = 0; i < BPM_FREQ_POLICY_NUM; ++i) {
		if (cpuload > g_active_policy[g_active_cur_idx].higher[i]) {
			bonus += 1;
//			break;	/* FIX ME: change the freq one by one */
		}
	}

	for (i = BPM_FREQ_POLICY_NUM - 1; i >= 0; --i) {
		if (cpuload < g_active_policy[g_active_cur_idx].lower[i]) {
			bonus -= 1;
//			break;	/* FIX ME: change the freq one by one */
		}
	}

	/* memory bound */
	if (bonus <= 0 && mem_stall > 17)
		bonus = 1;

	/* change to user_sleep policy ... */
	if (g_android_suspended && (g_active_cur_idx <= 1))
		bonus -= 1;

	if (bonus > g_active_ops_num - 1)
		bonus = g_active_ops_num - 1;
	else if (bonus < 1 - g_active_ops_num)
		bonus = 1 - g_active_ops_num;

	return bonus;
}

/*****************************************************************************/
/*                                                                           */
/*                               BPMD API                                    */
/*                                                                           */
/*****************************************************************************/

static int bpm_change_op(int cur_idx, int new_idx)
{
	int ret;
	struct dvfm_freqs freqs;
	unsigned int oscr;

	freqs.old = cur_idx;
	freqs.new = new_idx;
	oscr = OSCR;
	ret = dvfm_set_op(&freqs, freqs.new, RELATION_STICK);
	oscr = OSCR - oscr;
	DPRINTK("old: %d cur: %d (tm: %d)\n", cur_idx, new_idx, oscr/325);
/*	
	DPRINTK("ACCR: 0x%x ACSR: 0x%x AVCR: 0x%x SVCR: 0x%x CVCR: 0x%x\n",
			ACCR, ACSR, AVCR, SVCR, CVCR);
*/
	return ret;
}

/* this function need to be refatored later? */
int bpm_disable_op(int dyn_idx, int dev_idx)
{
	int i;
	int ret = 0;
	int cur_op_idx = -1, op_idx;
	int next_op_idx = -1, next_active_idx = -1;

	op_idx = g_dyn_ops[dyn_idx].index;

        /* save current op information */
        if (g_active_cur_idx != -1) {
                cur_op_idx = get_dyn_idx(g_active_cur_idx);
        }

        if (!dvfm_check_active_op(op_idx) && g_active_ops_num == 1 &&
			cur_op_idx == op_idx) {
		printk(KERN_ERR "Can't disable this op %d\n", op_idx);
		bpm_dump_policy();
		return -1;
        }

	/*
 	 * it should be at least two enabled ops here, 
 	 * otherwise it cannot come here if there is one enabled op. 
 	 */
	if ((g_active_cur_idx != -1) && (g_active_ops_num > 1)) {
                if (g_active_cur_idx == (g_active_ops_num - 1)) {
                        next_op_idx = get_dyn_idx(g_active_cur_idx - 1);
                        PM_BUG_ON((g_active_cur_idx - 1) < 0);
                        if ((g_active_cur_idx - 1) < 0) {
                                printk(KERN_ERR "err: %d %d\n", g_active_cur_idx, g_active_ops_num);
                                bpm_dump_policy();
                        }
                } else {
                        next_op_idx = get_dyn_idx(g_active_cur_idx + 1);
                        PM_BUG_ON((g_active_cur_idx + 1) > (g_active_ops_num - 1));
                        if ((g_active_cur_idx + 1) > (g_active_ops_num - 1)) {
                                printk(KERN_ERR "err2: %d %d\n", g_active_cur_idx, g_active_ops_num);
                                bpm_dump_policy();
                        }
                }		
	}
	
	g_dyn_ops[dyn_idx].count++;

	__dvfm_disable_op2(op_idx, dev_idx);

	if (!dvfm_check_active_op(op_idx) && g_dyn_ops[dyn_idx].count == 1) {
		build_active_ops();
	}

	if (cur_op_idx != -1) {
        	for (i = 0; i < g_active_ops_num; ++i) {
			if (get_dyn_idx(i) == cur_op_idx) {
                		g_active_cur_idx = i;
	                        break;
        	        }
       	 	}

		/* the disabled op is previous op, change to another op */
        	if (g_active_cur_idx == -1) {

			/* find next op */
                	for (i = 0; i < g_active_ops_num; ++i) {
                        	if (get_dyn_idx(i) == next_op_idx) {
                                	next_active_idx = i;
	                                break;
        	                }
                	}

              		PM_BUG_ON(cur_op_idx != op_idx);
			PM_BUG_ON(next_op_idx != get_dyn_idx(next_active_idx));
			g_active_cur_idx = next_active_idx;
              		ret = bpm_change_op(cur_op_idx, next_op_idx); 
			PM_BUG_ON(ret);
       		}
	}

	return ret;
}

int bpm_enable_op(int dyn_idx, int dev_idx)
{
	int i, cur_op_idx = -1;

	if (g_dyn_ops[dyn_idx].count <= 0) {
		printk(KERN_ERR "are you disable this op before?\n");
		return -1;
	}

        /* save current op information */
        if (g_active_cur_idx != -1) {
                cur_op_idx = get_dyn_idx(g_active_cur_idx);
        }

	g_dyn_ops[dyn_idx].count--;

	if (g_dyn_ops[dyn_idx].count == 0)
		build_active_ops();

	__dvfm_enable_op(g_dyn_ops[dyn_idx].index, dev_idx);

        if (cur_op_idx != -1) {
                for (i = 0; i < g_active_ops_num; ++i) {
                        if (get_dyn_idx(i) == cur_op_idx) {
                                g_active_cur_idx = i;
                                break;
                        }
                }
	}

	return 0;
}

int bpm_enable_op_name(char *name, int dev_idx, char *sid)
{
	unsigned long flag;
	int ret = 0, new_idx = -1;
	int i, found;
	struct list_head *list = NULL;
	struct bpm_cons *p = NULL;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	for (i = 0; i < sizeof(g_dyn_ops) / sizeof(struct dvfm_op); ++i) {
                if (g_dyn_ops[i].name != NULL && 
                        (!strncmp(name, g_dyn_ops[i].name, sizeof(name)))) {
			ret = bpm_enable_op(i, dev_idx);

                        if (!ret) {
				found = 0;
                                list_for_each(list, &(g_bpm_cons[i].list)) {
                                        p = list_entry(list, struct bpm_cons, list);
                                        if (!strncmp(p->sid, sid, CONSTRAINT_ID_LEN - 1)) {
						found = 1;
						PM_BUG_ON(p->count <= 0);
                                                p->count--;
						if (p->tmp_ms) {
							p->tm++;
							p->ms += (OSCR / 3250 - p->tmp_ms);
						}
						break;
                                        }
                                }
				PM_BUG_ON(!found);
			} else {
				printk(KERN_ERR "%s use PM interface rightly!\n", sid);
				PM_BUG_ON(1);
			}
			break;
		}
	}

	if (i == sizeof(g_dyn_ops) / sizeof(struct dvfm_op)) {
//              printk(KERN_ERR "Cannot find and enable op name %s\n", name);
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	/* Change to prefrer op */
	if (g_prefer_op_idx != cur_op && g_active_cur_idx != -1) {
		for (i = 0; i < g_active_ops_num; ++i) {
			if (get_dyn_idx(i) == g_prefer_op_idx) {
				new_idx = i;
				break;
			}
		}

		if (new_idx != -1) {
			ret = bpm_change_op(get_dyn_idx(g_active_cur_idx), get_dyn_idx(new_idx));
			if (0 == ret)
				g_active_cur_idx = new_idx;
			PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));
		}
	}

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return ret;
}

int bpm_disable_op_name(char *name, int dev_idx, char *sid)
{
	unsigned long flag;
	int ret = -1;
	int i;
	int find = 0;
	struct list_head *list = NULL;
	struct bpm_cons *p = NULL;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	for (i = 0; i < sizeof(g_dyn_ops) / sizeof(struct dvfm_op); ++i) {
		if (g_dyn_ops[i].name != NULL && 
			(!strncmp(name, g_dyn_ops[i].name, sizeof(name)))) {
			ret = bpm_disable_op(i, dev_idx);
			
			if (!ret) {
	        		list_for_each(list, &(g_bpm_cons[i].list)) {
					p = list_entry(list, struct bpm_cons, list);
					if (!strncmp(p->sid, sid, CONSTRAINT_ID_LEN - 1)) {
						p->count++;
						p->tmp_ms = OSCR / 3250;
						find = 1;
						break;
					}
				}
				
				if (find == 0) {
			                p = (struct bpm_cons *)kzalloc(sizeof(struct bpm_cons), GFP_KERNEL);
					strncpy(p->sid, sid, CONSTRAINT_ID_LEN - 1);
					p->count = 1;
					list_add_tail(&(p->list), &(g_bpm_cons[i].list));
				}
			}
			break;
		}
	}

	if (i == sizeof(g_dyn_ops) / sizeof(struct dvfm_op)) {
//              printk(KERN_ERR "Cannot find and disable op name %s\n", name);
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return ret;
}

static int handle_profiler_arg(struct bpm_freq_bonus_arg *parg)
{
	int bonus;
	int new_idx;
	unsigned long flag;
	int cur_dyn_idx, new_dyn_idx;

	if (g_dvfm_blink)
		return 0;

	/*
	 * bpm_enable_op_name() and bpm_disable_op_name() will update
	 * g_dyn_ops[] and g_active_xxx[], and then scale the op, so
	 * we need to avoid the conflict.
	 * Below code can not call schedule() indirectly.
	 */
	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	if (0 == g_bpm_enabled) {
		spin_unlock_irqrestore(&g_dyn_ops_lock, flag);	
		return 0;
	}

	bonus = calc_bonus(parg);
	new_idx = calc_new_idx(bonus);

        cur_dyn_idx = get_dyn_idx(g_active_cur_idx);
        new_dyn_idx = get_dyn_idx(new_idx);

/*
	DPRINTK
	    ("bonus:%d, cur_idx: %d, new_idx: %d, old_hw_idx: %d, new_hw_idx: %d\n",
	     bonus, g_active_cur_idx, new_idx, cur_dyn_idx, new_dyn_idx);
*/
	if (new_idx != g_active_cur_idx) {
		if (!bpm_change_op(cur_dyn_idx, new_dyn_idx)) {
			g_active_cur_idx = new_idx;
		} else {
			DPRINTK("scaling freq later!\n");
		}
		g_prefer_op_idx = new_dyn_idx;
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return 0;
}

static void dvfm_blink_timer_handler(unsigned long data)
{
	unsigned long flag;

	local_irq_save(flag);

	g_dvfm_blink = 0;
	g_dvfm_blink_timeout = 0;
	memset(&g_dvfm_binfo, 0, sizeof(struct dvfm_blink_info));

	local_irq_restore(flag);
}

static int handle_blink(struct bpm_event *pevent)
{
	int new_idx;
	unsigned long flag;
	int cur_dyn_idx, new_dyn_idx;
	struct dvfm_blink_info *pinfo = NULL; 

	if (0 == g_bpm_enabled)
		return 0;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	pinfo = (struct dvfm_blink_info *)pevent->info;

	DPRINTK("Blink: %d %lu %lu\n", g_dvfm_blink, g_dvfm_blink_timeout, jiffies + msecs_to_jiffies(pinfo->time));

	if ((0 == g_dvfm_blink) || time_before(g_dvfm_blink_timeout, jiffies + msecs_to_jiffies(pinfo->time))) {

		memcpy(&g_dvfm_binfo, pinfo, sizeof(struct dvfm_blink_info));

		g_dvfm_blink_timeout = jiffies + msecs_to_jiffies(pinfo->time);
		g_dvfm_blink = 1;
		mod_timer(&g_dvfm_blink_timer, g_dvfm_blink_timeout);

		new_idx = g_active_ops_num - 1;
		cur_dyn_idx = get_dyn_idx(g_active_cur_idx);
		new_dyn_idx = get_dyn_idx(new_idx);

		if (new_dyn_idx > cur_dyn_idx) {
			if (!bpm_change_op(cur_dyn_idx, new_dyn_idx)) {
				g_active_cur_idx = new_idx;
				g_prefer_op_idx = new_dyn_idx;
			}
		}
	} else {
		printk("Blink: %s already set and blink(%lu)\n", g_dvfm_binfo.name, g_dvfm_blink_timeout);
	}
	 
	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return 0;
}

static int handle_profiler(struct bpm_event *pevent)
{
	struct ipm_profiler_result *pinfo =
	    (struct ipm_profiler_result *)pevent->info;
	struct bpm_freq_bonus_arg bonus_arg;
	int mips = pinfo->mips;
	int mem_stall = 0;

#ifdef CONFIG_TEST_BPMD
	static int cpuload = 10;
	switch (g_cpuload_mode) {
	case 0:
		cpuload = mips * 100 / get_cur_freq();
		break;
	case 1:
		cpuload = (cpuload == 10 ? 90 : 10);
		break;
	case 2:
		cpuload = OSCR % 101;
		break;
	case 3:
		cpuload = (OSCR & 0x1) ? 90 : 10;
		break;
	case 4:
		cpuload = OSCR % 21;
		break;
	case 5:
		cpuload = 80 + OSCR % 21;
		break;
	}
	mips = cpuload * get_cur_freq() / 100;

//	DPRINTK("orig ratio: %d new ratio: %d\n", pinfo->busy_ratio, busy);
#endif
	DPRINTK("time_load: %d mips_load: %d (%d)\n", pinfo->busy_ratio, mips * 100 / get_cur_freq(), get_cur_freq());

	/*
	 * Get PMU Data, bla bla bla...  
	 */
	bonus_arg.mips = mips; 
	bonus_arg.mem_stall = mem_stall;

	handle_profiler_arg(&bonus_arg);

	bpm_start_pmu();
	return 0;
}

static int bpm_process_event(struct bpm_event *pevent)
{
	switch (pevent->type) {
	case IPM_EVENT_PROFILER:
		handle_profiler(pevent);
		break;

	case IPM_EVENT_BLINK:
		handle_blink(pevent);
		break;

	default:
		PM_BUG_ON(1);
	}
	return 0;
}

int bpm_pre_enter_d0csidle(int* op)
{
	unsigned long flag;
	int ret = 0, new_dyn_idx;;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	if (g_active_cur_idx != -1)
		*op = get_dyn_idx(g_active_cur_idx);
	else 
		*op = dvfm_get_defop(); 

	new_dyn_idx = get_dyn_idx(0);
	if (*op > new_dyn_idx) {
		ret = bpm_change_op(*op, new_dyn_idx);

		if ((0 == ret) && (-1 != g_active_cur_idx)) {
			g_active_cur_idx = 0;
		}
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

#ifdef CONFIG_MTD_NAND_HSS_FIX	
	if (!atomic_read(&nand_in_cmd))
#endif
		PM_BUG_ON(ret);

	return ret;
}

int bpm_post_exit_d0csidle(int op)
{
	unsigned long flag;
	int new_idx = -1;
	int cur_dyn_op, new_dyn_op;
	int i, ret;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	if (g_active_cur_idx != -1) {
		for (i = 0; i < g_active_ops_num; ++i) {
			if (get_dyn_idx(i) >= op) {
				new_idx = i;
				break;
			}
		}

		PM_BUG_ON(new_idx == -1);

		cur_dyn_op = get_dyn_idx(g_active_cur_idx);
		new_dyn_op = get_dyn_idx(new_idx);

		PM_BUG_ON(cur_dyn_op != cur_op);

		g_active_cur_idx = new_idx;
	} else {
		cur_dyn_op = cur_op;
		new_dyn_op = dvfm_get_defop();
		PM_BUG_ON(op != new_dyn_op);
	}

	PM_BUG_ON(cur_dyn_op > new_dyn_op);

	if (cur_dyn_op != new_dyn_op) {
		ret = bpm_change_op(cur_dyn_op, new_dyn_op);
		PM_BUG_ON(ret);
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return 0;
}

int bpm_set_active_op(const unsigned char* opname)
{
	int opname_idx = -1, i, cur_idx;
	int ret = 0;
	unsigned long flag;

	if (-1 != g_active_cur_idx) {
		spin_lock_irqsave(&g_dyn_ops_lock, flag);	

		for (i = 0; i < g_active_ops_num; ++i) {
			cur_idx = g_active_ops_map[i];
			if (!strcmp(opname, g_dyn_ops[cur_idx].name)) {
				opname_idx = i;
			}
		}

		if(opname_idx != -1) {
			if (g_active_cur_idx != opname_idx) {
				ret = bpm_change_op(get_dyn_idx(g_active_cur_idx), get_dyn_idx(opname_idx));
				g_active_cur_idx = opname_idx;
				g_prefer_op_idx = get_dyn_idx(opname_idx);
				PM_BUG_ON(ret);
			}
		} else 
			printk(KERN_WARNING "Cannot find %s, %s is disabled?\n", opname, opname);

		PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

		spin_unlock_irqrestore(&g_dyn_ops_lock, flag);
	}

	return ret;
}
/*****************************************************************************/
/*                                                                           */
/*                               BPMD Thread                                 */
/*                                                                           */
/*****************************************************************************/

static int change_to_active_op(void)
{
	unsigned long flag;
	int ret = 0;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	g_active_cur_idx = g_active_ops_num - 1;
	ret = bpm_change_op(dvfm_get_defop(), get_dyn_idx(g_active_cur_idx));
	g_prefer_op_idx = cur_op; 

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	PM_BUG_ON(ret);

	return ret;
}

static int change_to_def_op(void)
{
	unsigned long flag;
	int ret = 0;

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	ret = bpm_change_op(get_dyn_idx(g_active_cur_idx), dvfm_get_defop());
	g_prefer_op_idx = cur_op;

	g_active_cur_idx = -1;

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	PM_BUG_ON(ret);

	return ret;
}

static int bpm_start(void)
{
	int ret;

	if (0 == g_bpm_enabled) {
		bpmq_clear();
		change_to_active_op();
		ret = bpm_start_pmu();
		if (ret) {
			printk(KERN_ERR "Can't start_pmu, ret: %d\n", ret);
			g_bpm_enabled = 0;
			return ret;
		}
		g_bpm_enabled = 1;
#ifdef DEBUG
		bpm_dump_policy();
#endif
		wake_up_interruptible(&g_bpm_enabled_waitq);
	} else {
		printk(KERN_DEBUG "bpmd already enabled (%d)\n", g_bpm_enabled);
	}

	return 0;
}

extern int gpio_reset_work_around(void);
static int bpm_stop(void)
{
	if (1 == g_bpm_enabled) {
		bpm_stop_pmu();
		if (machine_is_bstd())
			gpio_reset_work_around();
		else
			change_to_def_op();
		g_bpm_enabled = 0;
	} else {
		printk(KERN_DEBUG "bpmd already stopped (%d)\n", g_bpm_enabled);
	}

	return 0;
}

static int bpm_thread(void *data)
{
	int ret = 0;
	struct bpm_event event;
	struct task_struct *tsk = current;
	struct sched_param param = {.sched_priority = 1 };

	DEFINE_WAIT(wait);
	
	if (g_dvfm_disabled)
		goto thread_over;

	daemonize("bpmd");
	strcpy(tsk->comm, "bpmd");

	allow_signal(SIGKILL);
	sched_setscheduler(tsk, SCHED_FIFO, &param);

	g_bpm_log_level = 0;

	msleep(SYSTEM_BOOTUP_TIME);

	ret = bpm_start();
	PM_BUG_ON(ret);

	DPRINTK("Begining bpm deamon thread ...\n");

	while (likely(!g_bpm_thread_exit)) {

		if (unlikely(signal_pending(tsk))) {
			printk(KERN_NOTICE "BPMD is killed by SIGKILL!\n");
			break;
		}

//		DPRINTK("g_bpm_enabled = %d, bpmq_empty = %d\n",
//			g_bpm_enabled, bpmq_empty());

		if (likely(g_bpm_enabled)) {
			if (likely(bpmq_empty())) {
				prepare_to_wait(&g_bpm_event_queue.waitq, &wait,
						TASK_INTERRUPTIBLE);
				schedule();
				finish_wait(&g_bpm_event_queue.waitq, &wait);
			}

			if (likely(!bpmq_empty())) {
				ret = bpmq_get(&event);
				PM_BUG_ON(ret);

				bpm_process_event(&event);
			}
		} else {
			prepare_to_wait(&g_bpm_enabled_waitq, &wait,
					TASK_INTERRUPTIBLE);
			schedule();
			finish_wait(&g_bpm_enabled_waitq, &wait);
		}
	}

	bpm_stop();

thread_over:	
	complete_and_exit(&g_bpm_thread_over, 0);

	printk(KERN_WARNING "bpm daemon thread exit!\n");
	return 0;
}

/*****************************************************************************/
/*                                                                           */
/*                        BPMD SYS Interface                                 */
/*                                                                           */
/*****************************************************************************/

static ssize_t op_show(struct sys_device *sys_dev, char *buf)
{
	int cur_dyn_idx, len;

	if (g_active_cur_idx != -1)
		cur_dyn_idx = get_dyn_idx(g_active_cur_idx);
	else
		cur_dyn_idx = dvfm_get_defop();

	PM_BUG_ON(cur_dyn_idx != cur_op);

	len = dvfm_dump_op(cur_dyn_idx, buf);

	return len;
}

static ssize_t op_store(struct sys_device *sys_dev, const char *buf, size_t len)
{
	int i;
	int dyn_idx, new_dyn_idx, cur_dyn_idx, new_active_idx = -1;
	unsigned long flag;
	int res = 0;

	sscanf(buf, "%u", &new_dyn_idx);

	spin_lock_irqsave(&g_dyn_ops_lock, flag);

	for (i = 0; i < g_active_ops_num; ++i) {
		dyn_idx = g_active_ops_map[i];
		if (g_dyn_ops[dyn_idx].index == new_dyn_idx) {
			new_active_idx = i;
			break;
		}
	}

	if (new_active_idx != -1) {
	        if (g_active_cur_idx != -1)
                	cur_dyn_idx = get_dyn_idx(g_active_cur_idx);
        	else
	                cur_dyn_idx = dvfm_get_defop();

		res = bpm_change_op(cur_dyn_idx, new_dyn_idx);
		g_prefer_op_idx = new_dyn_idx;

		PM_BUG_ON(res);

		g_active_cur_idx = new_active_idx;
	} else {
		printk(KERN_ERR "bpm is enabled, new dyn op:%d\n", new_dyn_idx);
		printk(KERN_ERR "Cannot find new active op, please check it\n");
	}

	PM_BUG_ON((-1 != g_active_cur_idx) && (get_dyn_idx(g_active_cur_idx) != cur_op));	

	spin_unlock_irqrestore(&g_dyn_ops_lock, flag);

	return len;
}

SYSDEV_ATTR(op, 0644, op_show, op_store);

static ssize_t ops_show(struct sys_device *sys_dev, char *buf)
{
	int len = 0;
	char *p = NULL;
	int i;

	for (i = 0; i < sizeof(g_dyn_ops) / sizeof(struct dvfm_op); ++i) {
		if (g_dyn_ops[i].name != NULL) {
			p = buf + len;
			len += dvfm_dump_op(i, p);
		}
	}

	return len;
}

SYSDEV_ATTR(ops, 0444, ops_show, NULL);

static ssize_t enable_op_show(struct sys_device *sys_dev, char *buf)
{
	int len = 0;
	char *p = NULL;
	int i;

	for (i = 0; i < sizeof(g_dyn_ops) / sizeof(struct dvfm_op); ++i) {
		if ((!g_dyn_ops[i].count) && (g_dyn_ops[i].name != NULL)) {
			p = buf + len;
			len += dvfm_dump_op(i, p);
		}
	}

	return len;
}

static ssize_t enable_op_store(struct sys_device *sys_dev, const char *buf,
			       size_t len)
{
	int level;
	char name[16];

	if (len >= 16) {
		printk(KERN_ERR "invalid parameter\n");
		return len;
	}

	memset(name, 0, sizeof(name));
	sscanf(buf, "%s %d", name, &level);

	if (level)
		bpm_enable_op_name(name, dvfm_dev_idx, "user-echo");
	else
		bpm_disable_op_name(name, dvfm_dev_idx, "user-echo");

	return len;
}

SYSDEV_ATTR(enable_op, 0666, enable_op_show, enable_op_store);

static ssize_t profiler_window_show(struct sys_device *sys_dev, char *buf)
{
	char *s = buf;

	s += sprintf(s, "%d\n", g_profiler_window);

	return (s - buf);
}

static ssize_t profiler_window_store(struct sys_device *sys_dev,
				     const char *buf, size_t n)
{
	sscanf(buf, "%u", &g_profiler_window);

	if (g_profiler_window < 10 || g_profiler_window > 20000)
		printk(KERN_ERR "please input the value in (10, 20000]\n");

	return n;
}

SYSDEV_ATTR(profiler_window, 0644, profiler_window_show, profiler_window_store);

static ssize_t bpm_show(struct sys_device *sys_dev, char *buf)
{
	char *s = buf;

	if (g_bpm_enabled) 
		s += sprintf(s, "%s\n", "enabled");
	else 
		s += sprintf(s, "%s\n", "disabled");

	return (s - buf);
}

static ssize_t bpm_store(struct sys_device *sys_dev, const char *buf, size_t n)
{
	if (n >= strlen("enable") &&
	    strncmp(buf, "enable", strlen("enable")) == 0) {
		bpm_start();
		return n;
	}

	if (n >= strlen("disable") &&
	    strncmp(buf, "disable", strlen("disable")) == 0) {
		bpm_stop();
		return n;
	}

        printk(KERN_ERR "invalid input, please try \"enable\" or \"disable\"\n");
	return n;
}

SYSDEV_ATTR(bpm, 0644, bpm_show, bpm_store);

static ssize_t blink_show(struct sys_device *sys_dev, char *buf)
{
	char *s = buf;

	if (g_dvfm_blink) 
		s += sprintf(s, "blink: %s\n", g_dvfm_binfo.name);
	else
		s += sprintf(s, "blink: no\n");

	return (s - buf);
}

static ssize_t blink_store(struct sys_device *sys_dev, const char *buf, size_t len)
{
	struct dvfm_blink_info binfo;

	if (len >= (DVFM_BLINK_OWNER_LEN - 1)) {
		printk(KERN_ERR "%s sets an invalid parameter of blink\n", current->comm);
		return len;
	}

	memset(binfo.name, 0, sizeof(binfo.name));
	sscanf(buf, "%s %d %*s", binfo.name, &binfo.time);

	DPRINTK("blink: %s %d\n", binfo.name, binfo.time);

	if (binfo.time < 0 || binfo.time > 3000) {
		printk("%s sets an invalid time of blink\n", current->comm);
		return len;
	}

	bpm_event_notify(IPM_EVENT_BLINK, IPM_EVENT_BLINK_SPEEDUP, &binfo, 
			sizeof(struct dvfm_blink_info));

	return len;
}
SYSDEV_ATTR(blink, 0666, blink_show, blink_store);

static ssize_t log_show(struct sys_device *sys_dev, char *buf)
{
        char *s = buf;

        s += sprintf(s, "%d\n", g_bpm_log_level);

        return (s - buf);
}

static ssize_t log_store(struct sys_device *sys_dev, const char *buf, size_t n)
{
        sscanf(buf, "%u", &g_bpm_log_level);

        if (g_bpm_log_level < 0 || g_bpm_log_level > 7) {
                g_bpm_log_level = 0;
                printk(KERN_ERR "invalid command\n");
        }
        return n;
}

SYSDEV_ATTR(log, 0644, log_show, log_store);

static ssize_t cons_show(struct sys_device *sys_dev, char *buf)
{
        char *s = buf;
        struct list_head *list = NULL;
        struct bpm_cons *p = NULL;
	int i;
	unsigned long avg_ms;

	for (i = 0; i < BPM_MAX_OP_NUM; ++i) {
                s += sprintf(s, "op %d: %d\n", i, g_dyn_ops[i].count);
                list_for_each(list, &(g_bpm_cons[i].list)) {
			p = list_entry(list, struct bpm_cons, list);
			if (p->tm)
				avg_ms = p->ms / p->tm; 
			else
				avg_ms = 0;
			s += sprintf(s, "\t%8ld %12ld %8ld %s: %d\n", 
				p->tm, p->ms, avg_ms, p->sid, p->count);
		}
	} 

        return (s - buf);
}

static ssize_t cons_store(struct sys_device *sys_dev, const char *buf, size_t n)
{
	struct list_head *list = NULL;
	struct bpm_cons *p = NULL;
	int i;
	int cons_ctl = 0;

	sscanf(buf, "%u", &cons_ctl);

	if (1 == cons_ctl) {
		for (i = 0; i < BPM_MAX_OP_NUM; ++i) {
			list_for_each(list, &(g_bpm_cons[i].list)) {
				p = list_entry(list, struct bpm_cons, list);
				p->tm = 0;
				p->ms = 0;
				p->tmp_ms = 0;
			}
		}
	}

        return n;
}

SYSDEV_ATTR(cons, 0644, cons_show, cons_store);

/*
 * Dump blocked device on specified OP.
 * And dump the device list that is tracked.
 */
static ssize_t trace_show(struct sys_device *sys_dev, char *buf)
{
	struct op_info *op_entry = NULL;
	struct dvfm_trace_info *entry = NULL;
	int len = 0, i;
	unsigned int blocked_dev;

	for (i = 0; i < op_nums; i++) {
		blocked_dev = 0;
		read_lock(&dvfm_op_list->lock);
		/* op list shouldn't be empty because op_nums is valid */
		list_for_each_entry(op_entry, &dvfm_op_list->list, list) {
			if (op_entry->index == i)
				blocked_dev = op_entry->device;
		}
		read_unlock(&dvfm_op_list->lock);
		if (!blocked_dev)
			continue;

		len += sprintf(buf + len, "Blocked devices on OP%d:", i);
		read_lock(&dvfm_trace_list.lock);
		list_for_each_entry(entry, &dvfm_trace_list.list, list) {
			if (test_bit(entry->index, (void *)&blocked_dev))
				len += sprintf(buf + len, "%s, ", entry->name);
		}
		read_unlock(&dvfm_trace_list.lock);
		len += sprintf(buf + len, "\n");
	}
	if (len == 0)
		len += sprintf(buf + len, "None device block OP\n");
	len += sprintf(buf + len, "Trace device list:\n");
	read_lock(&dvfm_trace_list.lock);
	list_for_each_entry(entry, &dvfm_trace_list.list, list) {
		len += sprintf(buf + len, "%s, ", entry->name);
	}
	read_unlock(&dvfm_trace_list.lock);
	len += sprintf(buf + len, "\n");
	return len;
}
SYSDEV_ATTR(trace, 0444, trace_show, NULL);

static struct attribute *bpm_attr[] = {
	&attr_bpm.attr,
	&attr_profiler_window.attr,
	&attr_op.attr,
	&attr_ops.attr,
	&attr_enable_op.attr,
	&attr_log.attr,
	&attr_cons.attr,
	&attr_blink.attr,
	&attr_trace.attr,
};

static int bpm_add(struct sys_device *sys_dev)
{
	int i, n, ret;
	n = ARRAY_SIZE(bpm_attr);
	for (i = 0; i < n; ++i) {
		ret = sysfs_create_file(&(sys_dev->kobj), bpm_attr[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int bpm_rm(struct sys_device *sys_dev)
{
	int i, n;
	n = ARRAY_SIZE(bpm_attr);
	for (i = 0; i < n; i++) {
		sysfs_remove_file(&(sys_dev->kobj), bpm_attr[i]);
	}
	return 0;
}

static struct sysdev_driver bpm_driver = {
	.add = bpm_add,
	.remove = bpm_rm,
};

#ifdef CONFIG_TEST_BPMD
#include "test_bpm.c"
#endif
/*****************************************************************************/
/*                                                                           */
/*                        BPMD Init & Fini                                   */
/*                                                                           */
/*****************************************************************************/

static int __init bpm_init(void)
{
	unsigned int ret = 0;
	unsigned long flag;

	bpmq_init();

	spin_lock_irqsave(&g_bpm_event_queue_lock, flag);

	build_dyn_ops();
	build_active_ops();

	spin_unlock_irqrestore(&g_bpm_event_queue_lock, flag);

	g_bpm_enabled = 0;
	init_waitqueue_head(&g_bpm_enabled_waitq);

	ret = sysdev_driver_register(&cpu_sysdev_class, &bpm_driver);
	if (ret) {
		printk(KERN_ERR "Can't register bpm sys driver,err:%d\n", ret);
		PM_BUG_ON(1);
	}

#ifdef CONFIG_TEST_BPMD
	ret = sysdev_driver_register(&cpu_sysdev_class, &bpm_test_driver);
	if (ret) {
		printk(KERN_ERR "Can't register bpm test driver,err:%d\n", ret);
		PM_BUG_ON(1);
	}
#endif

	dvfm_register("user-echo", &dvfm_dev_idx);

#ifdef CONFIG_ANDROID_POWER
	android_register_early_suspend(&bpm_early_suspend);
#endif
	init_timer(&g_dvfm_blink_timer);
	g_dvfm_blink_timer.function = dvfm_blink_timer_handler;
	g_dvfm_blink_timer.data = (unsigned long)NULL;

	g_bpm_thread_exit = 0;
	init_completion(&g_bpm_thread_over);
	ret = kernel_thread(bpm_thread, NULL, 0);

	printk(KERN_NOTICE "bpm init finished (%d)\n", ret);
	return 0;
}

static void __exit bpm_exit(void)
{

	g_bpm_thread_exit = 1;

#ifdef CONFIG_ANDROID_POWER	
	android_unregister_early_suspend(&bpm_early_suspend);
#endif
	dvfm_unregister("user-echo", &dvfm_dev_idx);

	g_bpm_enabled = 1;
	wake_up_interruptible(&g_bpm_enabled_waitq);
	wake_up_interruptible(&g_bpm_event_queue.waitq);
	wait_for_completion(&g_bpm_thread_over);
	g_bpm_enabled = 0;
}

module_init(bpm_init);
module_exit(bpm_exit);

MODULE_DESCRIPTION("BPMD");
MODULE_LICENSE("GPL");
