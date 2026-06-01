/*
 * PXA3xx DVFM Driver
 *
 * Copyright (C) 2007 Marvell Corporation
 * Haojian Zhuang <haojian.zhuang@marvell.com>
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2007 Marvell International Ltd.
 * All Rights Reserved
 */

#define DEBUG

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sysdev.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <asm/uaccess.h>
//#include <asm/arch/pxa-regs.h>
#include <mach/pxa3xx-regs.h>
#include <mach/pxa3xx_pmic.h>
#include <mach/hardware.h>
#include <mach/dvfm.h>
#include <mach/pxa3xx_dvfm.h>
#include <asm/io.h>
//#include <asm/arch/pxa_ispt.h>
//#include <asm/arch/mspm_prof.h>

#include "devices.h"

#ifdef CONFIG_CPU_PXA310
#define FREQ_CORE(xl, xn)	((xl)*(xn)*13)

#define FREQ_SRAM(sflfs)	(((sflfs) == 0x0)?104:	\
				((sflfs) == 0x1)?156:	\
				((sflfs) == 0x2)?208:312)

#define FREQ_STMM(smcfs)	(((smcfs) == 0x0)?78:	\
				((smcfs) == 0x2)?104:	\
				((smcfs) == 0x5)?208:0)

#define FREQ_DDR(dmcfs)		(((dmcfs) == 0x0)?26:	\
				((dmcfs) == 0x2)?208:	\
				((dmcfs) == 0x3)?260:0)

#define FREQ_HSS(hss)		(((hss) == 0x0)?104:	\
				((hss) == 0x1)?156:	\
				((hss) == 0x2)?208:0)

#define FREQ_DFCLK(smcfs, df_clkdiv)    				\
			(((df_clkdiv) == 0x1)?FREQ_STMM((smcfs)):	\
			((df_clkdiv) == 0x2)?FREQ_STMM((smcfs))/2:	\
			((df_clkdiv) == 0x3)?FREQ_STMM((smcfs))/4:0)

#define FREQ_EMPICLK(smcfs, empi_clkdiv)				\
			(((empi_clkdiv) == 0x1)?FREQ_STMM((smcfs)):	\
			((empi_clkdiv) == 0x2)?FREQ_STMM((smcfs))/2:	\
			((empi_clkdiv) == 0x3)?FREQ_STMM((smcfs))/4:0)

#define LPJ_PER_MHZ 4988
#endif

/* Enter D2 before exiting D0CS */
#define DVFM_LP_SAFE

struct pxa3xx_dvfm_info {
	/* flags */
	uint32_t 	       flags;

	/* CPU ID */
	uint32_t	       cpuid;

	/* LCD clock */
	struct clk		*lcd_clk;

	/* clock manager register base */
	unsigned char __iomem	*clkmgr_base;

	/* service power management unit */
	unsigned char __iomem	*spmu_base;

	/* slave power management unit */
	unsigned char __iomem	*bpmu_base;

	/* dynamic memory controller register base */
	unsigned char __iomem	*dmc_base;

	/* static memory controller register base */
	unsigned char __iomem	*smc_base;
};

#define MIN_SAFE_FREQUENCY	624

struct info_head pxa3xx_dvfm_op_list = {
	.list = LIST_HEAD_INIT(pxa3xx_dvfm_op_list.list),
	.lock = RW_LOCK_UNLOCKED,
};

#ifdef CONFIG_PXA3xx_DVFM_STATS

static unsigned int switch_lowpower_before, switch_lowpower_after;

static int pxa3xx_stats_notifier_freq(struct notifier_block *nb,
				unsigned long val, void *data);
static struct notifier_block notifier_freq_block = {
	.notifier_call = pxa3xx_stats_notifier_freq,
};
#endif

/* the operating point preferred by policy maker or user */
static int preferred_op;
static int current_op;

extern unsigned int cur_op;		/* current operating point */
extern unsigned int def_op;		/* default operating point */

extern int enter_d0cs_a(volatile u32 *, volatile u32 *);
extern int exit_d0cs_a(volatile u32 *, volatile u32 *);
extern int md2fvinfo(struct pxa3xx_fv_info *, struct dvfm_md_opt *);
extern void set_idle_op(int, int);

#ifdef CONFIG_FB_PXA
extern void pxafb_set_pcd(void);
#else
static void pxafb_set_pcd(void) {}
#endif

static int dvfm_dev_id;
#define LPJ_D0CS (293888 * 100 / HZ)
#define LPJ_104M (517120 * 100 / HZ)
#define LPJ_156M (778128 * 100 / HZ)
#define LPJ_208M (1036288 * 100 / HZ)
#define LPJ_416M (2076672 * 100 / HZ)
#define LPJ_624M (3112960 * 100 / HZ)
#define LPJ_806M (4020906 * 100 / HZ)

static int d0cs_lpj = LPJ_D0CS;

static int boot_core_freq = 0;

int out_d0cs = 0;

/* define the operating point of S0D0 and S0D0CS mode */
static struct dvfm_md_opt pxa300_op_array[] = {
	/* 60MHz -- ring oscillator */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 0,
		.xn = 0,
		.smcfs = 15,
		.sflfs = 60,
		.hss = 60,
		.dmcfs = 30, /* will be 60MHZ for PXA310 A2 and PXA935/PXA940 */
		.df_clk = 15,
		.empi_clk = 15,
		.power_mode = POWER_MODE_D0CS,
		.flag = OP_FLAG_FACTORY,
		.lpj = 293888*100/HZ,
		.name = "D0CS",
	},
	/* 104MHz */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 8,
		.xn = 1,
		.smcfs = 78,
		.sflfs = 104,
		.hss = 104,
		.dmcfs = 260,
		/* Actually it's 19.5, not 19 */
		.df_clk = 19,
		.empi_clk = 19,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 517120*100/HZ,
		.name = "104M",
	},
	/* 208MHz */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 16,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 1036288*100/HZ,
		.name = "208M",
	},
	/* 416MHz */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.xl = 16,
		.xn = 2,
		.smcfs = 104,
		.sflfs = 208,
		.hss = 156,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 2076672*100/HZ,
		.name = "416M",
	},
	/* 624MHz */
	{
		.vcc_core = 1375,
		.vcc_sram = 1400,
		.xl = 24,
		.xn = 2,
		.smcfs = 208,
		.sflfs = 312,
		.hss = 208,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 3112960*100/HZ,
		.name = "624M",
	},
	/* D1 mode */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.power_mode = POWER_MODE_D1,
		.flag = OP_FLAG_FACTORY,
		.name = "D1",
	},
	/* D2 mode */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.power_mode = POWER_MODE_D2,
		.flag = OP_FLAG_FACTORY,
		.name = "D2",
	},
};

static struct dvfm_md_opt pxa320_op_array[] = {
	/* 60MHz -- ring oscillator */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 0,
		.xn = 0,
		.smcfs = 15,
		.sflfs = 60,
		.hss = 60,
		.dmcfs = 30,
		.df_clk = 15,
		.empi_clk = 15,
		.power_mode = POWER_MODE_D0CS,
		.flag = OP_FLAG_FACTORY,
		.lpj = 293888*100/HZ,
		.name = "D0CS",
	},
	/* 104MHz */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 8,
		.xn = 1,
		.smcfs = 78,
		.sflfs = 104,
		.hss = 104,
		.dmcfs = 260,
		/* Actually it's 19.5, not 19 */
		.df_clk = 19,
		.empi_clk = 19,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 517120*100/HZ,
		.name = "104M",
	},
	/* 208MHz */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 16,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 1036288*100/HZ,
		.name = "208M",
	},
	/* 416MHz */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.xl = 16,
		.xn = 2,
		.smcfs = 104,
		.sflfs = 208,
		.hss = 156,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 2076672*100/HZ,
		.name = "416M",
	},
	/* 624MHz */
	{
		.vcc_core = 1375,
		.vcc_sram = 1400,
		.xl = 24,
		.xn = 2,
		.smcfs = 208,
		.sflfs = 312,
		.hss = 208,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 3112960*100/HZ,
		.name = "624M",
	},
	/* 806MHz */
	{
		.vcc_core = 1400,
		.vcc_sram = 1400,
		.xl = 31,
		.xn = 2,
		.smcfs = 208,
		.sflfs = 312,
		.hss = 208,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 4020906*100/HZ,
		.name = "806M",
	},
#if 0
	/* D1 mode */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.power_mode = POWER_MODE_D1,
		.flag = OP_FLAG_FACTORY,
		.name = "D1",
	},
#endif
	/* D2 mode */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.power_mode = POWER_MODE_D2,
		.flag = OP_FLAG_FACTORY,
		.name = "D2",
	},
};

static struct dvfm_md_opt pxa930_op_array[] = {
	/* 60MHz -- ring oscillator */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 0,
		.xn = 0,
		.smcfs = 15,
		.sflfs = 60,
		.hss = 60,
		.dmcfs = 30,
		.df_clk = 15,
		.empi_clk = 15,
		.power_mode = POWER_MODE_D0CS,
		.flag = OP_FLAG_FACTORY,
		.lpj = 293888*100/HZ,
		.name = "D0CS",
	},
	/* 156MHz -- single PLL mode */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 12,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 208,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 778128*100/HZ,
		.name = "156M",
	},
	/* 208MHz */
	{
		.vcc_core = 1000,
		.vcc_sram = 1100,
		.xl = 16,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 1036288*100/HZ,
		.name = "208M",
	},
	/* 416MHz */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.xl = 16,
		.xn = 2,
		.smcfs = 104,
		.sflfs = 208,
		.hss = 156,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 2076672*100/HZ,
		.name = "416M",
	},
	/* 624MHz */
	{
		.vcc_core = 1375,
		.vcc_sram = 1400,
		.xl = 24,
		.xn = 2,
		.smcfs = 208,
		.sflfs = 312,
		.hss = 208,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 3112960*100/HZ,
		.name = "624M",
	},
	/* D2 mode */
	{
		.vcc_core = 1100,
		.vcc_sram = 1200,
		.power_mode = POWER_MODE_D2,
		.flag = OP_FLAG_FACTORY,
		.name = "D2",
	},
};

static struct dvfm_md_opt pxa935_op_array[] = {
	/* 60MHz -- ring oscillator */
	{
		.vcc_core = 1250,
		.xl = 0,
		.xn = 0,
		.smcfs = 15,
		.sflfs = 60,
		.hss = 60,
		.dmcfs = 30,
		.df_clk = 15,
		.empi_clk = 15,
		.power_mode = POWER_MODE_D0CS,
		.flag = OP_FLAG_FACTORY,
		.lpj = 293888*100/HZ,
		.name = "D0CS",
	},
	/* 156MHz -- single PLL mode */
	{
		.vcc_core = 1250,
		.xl = 12,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 208,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 778128*100/HZ,
		.name = "156M",
	},
	/* 208MHz */
	{
		.vcc_core = 1250,
		.xl = 16,
		.xn = 1,
		.smcfs = 104,
		.sflfs = 156,
		.hss = 104,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 1036288*100/HZ,
		.name = "208M",
	},
	/* 416MHz */
	{
		.vcc_core = 1250,
		.xl = 16,
		.xn = 2,
		.smcfs = 104,
		.sflfs = 208,
		.hss = 156,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 2076672*100/HZ,
		.name = "416M",
	},
	/* 624MHz */
	{
		.vcc_core = 1250,
		.xl = 24,
		.xn = 2,
		.smcfs = 208,
		.sflfs = 312,
		.hss = 208,
		.dmcfs = 260,
		.df_clk = 52,
		.empi_clk = 52,
		.power_mode = POWER_MODE_D0,
		.flag = OP_FLAG_FACTORY,
		.lpj = 3112960*100/HZ,
		.name = "624M",
	},
#if 0
	/* D1 mode */
	{
		.vcc_core = 1250,
		.power_mode = POWER_MODE_D1,
		.flag = OP_FLAG_FACTORY,
		.name = "D1",
	},
#endif
	/* D2 mode */
	{
		.vcc_core = 1250,
		.power_mode = POWER_MODE_D2,
		.flag = OP_FLAG_FACTORY,
		.name = "D2",
	},
	/* CG (clock gated) mode */
	{
		.vcc_core = 1250,
		.power_mode = POWER_MODE_CG,
		.flag = OP_FLAG_FACTORY,
		.name = "CG",
	},

};

struct proc_op_array {
	unsigned int cpuid;
	char	*cpu_name;
	struct dvfm_md_opt *op_array;
	unsigned int nr_op;
};

#define ARRAY_AND_SIZE(x)	(x), ARRAY_SIZE(x)
static struct proc_op_array proc_op_arrays[] = {
	{0x6880, "PXA300", ARRAY_AND_SIZE(pxa300_op_array)},
	{0x6890, "PXA310", ARRAY_AND_SIZE(pxa300_op_array)},
	{0x6820, "PXA320", ARRAY_AND_SIZE(pxa320_op_array)},
	{0x6830, "PXA930", ARRAY_AND_SIZE(pxa930_op_array)},
	{0x6930, "PXA935/PXA940", ARRAY_AND_SIZE(pxa935_op_array)},
};

extern void pxa_clkcfg_write(unsigned int);

static int prepare_dmc(void *driver_data, int flag);
static int polling_dmc(void *driver_data);

#ifdef CONFIG_ISPT
static int ispt_dvfm_op(int old, int new)
{
	return ispt_dvfm_msg(old, new);
}

static int ispt_block_dvfm(int enable, int dev_id)
{
	int ret;
	if (enable)
		ret = ispt_driver_msg(CT_P_DVFM_BLOCK_REQ, dev_id);
	else
		ret = ispt_driver_msg(CT_P_DVFM_BLOCK_REL, dev_id);
	return ret;
}

static int ispt_power_state_d2(void)
{
	return ispt_power_msg(CT_P_PWR_STATE_ENTRY_D2);
}
#else
static int ispt_dvfm_op(int old, int new) { return 0; }
static int ispt_block_dvfm(int enable, int dev_id) { return 0; }
static int ispt_power_state_d2(void) { return 0; }
#endif

unsigned int pxa3xx_clk_to_lpj(unsigned int clk)
{
        if (clk == 624000000)
                return LPJ_624M;
        if (clk == 416000000)
                return LPJ_416M;
        if (clk == 208000000)
                return LPJ_208M;
        if (clk == 156000000)
                return LPJ_156M;
        if (clk == 104000000)
                return LPJ_104M;
	if (clk == 60000000) 
		return LPJ_D0CS;
	
	printk(KERN_CRIT "%s does not support clk (%d MHz)\n", 
		__FILE__, clk/1000000);

	return 0; 
}

/* #####################Debug Function######################## */
static int dump_op(void *driver_data, struct op_info *p, char *buf)
{
	int len, count, x;
	struct dvfm_md_opt *q = (struct dvfm_md_opt *)p->op;

	if (q == NULL)
		len = sprintf(buf, "Can't dump the op info\n");
	else {
		/* calculate how much bits is set in device word */
		x = p->device;
		for (count = 0; x; x = x & (x - 1), count++);
		len = sprintf(buf, "OP:%d name:%s [%s, %d]\n",
				p->index, q->name, (count)?"Disabled"
				:"Enabled", count);
		len += sprintf(buf + len, "vcore:%d vsram:%d xl:%d xn:%d "
				"smcfs:%d sflfs:%d hss:%d dmcfs:%d df_clk:%d "
				"power_mode:%d flag:%d\n",
				q->vcc_core, q->vcc_sram, q->xl, q->xn,
				q->smcfs, q->sflfs, q->hss, q->dmcfs,
				q->df_clk, q->power_mode, q->flag);
	}
	return len;
}

static int dump_op_list(void *driver_data, struct info_head *op_table, int flag)
{
	struct op_info *p = NULL;
	struct dvfm_md_opt *q = NULL;
	struct list_head *list = NULL;
	struct pxa3xx_dvfm_info *info = driver_data;
	char buf[256];

	if (!op_table || list_empty(&op_table->list)) {
		printk(KERN_WARNING "op list is null\n");
		return -EINVAL;
	}
	memset(buf, 0, 256);
	list_for_each(list, &op_table->list) {
		p = list_entry(list, struct op_info, list);
		q = (struct dvfm_md_opt *)p->op;
		if (q->flag <= flag) {
			dump_op(info, p, buf);
			pr_debug("%s", buf);
		}
	}
	return 0;
}

/* ########################################################## */
static int freq2reg(struct pxa3xx_fv_info *fv_info, struct dvfm_md_opt *orig)
{
	int res = -EFAULT, tmp;

	if (orig && fv_info) {
		fv_info->vcc_core = orig->vcc_core;
		fv_info->vcc_sram = orig->vcc_sram;
		if (orig->power_mode == POWER_MODE_D0) {
			res = 0;
			fv_info->xl = orig->xl;
			fv_info->xn = orig->xn;
			fv_info->d0cs = 0;
			if (orig->smcfs == 78)
				fv_info->smcfs = 0;
			else if (orig->smcfs == 104)
				fv_info->smcfs = 2;
			else if (orig->smcfs == 208)
				fv_info->smcfs = 5;
			else
				res = -EINVAL;
			if (orig->sflfs == 104)
				fv_info->sflfs = 0;
			else if (orig->sflfs == 156)
				fv_info->sflfs = 1;
			else if (orig->sflfs == 208)
				fv_info->sflfs = 2;
			else if (orig->sflfs == 312)
				fv_info->sflfs = 3;
			else
				res = -EINVAL;
			if (orig->hss == 104)
				fv_info->hss = 0;
			else if (orig->hss == 156)
				fv_info->hss = 1;
			else if (orig->hss == 208)
				fv_info->hss = 2;
			else
				res = -EINVAL;
			if (orig->dmcfs == 26)
				fv_info->dmcfs = 0;
			else if (orig->dmcfs == 208)
				fv_info->dmcfs = 2;
			else if (orig->dmcfs == 260)
				fv_info->dmcfs = 3;
			else
				res = -EINVAL;
			tmp = orig->smcfs / orig->df_clk;
			if (tmp == 2)
				fv_info->df_clk = 2;
			else if (tmp == 4)
				fv_info->df_clk = 3;
			fv_info->empi_clk = fv_info->df_clk;
		} else if (orig->power_mode == POWER_MODE_D0CS) {
			fv_info->d0cs = 1;
			res = 0;
		}
	}
	return res;
}

int md2fvinfo(struct pxa3xx_fv_info *fv_info, struct dvfm_md_opt *orig)
{
	return freq2reg(fv_info, orig);
}

static int reg2freq(void *driver_data, struct dvfm_md_opt *fv_info)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	int res = -EFAULT, tmp;
	uint32_t accr;

	if (fv_info) {
		res = 0;
		if (fv_info->power_mode == POWER_MODE_D0CS) {
			/* set S0D0CS operating pointer */
			fv_info->power_mode = POWER_MODE_D0CS;
			fv_info->xl = 0;
			fv_info->xn = 0;
			fv_info->smcfs = 15;
			fv_info->sflfs = 60;
			fv_info->hss = 60;
			/* PXA310 A2 or PXA935/PXA940 */
			accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
			if (accr & 0x80)
				fv_info->dmcfs = 60;
			else
				fv_info->dmcfs = 30;
			fv_info->df_clk = 15;
			fv_info->empi_clk = 15;
		} else {
			/* set S0D0 operating pointer */
			fv_info->power_mode = POWER_MODE_D0;
			tmp = fv_info->smcfs;
			if (tmp == 0)
				fv_info->smcfs = 78;
			else if (tmp == 2)
				fv_info->smcfs = 104;
			else if (tmp == 5)
				fv_info->smcfs = 208;
			else
				res = -EINVAL;
			tmp = fv_info->sflfs;
			if (tmp == 0)
				fv_info->sflfs = 104;
			else if (tmp == 1)
				fv_info->sflfs = 156;
			else if (tmp == 2)
				fv_info->sflfs = 208;
			else if (tmp == 3)
				fv_info->sflfs = 312;
			tmp = fv_info->hss;
			if (tmp == 0)
				fv_info->hss = 104;
			else if (tmp == 1)
				fv_info->hss = 156;
			else if (tmp == 2)
				fv_info->hss = 208;
			else
				res = -EINVAL;
			tmp = fv_info->dmcfs;
			if (tmp == 0)
				fv_info->dmcfs = 26;
			else if (tmp == 2)
				fv_info->dmcfs = 208;
			else if (tmp == 3)
				fv_info->dmcfs = 260;
			else
				res = -EINVAL;
			tmp = fv_info->df_clk;
			if (tmp == 1)
				fv_info->df_clk = fv_info->smcfs;
			else if (tmp == 2)
				fv_info->df_clk = fv_info->smcfs / 2;
			else if (tmp == 3)
				fv_info->df_clk = fv_info->smcfs / 4;
			fv_info->empi_clk = fv_info->df_clk;
		}
	}
	return res;
}

/* Get current setting, and record it in fv_info structure
 */
static int capture_op_info(void *driver_data, struct dvfm_md_opt *fv_info)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	int res = -EFAULT;
	uint32_t acsr, memclkcfg;

	if (fv_info) {
		memset(fv_info, 0, sizeof(struct dvfm_md_opt));
		acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
		fv_info->xl = (acsr >> ACCR_XL_OFFSET) & 0x1F;
		fv_info->xn = (acsr >> ACCR_XN_OFFSET) & 0x07;
		fv_info->smcfs = (acsr >> ACCR_SMCFS_OFFSET) & 0x07;
		fv_info->sflfs = (acsr >> ACCR_SFLFS_OFFSET) & 0x03;
		fv_info->hss = (acsr >> ACCR_HSS_OFFSET) & 0x03;
		fv_info->dmcfs = (acsr >> ACCR_DMCFS_OFFSET) & 0x03;
		fv_info->power_mode = (acsr >> ACCR_D0CS_OFFSET) & 0x01;
		memclkcfg = __raw_readl(info->smc_base + MEMCLKCFG_OFF);
		fv_info->df_clk = (memclkcfg >> MEMCLKCFG_DF_OFFSET) & 0x07;
		fv_info->empi_clk = (memclkcfg >> MEMCLKCFG_EMPI_OFFSET) & 0x07;
		res = reg2freq(info, fv_info);
		pxa3xx_pmic_get_voltage(VCC_CORE, &fv_info->vcc_core);
		if ((info->cpuid & 0xFFF0) == 0x6930) {
			/* PXA935/PXA940 doesn't have VCC_SRAM */
			fv_info->vcc_sram = 0;
		} else {
			pxa3xx_pmic_get_voltage(VCC_SRAM, &fv_info->vcc_sram);
		}
		/* TODO: mix up the usage of struct dvfm_md_opt and struct pxa3xx_fv_info
		 * better to define reg2freq(struct dvfm_md_opt *md_info,
		 * struct pxa3xx_fv_info *fv_info)
		 */
	}
	return res;
}

/* return all op including user defined op, and boot op */
static int get_op_num(void *driver_data, struct info_head *op_table)
{
	struct list_head *entry = NULL;
	int num = 0;

	if (!op_table)
		goto out;
	read_lock(&op_table->lock);
	if (list_empty(&op_table->list)) {
		read_unlock(&op_table->lock);
		goto out;
	}
	list_for_each(entry, &op_table->list) {
		num++;
	}
	read_unlock(&op_table->lock);
out:
	return num;
}

/* return op name. */
static char *get_op_name(void *driver_data, struct op_info *p)
{
	struct dvfm_md_opt *q = NULL;
	if (p == NULL)
		return NULL;
	q = (struct dvfm_md_opt *)p->op;
	return q->name;
}

static int update_voltage(void *driver_data, struct dvfm_md_opt *old, struct dvfm_md_opt *new)
{
	struct pxa3xx_dvfm_info *info = driver_data;

	if (!(info->flags & PXA3xx_USE_POWER_I2C)) {
		pxa3xx_pmic_set_voltage(VCC_CORE, new->vcc_core);
		pxa3xx_pmic_set_voltage(VCC_SRAM, new->vcc_sram);
	}
	return 0;
}

static void pxa3xx_enter_d0cs(void *driver_data)
{
	struct pxa3xx_dvfm_info *info = driver_data;

	unsigned int reg, spll = 0;
	uint32_t accr, mdrefr;

	reg = (12 << ACCR_XL_OFFSET) | (1 << ACCR_XN_OFFSET);
	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	if (reg == (accr & (ACCR_XN_MASK | ACCR_XL_MASK))) {
		spll = 1;
	}
	/* clk_disable(info->lcd_clk);*/
	enter_d0cs_a((volatile u32 *)info->clkmgr_base, (volatile u32 *)info->dmc_base);
	pxafb_set_pcd();
	/* clk_enable(info->lcd_clk);*/
	/* update to D0CS LPJ, it must be updated before udelay() */
	loops_per_jiffy = d0cs_lpj;
	if (cpu_is_pxa930())
		udelay(200);
	else
		udelay(100);

	/* disable PLL */
	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	if (spll) {
		/* single PLL mode only disable System PLL */
		accr |= (1 << ACCR_SPDIS_OFFSET);
	} else {
		/* Disable both System PLL and Core PLL */
		accr |= (1 << ACCR_XPDIS_OFFSET) | (1 << ACCR_SPDIS_OFFSET);
	}
	__raw_writel(accr, info->clkmgr_base + ACCR_OFF);

	mdrefr = __raw_readl(info->dmc_base + MDREFR_OFF);
	__raw_writel(mdrefr, info->dmc_base + MDREFR_OFF);
}

static void pxa3xx_exit_d0cs(void *driver_data)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	unsigned int spll = 0;
	uint32_t reg, accr, acsr, mdrefr;

	reg = (12 << ACCR_XL_OFFSET) | (1 << ACCR_XN_OFFSET);
	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	if (reg == (accr & (ACCR_XN_MASK | ACCR_XL_MASK))) {
		spll = 1;
	}
	/* enable PLL */
	if (spll) {
		/* single PLL mode only enable System PLL */
		accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
		accr &= ~(1 << ACCR_SPDIS_OFFSET);
		__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
		do {
			acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
		} while (acsr & (1 << ACCR_SPDIS_OFFSET));
	} else {
		/* enable both System PLL and Core PLL */
		accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
		accr &= ~((1 << ACCR_XPDIS_OFFSET) |
				(1 << ACCR_SPDIS_OFFSET));
		__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
		do {
			acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
		} while (acsr & (1 << ACCR_XPDIS_OFFSET)
			|| acsr & (1 << ACCR_SPDIS_OFFSET));
	}

	/* clk_disable(info->lcd_clk);*/
	exit_d0cs_a((volatile u32 *)info->clkmgr_base, (volatile u32 *)info->dmc_base);
	mdrefr = __raw_readl(info->dmc_base + MDREFR_OFF);
	__raw_writel(mdrefr, info->dmc_base + MDREFR_OFF);
	pxafb_set_pcd();
	/* clk_enable(info->lcd_clk);*/
}

/* Return 1 if Grayback PLL is on. */
static int check_grayback_pll(void *driver_data)
{
	struct pxa3xx_dvfm_info *info = driver_data;

	return (__raw_readl(info->clkmgr_base + OSCC_OFF) & (1 << 17));
}

static int set_grayback_pll(void *driver_data, int lev)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	int timeout = 100, turnoff;
	uint32_t oscc, agenp;

	if ((info->cpuid & 0xFFF0) != 0x6830 && (info->cpuid & 0xFFF0) != 0x6930) {
		/* It's not PXA930/PXA935/PXA940*/
		return 0;
	}
	if (lev) {
		/* turn on grayback PLL */
		for (;;){
			timeout = 100;
			/* clear OSCC[GPRM] */
			oscc = __raw_readl(info->clkmgr_base + OSCC_OFF);
			oscc &= ~(1 << 18);
			__raw_writel(oscc, info->clkmgr_base + OSCC_OFF);

			/* set AGENP[GBPLL_CTRL] and AGENP[GBPLL_ST] */
			agenp = __raw_readl(info->bpmu_base + AGENP_OFF);
			agenp |= (3 << 28);
			__raw_writel(agenp, info->bpmu_base + AGENP_OFF);

			/* check OSCC[GPRL] */
			do {
				oscc = __raw_readl(info->clkmgr_base + OSCC_OFF);
				if (--timeout == 0)
					break;
			} while (!(oscc & (1 << 17)));

			if (timeout)
				break;
		}
	} else {
		/* turn off Grayback PLL */
		for (;;){
			timeout = 100;
			/* clear AGENP[GBPLL_CTRL] and AGENP[GBPLL_ST] */
			agenp = __raw_readl(info->bpmu_base + AGENP_OFF);
			if (agenp & (1 << 28)) {
				turnoff = 1;
				agenp &= ~(3 << 28);
				agenp |= (2 << 28);
				__raw_writel(agenp, info->bpmu_base + AGENP_OFF);

				/* check OSCC[GPRL] */
				do {
					oscc = __raw_readl(info->clkmgr_base + OSCC_OFF);
					if (--timeout == 0)
						break;
				} while ((oscc & (1 << 17)));
			}

			if (timeout)
				break;
		}
		if (turnoff) {
			/* set OSCC[GPRM] */
			oscc = __raw_readl(info->clkmgr_base + OSCC_OFF);
			oscc |= (1 << 18);
			__raw_writel(oscc, info->clkmgr_base + OSCC_OFF);
		}
	}
	return 0;
}

/*
 * Return 2 if MTS should be changed to 2.
 * Return 1 if MTS should be changed to 1.
 * Return 0 if MTS won't be changed.
 * In this function, the maxium MTS is 2.
 */
static int check_mts(struct dvfm_md_opt *old, struct dvfm_md_opt *new)
{
	int ret = 0;
	if ((old->xn == 1) && (new->xn == 2))
		ret = 2;
	if ((old->xn == 2) && (new->xn == 1))
		ret = 1;
	return ret;
}

static int set_mts(void *driver_data, int mts)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	unsigned int ascr;

	ascr = __raw_readl(info->bpmu_base + ASCR_OFF);
	ascr &= ~(3 << ASCR_MTS_OFFSET);
	ascr |= (mts << ASCR_MTS_OFFSET);
	__raw_writel(ascr, info->bpmu_base + ASCR_OFF);

	/* wait MTS is set */
	do {
		ascr = __raw_readl(info->bpmu_base + ASCR_OFF);
	}while (((ascr >> ASCR_MTS_OFFSET) & 0x3)
		!= ((ascr >> ASCR_MTS_S_OFFSET) & 0x3));

	return 0;
}

static int prepare_dmc(void *driver_data, int flag)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	int pll;
	uint32_t mdcnfg, ddr_hcal;

	if (flag == DMEMC_D0CS_ENTER) {
		mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);
		mdcnfg |= (1 << MDCNFG_HWFREQ_OFFSET);
		__raw_writel(mdcnfg, info->dmc_base + MDCNFG_OFF);

		ddr_hcal = __raw_readl(info->dmc_base + DDR_HCAL_OFF);
		ddr_hcal &= ~(1 << HCAL_HCEN_OFFSET);
		__raw_writel(ddr_hcal, info->dmc_base + DDR_HCAL_OFF);

		return 0;
	} else if (flag == DMEMC_D0CS_EXIT) {
		mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);
		mdcnfg |= (1 << MDCNFG_HWFREQ_OFFSET);
		__raw_writel(mdcnfg, info->dmc_base + MDCNFG_OFF);

		ddr_hcal = __raw_readl(info->dmc_base + DDR_HCAL_OFF);
		ddr_hcal |= (1 << HCAL_HCEN_OFFSET);
		__raw_writel(ddr_hcal, info->dmc_base + DDR_HCAL_OFF);

		return 0;
	} else if (flag == DMEMC_FREQ_LOW) {
		pll = 3;
	} else {
		pll = 2;
	}

	mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);
	mdcnfg &= ~(3 << 28);
	mdcnfg |= (pll << 28);
	__raw_writel(mdcnfg, info->dmc_base + MDCNFG_OFF);
	mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);

	ddr_hcal = __raw_readl(info->dmc_base + DDR_HCAL_OFF);
	ddr_hcal |= (1 << HCAL_HCEN_OFFSET);
	__raw_writel(ddr_hcal, info->dmc_base + DDR_HCAL_OFF);
	ddr_hcal = __raw_readl(info->dmc_base + DDR_HCAL_OFF);

	do {
		/*pr_debug("polling MDCNFG:0x%x\n", MDCNFG);*/
		mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);
	} while (((mdcnfg >> 28) & 0x3) != pll);

	return 0;
}

static int set_dmc60(void *driver_data, int flag)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	uint32_t accr, reg;

	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	if (flag)
		accr |= 0x80;
	else
		accr &= ~0x80;
	__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
	/* polling ACCR */
	do {
		reg = __raw_readl(info->clkmgr_base + ACCR_OFF);
	} while ((accr & 0x80) != (reg & 0x80));

	return 0;
}

/* set DF and EMPI divider */
/* TODO: why did not we see DF/EMPI clock as input here? If we want to set DFI_clock or
 * EMPI clock as other frequecy than 52, how can we do?
 */
static int set_df(void *driver_data, int smc)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	uint32_t memclkcfg;
	int fix_empi;

	if (((info->cpuid > 0x6880) && (info->cpuid <= 0x6881))
		|| ((info->cpuid >= 0x6890) && (info->cpuid <= 0x6892)))
		/* It's PXA300 or PXA310 */
		fix_empi = 1;
	else
		fix_empi = 0;

	memclkcfg = __raw_readl(info->smc_base + MEMCLKCFG_OFF);
	memclkcfg &= ~((7 << MEMCLKCFG_DF_OFFSET) | (7 << MEMCLKCFG_EMPI_OFFSET));
	if (fix_empi) {
		memclkcfg |= (3 << MEMCLKCFG_EMPI_OFFSET);
		switch (smc) {
		case 208:
			/* divider -- 4 */
			memclkcfg |= (3 << MEMCLKCFG_DF_OFFSET);
			break;
		case 104:
			/* divider -- 2 */
			memclkcfg |= (2 << MEMCLKCFG_DF_OFFSET);
			break;
		case 78:
			/* divider -- 4 */
			memclkcfg |= (3 << MEMCLKCFG_DF_OFFSET);
			break;
		}
	} else {
		switch (smc) {
		case 208:
			/* divider -- 4 */
			memclkcfg |= (3 << MEMCLKCFG_DF_OFFSET);
			memclkcfg |= (3 << MEMCLKCFG_EMPI_OFFSET);
			break;
		case 104:
			/* divider -- 2 */
			memclkcfg |= (2 << MEMCLKCFG_DF_OFFSET);
			memclkcfg |= (2 << MEMCLKCFG_EMPI_OFFSET);
			break;
		case 78:
			/* divider -- 4 */
			memclkcfg |= (3 << MEMCLKCFG_DF_OFFSET);
			memclkcfg |= (3 << MEMCLKCFG_EMPI_OFFSET);
			break;
		}
	}
	__raw_writel(memclkcfg, info->smc_base + MEMCLKCFG_OFF);
	memclkcfg = __raw_readl(info->smc_base + MEMCLKCFG_OFF);

	return 0;
}

/* TODO: sugguest to differentiate the operating point definition from
 * register info.And we can remove *reg_new here, and convert dvfm_md_opt to
 * it in the routine. That will make it much more clear.
 */
static int update_hss(void *driver_data, struct dvfm_md_opt *old, struct dvfm_md_opt *new,
			struct pxa3xx_fv_info *fv_info)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	unsigned int accr, acsr;

	if (old->hss != new->hss) {
		accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
		accr &= ~ACCR_HSS_MASK;
		accr |= (fv_info->hss << ACCR_HSS_OFFSET);
		__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
		/* wait until ACSR is changed */
		do {
			accr = __raw_readl(info->clkmgr_base + ACCR_OFF) ;
			acsr = __raw_readl(info->clkmgr_base + ACSR_OFF) ;
		}while ((accr & ACCR_HSS_MASK) != (acsr & ACCR_HSS_MASK));
		/* clk_disable(info->lcd_clk);*/
		/* set PCD just after HSS updated */
		pxafb_set_pcd();
		/* clk_enable(info->lcd_clk);*/
	}

	return 0;
}

static int update_bus_freq(void *driver_data, struct dvfm_md_opt *old, struct dvfm_md_opt *new)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	struct pxa3xx_fv_info fv_info;
	uint32_t accr, acsr, mdcnfg, mask;
	int timeout, dmcflag = 1;

	freq2reg(&fv_info, new);
	if (old->dmcfs < new->dmcfs)
		prepare_dmc(info, DMEMC_FREQ_HIGH);
	else if (old->dmcfs > new->dmcfs)
		prepare_dmc(info, DMEMC_FREQ_LOW);
	else
		dmcflag = 0;
	if (new->smcfs == 208 || new->smcfs == 78)
		set_df(info, new->smcfs);

	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	mask = 0;
	if (old->smcfs != new->smcfs) {
		accr &= ~ACCR_SMCFS_MASK;
		accr |= (fv_info.smcfs << ACCR_SMCFS_OFFSET);
		mask |= ACCR_SMCFS_MASK;
	}
	if (old->sflfs != new->sflfs) {
		accr &= ~ACCR_SFLFS_MASK;
		accr |= (fv_info.sflfs << ACCR_SFLFS_OFFSET);
		mask |= ACCR_SFLFS_MASK;
	}
	if (old->dmcfs != new->dmcfs) {
		accr &= ~ACCR_DMCFS_MASK;
		accr |= (fv_info.dmcfs << ACCR_DMCFS_OFFSET);
		mask |= ACCR_DMCFS_MASK;
	}
	__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);

	/* wait until ACSR is changed */
	do {
		accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
		acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
	} while ((accr & mask) != (acsr & mask));

	if (dmcflag) {
		timeout = 10;
		do {
			mdcnfg = __raw_readl(info->dmc_base + MDCNFG_OFF);
			udelay(1);
			if (--timeout == 0) {
				printk(KERN_WARNING "MDCNFG[29:28] isn't zero\n");
				break;
			}
		} while (mdcnfg & ( 3 << 28));
	}

	if (new->smcfs == 104) {
		set_df(info, new->smcfs);
	}

	update_hss(info, old, new, &fv_info);

	return 0;
}

static int set_freq(void *driver_data, struct dvfm_md_opt *old, struct dvfm_md_opt *new)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	int spll;
	uint32_t accr, acsr;

	/* check whether new OP is single PLL mode */
	if ((new->xl == 0x0c) && (new->xn == 0x1))
		spll = 1;
	else
		spll = 0;

	/* turn on Grayback PLL */
	if (!spll & !check_grayback_pll(info))
		set_grayback_pll(info ,1);
	if (check_mts(old, new) == 2)
		set_mts(info, 2);

	accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
	accr &= ~(ACCR_XL_MASK | ACCR_XN_MASK | ACCR_XSPCLK_MASK);
	accr |= ((new->xl << ACCR_XL_OFFSET) | (new->xn << ACCR_XN_OFFSET)
			| (3 << ACCR_XSPCLK_OFFSET));
	__raw_writel(accr, info->clkmgr_base + ACCR_OFF);
	/* delay 2 cycles of 13MHz clock */
	udelay(1);

	if (check_mts(old, new) == 1)
		set_mts(info, 1);

	if ((new->xl == old->xl) && (new->xn != old->xn))
		/* set T bit */
		pxa_clkcfg_write(1);
	else
		/* set F bit */
		pxa_clkcfg_write(2);
	do {
		accr = __raw_readl(info->clkmgr_base + ACCR_OFF);
		acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
	} while ((accr & (ACCR_XL_MASK | ACCR_XN_MASK))
			!= (acsr & (ACCR_XL_MASK | ACCR_XN_MASK)));

	udelay(1);
	update_bus_freq(info, old, new);

	/* turn off Grayback PLL */
	if (spll)
		set_grayback_pll(info, 0);
	return 0;
}

static int update_freq(void *driver_data, struct dvfm_freqs *freqs)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	static struct dvfm_md_opt before_d0cs;
	struct dvfm_md_opt old, new;
	struct op_info *p = NULL;
	unsigned long flags;
	int found = 0, new_op = cur_op;

	memset(&old, 0, sizeof(struct dvfm_md_opt));
	memset(&new, 0, sizeof(struct dvfm_md_opt));
	write_lock_irqsave(&pxa3xx_dvfm_op_list.lock, flags);
	if (!list_empty(&pxa3xx_dvfm_op_list.list)) {
		list_for_each_entry(p, &pxa3xx_dvfm_op_list.list, list) {
			if (p->index == freqs->old) {
				found++;
				memcpy(&old, (struct dvfm_md_opt *)p->op,
					sizeof(struct dvfm_md_opt));
			}
			if (p->index == freqs->new) {
				found++;
				memcpy(&new, (struct dvfm_md_opt *)p->op,
					sizeof(struct dvfm_md_opt));
				new_op = p->index;
			}
			if (found == 2)
				break;
		}
	}
	write_unlock_irqrestore(&pxa3xx_dvfm_op_list.lock, flags);
	if (found != 2)
		return -EINVAL;

	if ((old.power_mode == POWER_MODE_D0)
		&& (new.power_mode == POWER_MODE_D0CS)) {
		memcpy(&before_d0cs, &old, sizeof(struct dvfm_md_opt));

		pxa3xx_enter_d0cs(info);
		update_voltage(info, &old, &new);
		cur_op = new_op;
		loops_per_jiffy = new.lpj;
		return 0;
	} else if ((old.power_mode == POWER_MODE_D0CS)
		&& (new.power_mode == POWER_MODE_D0)) {
		if (memcmp(&before_d0cs, &new, sizeof(struct dvfm_md_opt))) {
			/* exit d0cs and set new operating point */
			if ((before_d0cs.vcc_core < new.vcc_core) ||
				(before_d0cs.vcc_sram < new.vcc_sram)) {
				update_voltage(info, &old, &new);
			} else {
				update_voltage(info, &old, &before_d0cs);
			}
			pxa3xx_exit_d0cs(info);
			set_freq(info, &before_d0cs, &new);

			if ((before_d0cs.vcc_core > new.vcc_core) ||
				(before_d0cs.vcc_sram > new.vcc_sram))
				update_voltage(info, &before_d0cs, &new);
		} else {
			update_voltage(info, &old, &new);
			/* exit d0cs */
			pxa3xx_exit_d0cs(info);
		}
		cur_op = new_op;
		loops_per_jiffy = new.lpj;
		return 0;
	} else if ((old.power_mode == POWER_MODE_D0CS)
		&& (new.power_mode == POWER_MODE_D0CS)) {
		cur_op = new_op;
		return 0;
	}

	if (old.core < new.core) {
		update_voltage(info, &old, &new);
	}
	set_freq(info, &old, &new);
	if (old.core > new.core) {
		update_voltage(info, &old, &new);
	}
	cur_op = new_op;
	if ((new.power_mode == POWER_MODE_D0)
		|| (new.power_mode == POWER_MODE_D0CS))
		loops_per_jiffy = new.lpj;
	return 0;
}

/* function of entering low power mode */
extern void enter_lowpower_mode(int state);

static void do_freq_notify(void *driver_data, struct dvfm_freqs *freqs)
{
	struct pxa3xx_dvfm_info *info = driver_data;

	dvfm_notifier_frequency(freqs, DVFM_FREQ_PRECHANGE);
	update_freq(info, freqs);
	dvfm_notifier_frequency(freqs, DVFM_FREQ_POSTCHANGE);
	ispt_dvfm_op(freqs->old, freqs->new);

	printk("-- dvfm: cur_op=%d\n",cur_op);
}

static void do_lowpower_notify(void *driver_data, struct dvfm_freqs *freqs, unsigned int state)
{
	dvfm_notifier_frequency(freqs, DVFM_FREQ_PRECHANGE);
	//enter_lowpower_mode(state);
	dvfm_notifier_frequency(freqs, DVFM_FREQ_POSTCHANGE);
	ispt_power_state_d2();
}

static int check_op(void *driver_data, struct dvfm_freqs *freqs, unsigned int new,
			unsigned int relation)
{
	struct op_info *p = NULL;
	struct dvfm_md_opt *q = NULL;
	int core, tmp_core = -1, found = 0;
	int first_op = 0;

	freqs->new = -1;
	if (!dvfm_find_op(new, &p)) {
		q = (struct dvfm_md_opt *)p->op;
		core = q->core;
	} else
		return -EINVAL;
	/*
	pr_debug("%s, old:%d, new:%d, core:%d\n", __FUNCTION__, freqs->old,
		new, core);
	*/
	read_lock(&pxa3xx_dvfm_op_list.lock);
	if (relation == RELATION_LOW) {
		/* Set the lowest frequency that is higher than specifed one */
		list_for_each_entry(p, &pxa3xx_dvfm_op_list.list, list) {
			q = (struct dvfm_md_opt *)p->op;
			if (core == 0) {
				/* Lowpower mode */
				if ((q->power_mode == POWER_MODE_D1)
					|| (q->power_mode == POWER_MODE_D2)
					|| (q->power_mode == POWER_MODE_CG)) {
					if (!p->device && (new == p->index)) {
						freqs->new = p->index;
						/*
						pr_debug("%s, found op%d\n",
							__FUNCTION__, p->index);
						*/
						break;
					}
				}
				continue;
			}

			if (!p->device && (q->core >= core)) {
				if (tmp_core == -1 || (tmp_core >= q->core)) {
					/*
					pr_debug("%s, found op%d, core:%d\n",
						__FUNCTION__, p->index,
						q->core);
					*/
					if (first_op == 0)
						first_op = p->index;
					freqs->new = p->index;
					tmp_core = q->core;
					found = 1;
				}
				if (found && (new == p->index))
					break;
			}
		}
		if (found && (first_op == 1) && (new != p->index))
			freqs->new = first_op;
	} else if (relation == RELATION_HIGH) {
		/* Set the highest frequency that is lower than specified one */
		list_for_each_entry(p, &pxa3xx_dvfm_op_list.list, list) {
			q = (struct dvfm_md_opt *)p->op;
			if (!p->device && (q->core <= core)) {
				if (tmp_core == -1 || tmp_core < q->core) {
					freqs->new = p->index;
					tmp_core = q->core;
				}
			}
		}
	} else if (relation == RELATION_STICK) {
		/* Set the specified frequency */
		list_for_each_entry(p, &pxa3xx_dvfm_op_list.list, list) {
			if (!p->device && (p->index == new)) {
				freqs->new = p->index;
				break;
			}
		}
	}
	read_unlock(&pxa3xx_dvfm_op_list.lock);
	if (freqs->new == -1) {
		/*
		pr_debug("%s, Can't find op\n", __FUNCTION__);
		pr_debug("%s, old:%d, new:%d, core:%d\n", __FUNCTION__,
			freqs->old, new, core);
		*/
		return -EINVAL;
	}
	return 0;
}

static int pxa3xx_get_freq(void *driver_data, struct op_info *p, struct op_freq *freq)
{
        struct dvfm_md_opt *q = (struct dvfm_md_opt *)p->op;
	freq->cpu_freq = q->core;
	return 0;
}

static int pxa3xx_check_active_op(void *driver_data, struct op_info *p)
{
	struct dvfm_md_opt *q = (struct dvfm_md_opt *)p->op;

	if ((!strcmp(q->name, "D0CS")) && (boot_core_freq >= q->core))
		return 0;

        if ((!strcmp(q->name, "104M")) && (boot_core_freq >= q->core))
                return 0;

	if ((!strcmp(q->name, "156M")) && (boot_core_freq >= q->core))
		return 0;

	if ((!strcmp(q->name, "208M")) && (boot_core_freq >= q->core))
		return 0;

	if ((!strcmp(q->name, "416M")) && (boot_core_freq >= q->core))
		return 0;

	if ((!strcmp(q->name, "624M")) && (boot_core_freq >= q->core))
		return 0;

	return -EINVAL;
}


static int pxa3xx_set_op(void *driver_data, struct dvfm_freqs *freqs, unsigned int new,
			unsigned int relation)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	struct dvfm_md_opt *md = NULL, *old_md = NULL;
	struct op_info *p = NULL;
	unsigned long flags;
	int ret;
	out_d0cs = 0;

	local_fiq_disable();
	local_irq_save(flags);
	ret = dvfm_find_op(freqs->old, &p);
	if (ret) {
		printk("---- pxa3xx_set_op1 check_op failed to %d\n",new);
		goto out;
	}

	memcpy(&freqs->old_info, p, sizeof(struct op_info));
	ret = check_op(info, freqs, new, relation);
	if (ret) {
		printk("---- pxa3xx_set_op2 check_op failed to %d\n",new);
		goto out;
	}

	if (!dvfm_find_op(freqs->new, &p)) {
		memcpy(&(freqs->new_info), p, sizeof(struct op_info));
		/* If find old op and new op is same, skip it.
		 * At here, ret should be zero.
		 */
		if (freqs->old_info.index == freqs->new_info.index)
			goto out;
#ifdef DVFM_LP_SAFE
		md = (struct dvfm_md_opt *)(freqs->new_info.op);
		old_md = (struct dvfm_md_opt *)(freqs->old_info.op);
		if ((old_md->power_mode == POWER_MODE_D0CS)
			&& ((md->power_mode == POWER_MODE_D1)
			|| (md->power_mode == POWER_MODE_D2))) {
			dvfm_disable_op_name("D0CS", dvfm_dev_id);
			out_d0cs = 1;
		}

		md = (struct dvfm_md_opt *)p->op;
		switch (md->power_mode) {
		case POWER_MODE_D0:
		case POWER_MODE_D0CS:
			do_freq_notify(info, freqs);
			break;
		case POWER_MODE_D1:
		case POWER_MODE_D2:
		case POWER_MODE_CG:
			do_lowpower_notify(info, freqs, md->power_mode);
			break;
		}
		local_irq_restore(flags);
		local_fiq_enable();

		if (out_d0cs) {
			dvfm_enable_op_name("D0CS", dvfm_dev_id);
		}
#else
		md = (struct dvfm_md_opt *)p->op;
		switch (md->power_mode) {
		case POWER_MODE_D0:
		case POWER_MODE_D0CS:
			do_freq_notify(info, freqs);
			break;
		case POWER_MODE_D1:
		case POWER_MODE_D2:
		case POWER_MODE_CG:
			do_lowpower_notify(info, freqs, md->power_mode);
			break;
		}
		local_irq_restore(flags);
		local_fiq_enable();
#endif
	}
	return 0;
out:
	local_irq_restore(flags);
	local_fiq_enable();
	return ret;
}

static int pxa3xx_request_op(void *driver_data, int index)
{
	struct dvfm_freqs freqs;
	struct op_info *info = NULL;
	struct dvfm_md_opt *md = NULL;
	int relation, ret;
	ret = dvfm_find_op(index, &info);
	if (ret)
		goto out;
	freqs.old = cur_op;
	freqs.new = index;
	md = (struct dvfm_md_opt *)(info->op);
	switch (md->power_mode) {
	case POWER_MODE_D1:
	case POWER_MODE_D2:
	case POWER_MODE_CG:
		relation = RELATION_STICK;
		ret = pxa3xx_set_op(driver_data, &freqs, index, relation);
		break;
	default:
		relation = RELATION_LOW;
		/* only use non-low power mode as preferred op */
		ret = pxa3xx_set_op(driver_data, &freqs, index, relation);
		if (!ret)
			preferred_op = index;
		break;
	}
out:
	return ret;
}

static int is_d0cs(void *driver_data)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	unsigned int acsr;
	/* read ACSR */
	acsr = __raw_readl(info->clkmgr_base + ACSR_OFF);
	/* Check ring oscillator status */
	if (acsr & (1 << 26))
		return 1;
	return 0;
}

/* Produce a operating point table */
static int op_init(void *driver_data, struct info_head *op_table)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	unsigned long flags;
	int i, index;
	struct op_info *p = NULL, *q = NULL;
	struct dvfm_md_opt *md = NULL, *smd = NULL;
	struct proc_op_array *proc = NULL;

	write_lock_irqsave(&op_table->lock, flags);
	for (i = 0; i < ARRAY_SIZE(proc_op_arrays); i++){
		if (proc_op_arrays[i].cpuid == (info->cpuid & 0xfff0)) {
			proc = &proc_op_arrays[i];
			break;
		}
	}
	if (proc == NULL) {
		printk(KERN_ERR "Failed to find op tables for cpu_id 0x%08x", info->cpuid);
		write_unlock_irqrestore(&op_table->lock, flags);
		return -EIO;
	} else {
		printk("initializing op table for %s\n", proc->cpu_name);
	}
	for (i = 0, index = 0; i < proc->nr_op; i++) {
		/* PXA310 A2 or PXA935/PXA940, dmcfs 60MHz in S0D0CS mode */
		if ((proc->op_array[i].power_mode == POWER_MODE_D0CS)
			&& (info->cpuid == 0x6892 || (info->cpuid & 0xFFF0) == 0x6930)) {
			set_dmc60(info, 1);
			proc->op_array[i].dmcfs = 60;
		}

		/* Set index of operating point used in idle */
		if (proc->op_array[i].power_mode != POWER_MODE_D0) {
			//set_idle_op(index, proc->op_array[i].power_mode);
		}

		md = (struct dvfm_md_opt *)kzalloc(sizeof(struct dvfm_md_opt),
					GFP_KERNEL);
		p = (struct op_info *)kzalloc(sizeof(struct op_info),
					GFP_KERNEL);
		p->op = (void *)md;
		memcpy(p->op, &proc->op_array[i], sizeof(struct dvfm_md_opt));
		md->core = 13 * md->xl * md->xn;
		if (md->power_mode == POWER_MODE_D0CS)
			md->core = 60;
		p->index = index++;
		list_add_tail(&(p->list), &(op_table->list));
	}
	md = (struct dvfm_md_opt *)kzalloc(sizeof(struct dvfm_md_opt),
					GFP_KERNEL);
	p = (struct op_info *)kzalloc(sizeof(struct op_info), GFP_KERNEL);
	p->op = (void *)md;
	if (capture_op_info(info, md)) {
		printk(KERN_WARNING "Failed to get current op setting\n");
	} else {
		def_op = 0x5a5a;	/* magic number */
		list_for_each_entry(q, &(op_table->list), list) {
			smd = (struct dvfm_md_opt *)q->op;
			md->flag = smd->flag;
			md->lpj = smd->lpj;
			md->core = smd->core;
			if (memcmp(md, smd, sizeof(struct dvfm_md_opt)) == 0) {
				def_op = q->index;
				break;
			}
		}
	}
	if (is_d0cs(driver_data))
		md->core = 60;
	else
		md->core = 13 * md->xl * md->xn;
	md->lpj = loops_per_jiffy;
	md->flag = OP_FLAG_BOOT;
	sprintf(md->name, "BOOT OP");

	boot_core_freq = md->core;

#if 0   /* disable CUSTOM OP for borq platfrom */
	smd = (struct dvfm_md_opt *)kzalloc(sizeof(struct dvfm_md_opt),
					GFP_KERNEL);
	q = (struct op_info *)kzalloc(sizeof(struct op_info), GFP_KERNEL);
	memcpy(q, p, sizeof(struct op_info));
	memcpy(smd, md, sizeof(struct dvfm_md_opt));
	smd->core = md->core;
	smd->lpj = md->lpj;
	smd->flag = OP_FLAG_USER_DEFINED;
	sprintf(smd->name, "CUSTOM OP");
	q->op = (void *)smd;
	/* Add CUSTOM OP into op list */
	q->index = index++;
	list_add_tail(&q->list, &op_table->list);
#endif
	/* Add BOOT OP into op list */
	p->index = index++;
	preferred_op = p->index;
	list_add_tail(&p->list, &op_table->list);
	/* BOOT op */
	if (def_op == 0x5a5a) {
		cur_op = p->index;
		def_op = p->index;
	} else
		cur_op = def_op;
	pr_debug("%s, def_op:%d, cur_op:%d\n", __FUNCTION__, def_op, cur_op);

	op_nums = proc->nr_op + 2;	/* set the operating point number */

	pr_debug("Current Operating Point is %d\n", cur_op);
	dump_op_list(info, op_table, OP_FLAG_ALL);
	write_unlock_irqrestore(&op_table->lock, flags);

	return 0;
}

/*
 * The machine operation of dvfm_enable
 */
static int pxa3xx_enable_dvfm(void *driver_data, int dev_id)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	struct dvfm_md_opt *md = NULL;
	struct op_info *p = NULL;
	int i, num;
	num = get_op_num(info, &pxa3xx_dvfm_op_list);
	for (i = 0; i < num; i++) {
		if (!dvfm_find_op(i, &p)) {
			md = (struct dvfm_md_opt *)p->op;
			if (md->core < boot_core_freq)
				dvfm_enable_op_name(md->name, dev_id);
		}
	}
	ispt_block_dvfm(0, dev_id);
	return 0;
}

/*
 * The mach operation of dvfm_disable
 */
static int pxa3xx_disable_dvfm(void *driver_data, int dev_id)
{
	struct pxa3xx_dvfm_info *info = driver_data;
	struct dvfm_md_opt *md = NULL;
	struct op_info *p = NULL;
	int i, num;
	num = get_op_num(info, &pxa3xx_dvfm_op_list);
	for (i = 0; i < num; i++) {
		if (!dvfm_find_op(i, &p)) {
			md = (struct dvfm_md_opt *)p->op;
			if (md->core < boot_core_freq)
				dvfm_disable_op_name(md->name, dev_id);
		}
	}
	ispt_block_dvfm(1, dev_id);
	return 0;
}

static int pxa3xx_enable_op(void *driver_data, int index, int relation)
{
	/*
	 * Restore preferred_op. Because this op is sugguested by policy maker
	 * or user.
	 */
	return pxa3xx_request_op(driver_data, preferred_op);
}

static int pxa3xx_disable_op(void *driver_data, int index, int relation)
{
	struct dvfm_freqs freqs;
	if (cur_op == index) {
		freqs.old = index;
		freqs.new = -1;
		dvfm_set_op(&freqs, freqs.old, relation);
	}
	return 0;
}

static int pxa3xx_volt_show(void *driver_data, char *buf)
{
	struct dvfm_md_opt new;
	int len = 0;

	memset(&new, 0, sizeof(struct dvfm_md_opt));
	pxa3xx_pmic_get_voltage(VCC_CORE, &new.vcc_core);
	pxa3xx_pmic_get_voltage(VCC_SRAM, &new.vcc_sram);
	len = sprintf(buf, "core voltage:%dmv, sram voltage:%dmv\n",
			new.vcc_core, new.vcc_sram);
	return len;
}

#ifdef CONFIG_CPU_PXA310
static int pxa3xx_freq_show(void *driver_data, struct op_info *p, char *buf)
{
	struct dvfm_md_opt *q = (struct dvfm_md_opt *)p->op;
	struct pxa3xx_fv_info info;

	if (q == NULL)
		return sprintf(buf, "unable to get frequency info\n");
	else {
		freq2reg(&info, q);
		if (!info.d0cs){
			return sprintf(buf, "current frequency is %luMhz"
					" (XL: %lu, XN: %lu, %s) with\n"
					"  SMEM: %lu (%dMhz)\n"
					"  SRAM: %lu (%dMhz)\n"
					"  HSS: %lu (%dMhz)\n"
					"  DDR: %lu (%dMhz)\n"
					"  DFCLK: %lu (%dMhz)\n"
					"  EMPICLK: %lu (%dMhz)\n"
					"  D0CKEN_A: 0x%08x\n"
					"  D0CKEN_B: 0x%08x\n"
					"  ACCR: 0x%08x\n"
					"  ACSR: 0x%08x\n"
					"  OSCC: 0x%08x\n",
					FREQ_CORE(info.xl, info.xn), info.xl, info.xn,
					(info.xn != 0x1)? "Turbo Mode" : "Run Mode",
					info.smcfs, FREQ_STMM(info.smcfs),
					info.sflfs, FREQ_SRAM(info.sflfs),
					info.hss, FREQ_HSS(info.hss),
					info.dmcfs, FREQ_DDR(info.dmcfs),
					info.df_clk, FREQ_DFCLK(info.smcfs, info.df_clk),
					info.empi_clk, FREQ_EMPICLK(info.smcfs, info.empi_clk),
					CKENA, CKENB, ACCR, ACSR, OSCC);
		} else {
			return sprintf(buf, "current frequency is 60Mhz"
					" (ring oscillator mode) with\n"
					"  SMEM:15Mhz\n"
					"  SRAM:60Mhz\n"
					"  HSS:60Mhz\n"
					"  DDR:30Mhz\n"
					"  DFCLK:%sMhz\n"
					"  EMPICLK:%sMhz\n"
					"  D0CKEN_A: 0x%08x\n"
					"  D0CKEN_B: 0x%08x\n"
					"  ACCR: 0x%08x\n"
					"  ACSR: 0x%08x\n"
					"  OSCC: 0x%08x\n",
					(info.df_clk == 1)?"15":
					(info.df_clk == 2)?"7.5":
					(info.df_clk == 3)?"3.75":"0",
					(info.empi_clk == 1)?"15":
					(info.empi_clk == 2)?"7.5":
					(info.empi_clk == 3)?"3.75":"0",
					CKENA, CKENB, ACCR, ACSR, OSCC);
		}


	}
}
#endif

#ifdef CONFIG_PXA3xx_DVFM_STATS
/* Convert ticks from 32K timer to microseconds */
static unsigned int pxa3xx_ticks_to_usec(unsigned int ticks)
{
	return (ticks * 5 * 5 * 5 * 5 * 5 * 5) >> 9;
}

static unsigned int pxa3xx_ticks_to_sec(unsigned int ticks)
{
	return (ticks >> 15);
}

static unsigned int pxa3xx_read_time(void)
{
	return OSCR4;
}

/* It's invoked by PM functions.
 * PM functions can store the accurate time of entering/exiting low power
 * mode.
 */
int calc_switchtime(unsigned int end, unsigned int start)
{
	switch_lowpower_before = end;
	switch_lowpower_after = start;
	return 0;
}

static int pxa3xx_stats_notifier_freq(struct notifier_block *nb,
				unsigned long val, void *data)
{
	struct dvfm_freqs *freqs = (struct dvfm_freqs *)data;
	struct op_info *info = &(freqs->new_info);
	struct dvfm_md_opt *md = NULL;
	unsigned int ticks;

	ticks = pxa3xx_read_time();
	md = (struct dvfm_md_opt *)(info->op);
	if (md->power_mode == POWER_MODE_D0 ||
		md->power_mode == POWER_MODE_D0CS) {
		switch (val) {
		case DVFM_FREQ_PRECHANGE:
			calc_switchtime_start(freqs->old, freqs->new, ticks);
			break;
		case DVFM_FREQ_POSTCHANGE:
			/* Calculate the costed time on switching frequency */
			calc_switchtime_end(freqs->old, freqs->new, ticks);
			dvfm_add_event(freqs->old, CPU_STATE_RUN,
					freqs->new, CPU_STATE_RUN);
			dvfm_add_timeslot(freqs->old, CPU_STATE_RUN);
			mspm_add_event(freqs->old, CPU_STATE_RUN);
			break;
		}
	} else if (md->power_mode == POWER_MODE_D1 ||
		md->power_mode == POWER_MODE_D2 ||
		md->power_mode == POWER_MODE_CG) {
		switch (val) {
		case DVFM_FREQ_PRECHANGE:
			calc_switchtime_start(freqs->old, freqs->new, ticks);
			/* Consider lowpower mode as idle mode */
			dvfm_add_event(freqs->old, CPU_STATE_RUN,
					freqs->new, CPU_STATE_IDLE);
			dvfm_add_timeslot(freqs->old, CPU_STATE_RUN);
			mspm_add_event(freqs->old, CPU_STATE_RUN);
			break;
		case DVFM_FREQ_POSTCHANGE:
			/* switch_lowpower_start before switch_lowpower_after
			 * is updated in calc_switchtime().
			 * It's invoked in pm function.
			 */
			calc_switchtime_end(freqs->old, freqs->new,
					switch_lowpower_before);
			calc_switchtime_start(freqs->new, freqs->old,
					switch_lowpower_after);
			calc_switchtime_end(freqs->new, freqs->old,
					ticks);
			dvfm_add_event(freqs->new, CPU_STATE_IDLE,
					freqs->old, CPU_STATE_RUN);
			dvfm_add_timeslot(freqs->new, CPU_STATE_IDLE);
			mspm_add_event(freqs->new, CPU_STATE_IDLE);
			break;
		}
	}
	return 0;
}
#else
#define pxa3xx_ticks_to_usec	NULL
#define pxa3xx_ticks_to_sec	NULL
#define pxa3xx_read_time	NULL
#endif

static struct dvfm_driver pxa3xx_driver = {
	.count	= get_op_num,
	.set	= pxa3xx_set_op,
	.dump	= dump_op,
	.name	= get_op_name,
	.request_set	= pxa3xx_request_op,
	.enable_dvfm	= pxa3xx_enable_dvfm,
	.disable_dvfm	= pxa3xx_disable_dvfm,
	.enable_op	= pxa3xx_enable_op,
	.disable_op	= pxa3xx_disable_op,
	.volt_show	= pxa3xx_volt_show,
#ifdef CONFIG_CPU_PXA310
	.freq_show	= pxa3xx_freq_show,
#endif
	.ticks_to_usec	= pxa3xx_ticks_to_usec,
	.ticks_to_sec	= pxa3xx_ticks_to_sec,
	.read_time	= pxa3xx_read_time,
	.get_freq	= pxa3xx_get_freq,
	.check_active_op = pxa3xx_check_active_op,
};

#ifdef CONFIG_PM
static int pxa3xx_freq_suspend(struct platform_device *pdev, pm_message_t state)
{
	current_op = cur_op;
	dvfm_request_op(1);
	return 0;
}

static int pxa3xx_freq_resume(struct platform_device *pdev)
{
	dvfm_request_op(current_op);
	return 0;
}
#else
#define pxa3xx_freq_suspend    NULL
#define pxa3xx_freq_resume     NULL
#endif

static void pxa3xx_poweri2c_init(struct pxa3xx_dvfm_info *info)
{
	uint32_t avcr, svcr, cvcr, pcfr, pvcr;

	if ((info->flags & PXA3xx_USE_POWER_I2C) &&
		((info->cpuid & 0xfff0) == 0x6930)) {
		/* set AVCR for PXA935/PXA940:
		 *	level 0: 1250mv, 0x15
		 *	level 1: 1250mv, 0x15
		 *	level 2: 1250mv, 0x15
		 *	level 3: 1250mv, 0x15
		 */
		avcr = __raw_readl(info->spmu_base + AVCR_OFF);
		avcr &= 0xE0E0E0E0;
		avcr |= (0x15 << 24) | (0x15 << 16) | (0x15 << 8) | 0x15;
		__raw_writel(avcr, info->spmu_base + AVCR_OFF);
		avcr = __raw_readl(info->spmu_base + AVCR_OFF);

		/* set delay */
		pcfr = __raw_readl(info->spmu_base + PCFR_OFF);
		pcfr &= 0x000FFFFF;
		pcfr |= 0xCCF00000;
		/* Disable pullup/pulldown in PWR_SCL and PWR_SDA */
		pcfr |= 0x04;
		__raw_writel(pcfr, info->spmu_base + PCFR_OFF);
		pcfr = __raw_readl(info->spmu_base + PCFR_OFF);

		/* enable FVE,PVE,TVE bit */
		__raw_writel(0xe0500034, info->spmu_base + PVCR_OFF);
	} else if (info->flags & PXA3xx_USE_POWER_I2C) {
		/* set AVCR for PXA300/PXA310/PXA320/PXA930
		 *	level 0: 1000mv, 0x0b
		 *	level 1: 1100mv, 0x0f
		 *	level 2: 1375mv, 0x1a
		 *	level 3: 1400mv, 0x1b
		 */
		avcr = __raw_readl(info->spmu_base + AVCR_OFF);
		avcr &= 0xE0E0E0E0;
		/* PXA930 B0(cpuid 0x6835) requires special setting */
		if (info->cpuid == 0x6835)
			avcr |= (0x1b << 24) | (0x1a << 16) | (0x0f << 8) | 0xb;
		else
			avcr |= (0x0f << 24) | (0x1a << 16) | (0x0f << 8) | 0xb;
		__raw_writel(avcr, info->spmu_base + AVCR_OFF);
		avcr = __raw_readl(info->spmu_base + AVCR_OFF);
		/* set SVCR:
		 * 	level 0: 1100mv, 0x0f
		 * 	level 1: 1200mv, 0x13
		 * 	level 2: 1400mv, 0x1b
		 * 	level 3: 1400mv, 0x1b
		 */
		svcr = __raw_readl(info->spmu_base + SVCR_OFF);
		svcr &= 0xE0E0E0E0;
		if (info->cpuid == 0x6835)
			svcr |= (0x1b << 24) | (0x1b << 16) | (0x13 << 8) | 0xf;
		else
			svcr |= (0x0f << 24) | (0x1b << 16) | (0x13 << 8) | 0xf;
		__raw_writel(svcr, info->spmu_base + SVCR_OFF);
		svcr = __raw_readl(info->spmu_base + SVCR_OFF);
		/* set CVCR:
		 * 	level 0: 925mv, 0x08
		 * 	level 1: 1250mv, 0x15
		 * 	level 2: 1375mv, 0x1a
		 * 	level 3: 1400mv, 0x1b
		 */
		cvcr = __raw_readl(info->spmu_base + CVCR_OFF);
		cvcr &= 0xE0E0E0E0;
		if (info->cpuid == 0x6835)
			cvcr |= (0x1b << 24) | (0x1a << 16) | (0x15 << 8) | 0x08;
		else
			cvcr |= (0x0f << 24) | (0x1a << 16) | (0x15 << 8) | 0x08;
		__raw_writel(cvcr, info->spmu_base + CVCR_OFF);
		cvcr = __raw_readl(info->spmu_base + CVCR_OFF);

		/* set delay */
		pcfr = __raw_readl(info->spmu_base + PCFR_OFF);
		pcfr &= 0x000FFFFF;
		pcfr |= 0xCCF00000;
		/* Disable pullup/pulldown in PWR_SCL and PWR_SDA */
		pcfr |= 0x04;
		__raw_writel(pcfr, info->spmu_base + PCFR_OFF);
		pcfr = __raw_readl(info->spmu_base + PCFR_OFF);

		/* enable FVE,PVE,TVE bit */
		__raw_writel(0xe0500034, info->spmu_base + PVCR_OFF);
	} else {
		/* disable FVE,PVE,TVE,FVC bit */
		pvcr = __raw_readl(info->spmu_base + PVCR_OFF);
		pvcr &= 0x0fffffff;
		__raw_writel(pvcr, info->spmu_base + PVCR_OFF);
	}
}

int gpio_reset_work_around(void)
{
	dvfm_disable_op_name("624M", dvfm_dev_id);
	dvfm_disable_op_name("416M", dvfm_dev_id);
	dvfm_disable_op_name("208M", dvfm_dev_id);
	return 0;
}

static int pxa3xx_freq_probe(struct platform_device *pdev)
{
	struct resource *res;
	struct pxa3xx_freq_mach_info *pdata;
	struct pxa3xx_dvfm_info *info;
	int rc;

	/* initialize the information necessary to frequency/voltage change operation */
	pdata = pdev->dev.platform_data;
	info = kzalloc(sizeof(struct pxa3xx_dvfm_info), GFP_KERNEL);
	info->flags = pdata->flags;
	info->cpuid = read_cpuid(0) & 0xFFFF;

	//info->lcd_clk = clk_get(&pxa_device_fb.dev, "LCDCLK");
	//if (IS_ERR(info->lcd_clk)) goto err;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "clkmgr_regs");
	if (!res) goto err;
	info->clkmgr_base = ioremap(res->start, res->end - res->start + 1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "spmu_regs");
	if (!res) goto err;
	info->spmu_base = ioremap(res->start, res->end - res->start + 1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "bpmu_regs");
	if (!res) goto err;
	info->bpmu_base = ioremap(res->start, res->end - res->start + 1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dmc_regs");
	if (!res) goto err;
	info->dmc_base = ioremap(res->start, res->end - res->start + 1);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "smc_regs");
	if (!res) goto err;
	info->smc_base = ioremap(res->start, res->end - res->start + 1);

	pxa3xx_driver.priv = info;

	pxa3xx_poweri2c_init(info);
	op_init(info, &pxa3xx_dvfm_op_list);

	return dvfm_register_driver(&pxa3xx_driver, &pxa3xx_dvfm_op_list);
err:
	printk("pxa3xx_dvfm init failed\n");
	return -EIO;
}

static int pxa3xx_freq_remove(struct platform_device *pdev)
{
	kfree(pxa3xx_driver.priv);
	return dvfm_unregister_driver(&pxa3xx_driver);
}

static struct platform_driver pxa3xx_freq_driver = {
	.driver = {
		.name	= "pxa3xx-freq",
	},
	.probe		= pxa3xx_freq_probe,
	.remove		= pxa3xx_freq_remove,
#ifdef CONFIG_PM
	//.suspend	= pxa3xx_freq_suspend,
	//.resume		= pxa3xx_freq_resume,
#endif
};


static int __init pxa3xx_freq_init(void)
{
	int ret;
	ret = platform_driver_register(&pxa3xx_freq_driver);
	if (ret)
		goto out;
#ifdef CONFIG_PXA3xx_DVFM_STATS
	ret = dvfm_register_notifier(&notifier_freq_block,
				DVFM_FREQUENCY_NOTIFIER);
#endif
	ret = dvfm_register("DVFM", &dvfm_dev_id);
out:
	return ret;
}

static void __exit pxa3xx_freq_exit(void)
{
#ifdef CONFIG_PXA3xx_DVFM_STATS
	dvfm_unregister_notifier(&notifier_freq_block,
				DVFM_FREQUENCY_NOTIFIER);
#endif
	dvfm_unregister("DVFM", &dvfm_dev_id);
	platform_driver_unregister(&pxa3xx_freq_driver);
}

module_init(pxa3xx_freq_init);
module_exit(pxa3xx_freq_exit);

