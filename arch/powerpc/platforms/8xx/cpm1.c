// SPDX-License-Identifier: GPL-2.0
/*
 * General Purpose functions for the global management of the
 * Communication Processor Module.
 * Copyright (c) 1997 Dan error_act (dmalek@jlc.net)
 *
 * In addition to the individual control of the communication
 * channels, there are a few functions that globally affect the
 * communication processor.
 *
 * Buffer descriptors must be allocated from the dual ported memory
 * space.  The allocator for that is here.  When the communication
 * process is reset, we reclaim the memory available.  There is
 * currently no deallocator for this memory.
 * The amount of space available is platform dependent.  On the
 * MBX, the EPPC software loads additional microcode into the
 * communication processor, and uses some of the DP ram for this
 * purpose.  Current, the first 512 bytes and the last 256 bytes of
 * memory are used.  Right now I am conservative and only use the
 * memory that can never be used for microcode.  If there are
 * applications that require more DP ram, we can expand the boundaries
 * but then we have to be careful of any downloaded microcode.
 */
#include <linux/errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/param.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/of_irq.h>
#include <asm/page.h>
#include <asm/8xx_immap.h>
#include <asm/cpm1.h>
#include <asm/io.h>
#include <asm/rheap.h>
#include <asm/cpm.h>
#include <asm/fixmap.h>

#include <sysdev/fsl_soc.h>

#ifdef CONFIG_8xx_GPIO
#include <linux/gpio/driver.h>
#endif

#define CPM_MAP_SIZE    (0x4000)

cpm8xx_t __iomem *cpmp;  /* Pointer to comm processor space */
immap_t __iomem *mpc8xx_immr = (void __iomem *)VIRT_IMMR_BASE;

void __init cpm_reset(void)
{
	cpmp = &mpc8xx_immr->im_cpm;

#ifndef CONFIG_PPC_EARLY_DEBUG_CPM
	/* Perform a reset. */
	out_be16(&cpmp->cp_cpcr, CPM_CR_RST | CPM_CR_FLG);

	/* Wait for it. */
	while (in_be16(&cpmp->cp_cpcr) & CPM_CR_FLG);
#endif

#ifdef CONFIG_UCODE_PATCH
	cpm_load_patch(cpmp);
#endif

	/*
	 * Set SDMA Bus Request priority 5.
	 * On 860T, this also enables FEC priority 6.  I am not sure
	 * this is what we really want for some applications, but the
	 * manual recommends it.
	 * Bit 25, FAM can also be set to use FEC aggressive mode (860T).
	 */
	if ((mfspr(SPRN_IMMR) & 0xffff) == 0x0900) /* MPC885 */
		out_be32(&mpc8xx_immr->im_siu_conf.sc_sdcr, 0x40);
	else
		out_be32(&mpc8xx_immr->im_siu_conf.sc_sdcr, 1);
}

static DEFINE_SPINLOCK(cmd_lock);

#define MAX_CR_CMD_LOOPS        10000

int cpm_command(u32 command, u8 opcode)
{
	int i, ret;
	unsigned long flags;

	if (command & 0xffffff03)
		return -EINVAL;

	spin_lock_irqsave(&cmd_lock, flags);

	ret = 0;
	out_be16(&cpmp->cp_cpcr, command | CPM_CR_FLG | (opcode << 8));
	for (i = 0; i < MAX_CR_CMD_LOOPS; i++)
		if ((in_be16(&cpmp->cp_cpcr) & CPM_CR_FLG) == 0)
			goto out;

	printk(KERN_ERR "%s(): Not able to issue CPM command\n", __func__);
	ret = -EIO;
out:
	spin_unlock_irqrestore(&cmd_lock, flags);
	return ret;
}
EXPORT_SYMBOL(cpm_command);

/*
 * Set a baud rate generator.  This needs lots of work.  There are
 * four BRGs, any of which can be wired to any channel.
 * The internal baud rate clock is the system clock divided by 16.
 * This assumes the baudrate is 16x oversampled by the uart.
 */
#define BRG_INT_CLK		(get_brgfreq())
#define BRG_UART_CLK		(BRG_INT_CLK/16)
#define BRG_UART_CLK_DIV16	(BRG_UART_CLK/16)

void
cpm_setbrg(uint brg, uint rate)
{
	u32 __iomem *bp;

	/* This is good enough to get SMCs running..... */
	bp = &cpmp->cp_brgc1;
	bp += brg;
	/*
	 * The BRG has a 12-bit counter.  For really slow baud rates (or
	 * really fast processors), we may have to further divide by 16.
	 */
	if (((BRG_UART_CLK / rate) - 1) < 4096)
		out_be32(bp, (((BRG_UART_CLK / rate) - 1) << 1) | CPM_BRG_EN);
	else
		out_be32(bp, (((BRG_UART_CLK_DIV16 / rate) - 1) << 1) |
			      CPM_BRG_EN | CPM_BRG_DIV16);
}
EXPORT_SYMBOL(cpm_setbrg);

struct cpm_ioport16 {
	__be16 dir, par, odr_sor, dat, intr;
	__be16 res[3];
};

struct cpm_ioport32b {
	__be32 dir, par, odr, dat;
};

struct cpm_ioport32e {
	__be32 dir, par, sor, odr, dat;
};

static void __init cpm1_set_pin32(int port, int pin, int flags)
{
	struct cpm_ioport32e __iomem *iop;
	pin = 1 << (31 - pin);

	if (port == CPM_PORTB)
		iop = (struct cpm_ioport32e __iomem *)
		      &mpc8xx_immr->im_cpm.cp_pbdir;
	else
		iop = (struct cpm_ioport32e __iomem *)
		      &mpc8xx_immr->im_cpm.cp_pedir;

	if (flags & CPM_PIN_OUTPUT)
		setbits32(&iop->dir, pin);
	else
		clrbits32(&iop->dir, pin);

	if (!(flags & CPM_PIN_GPIO))
		setbits32(&iop->par, pin);
	else
		clrbits32(&iop->par, pin);

	if (port == CPM_PORTB) {
		if (flags & CPM_PIN_OPENDRAIN)
			setbits16(&mpc8xx_immr->im_cpm.cp_pbodr, pin);
		else
			clrbits16(&mpc8xx_immr->im_cpm.cp_pbodr, pin);
	}

	if (port == CPM_PORTE) {
		if (flags & CPM_PIN_SECONDARY)
			setbits32(&iop->sor, pin);
		else
			clrbits32(&iop->sor, pin);

		if (flags & CPM_PIN_OPENDRAIN)
			setbits32(&mpc8xx_immr->im_cpm.cp_peodr, pin);
		else
			clrbits32(&mpc8xx_immr->im_cpm.cp_peodr, pin);
	}
}

static void __init cpm1_set_pin16(int port, int pin, int flags)
{
	struct cpm_ioport16 __iomem *iop =
		(struct cpm_ioport16 __iomem *)&mpc8xx_immr->im_ioport;

	pin = 1 << (15 - pin);

	if (port != 0)
		iop += port - 1;

	if (flags & CPM_PIN_OUTPUT)
		setbits16(&iop->dir, pin);
	else
		clrbits16(&iop->dir, pin);

	if (!(flags & CPM_PIN_GPIO))
		setbits16(&iop->par, pin);
	else
		clrbits16(&iop->par, pin);

	if (port == CPM_PORTA) {
		if (flags & CPM_PIN_OPENDRAIN)
			setbits16(&iop->odr_sor, pin);
		else
			clrbits16(&iop->odr_sor, pin);
	}
	if (port == CPM_PORTC) {
		if (flags & CPM_PIN_SECONDARY)
			setbits16(&iop->odr_sor, pin);
		else
			clrbits16(&iop->odr_sor, pin);
		if (flags & CPM_PIN_FALLEDGE)
			setbits16(&iop->intr, pin);
		else
			clrbits16(&iop->intr, pin);
	}
}

void __init cpm1_set_pin(enum cpm_port port, int pin, int flags)
{
	if (port == CPM_PORTB || port == CPM_PORTE)
		cpm1_set_pin32(port, pin, flags);
	else
		cpm1_set_pin16(port, pin, flags);
}

int __init cpm1_clk_setup(enum cpm_clk_target target, int clock, int mode)
{
	int shift;
	int i, bits = 0;
	u32 __iomem *reg;
	u32 mask = 7;

	u8 clk_map[][3] = {
		{CPM_CLK_SCC1, CPM_BRG1, 0},
		{CPM_CLK_SCC1, CPM_BRG2, 1},
		{CPM_CLK_SCC1, CPM_BRG3, 2},
		{CPM_CLK_SCC1, CPM_BRG4, 3},
		{CPM_CLK_SCC1, CPM_CLK1, 4},
		{CPM_CLK_SCC1, CPM_CLK2, 5},
		{CPM_CLK_SCC1, CPM_CLK3, 6},
		{CPM_CLK_SCC1, CPM_CLK4, 7},

		{CPM_CLK_SCC2, CPM_BRG1, 0},
		{CPM_CLK_SCC2, CPM_BRG2, 1},
		{CPM_CLK_SCC2, CPM_BRG3, 2},
		{CPM_CLK_SCC2, CPM_BRG4, 3},
		{CPM_CLK_SCC2, CPM_CLK1, 4},
		{CPM_CLK_SCC2, CPM_CLK2, 5},
		{CPM_CLK_SCC2, CPM_CLK3, 6},
		{CPM_CLK_SCC2, CPM_CLK4, 7},

		{CPM_CLK_SCC3, CPM_BRG1, 0},
		{CPM_CLK_SCC3, CPM_BRG2, 1},
		{CPM_CLK_SCC3, CPM_BRG3, 2},
		{CPM_CLK_SCC3, CPM_BRG4, 3},
		{CPM_CLK_SCC3, CPM_CLK5, 4},
		{CPM_CLK_SCC3, CPM_CLK6, 5},
		{CPM_CLK_SCC3, CPM_CLK7, 6},
		{CPM_CLK_SCC3, CPM_CLK8, 7},

		{CPM_CLK_SCC4, CPM_BRG1, 0},
		{CPM_CLK_SCC4, CPM_BRG2, 1},
		{CPM_CLK_SCC4, CPM_BRG3, 2},
		{CPM_CLK_SCC4, CPM_BRG4, 3},
		{CPM_CLK_SCC4, CPM_CLK5, 4},
		{CPM_CLK_SCC4, CPM_CLK6, 5},
		{CPM_CLK_SCC4, CPM_CLK7, 6},
		{CPM_CLK_SCC4, CPM_CLK8, 7},

		{CPM_CLK_SMC1, CPM_BRG1, 0},
		{CPM_CLK_SMC1, CPM_BRG2, 1},
		{CPM_CLK_SMC1, CPM_BRG3, 2},
		{CPM_CLK_SMC1, CPM_BRG4, 3},
		{CPM_CLK_SMC1, CPM_CLK1, 4},
		{CPM_CLK_SMC1, CPM_CLK2, 5},
		{CPM_CLK_SMC1, CPM_CLK3, 6},
		{CPM_CLK_SMC1, CPM_CLK4, 7},

		{CPM_CLK_SMC2, CPM_BRG1, 0},
		{CPM_CLK_SMC2, CPM_BRG2, 1},
		{CPM_CLK_SMC2, CPM_BRG3, 2},
		{CPM_CLK_SMC2, CPM_BRG4, 3},
		{CPM_CLK_SMC2, CPM_CLK5, 4},
		{CPM_CLK_SMC2, CPM_CLK6, 5},
		{CPM_CLK_SMC2, CPM_CLK7, 6},
		{CPM_CLK_SMC2, CPM_CLK8, 7},
	};

	switch (target) {
	case CPM_CLK_SCC1:
		reg = &mpc8xx_immr->im_cpm.cp_sicr;
		shift = 0;
		break;

	case CPM_CLK_SCC2:
		reg = &mpc8xx_immr->im_cpm.cp_sicr;
		shift = 8;
		break;

	case CPM_CLK_SCC3:
		reg = &mpc8xx_immr->im_cpm.cp_sicr;
		shift = 16;
		break;

	case CPM_CLK_SCC4:
		reg = &mpc8xx_immr->im_cpm.cp_sicr;
		shift = 24;
		break;

	case CPM_CLK_SMC1:
		reg = &mpc8xx_immr->im_cpm.cp_simode;
		shift = 12;
		break;

	case CPM_CLK_SMC2:
		reg = &mpc8xx_immr->im_cpm.cp_simode;
		shift = 28;
		break;

	default:
		printk(KERN_ERR "cpm1_clock_setup: invalid clock target\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(clk_map); i++) {
		if (clk_map[i][0] == target && clk_map[i][1] == clock) {
			bits = clk_map[i][2];
			break;
		}
	}

	if (i == ARRAY_SIZE(clk_map)) {
		printk(KERN_ERR "cpm1_clock_setup: invalid clock combination\n");
		return -EINVAL;
	}

	bits <<= shift;
	mask <<= shift;

	if (reg == &mpc8xx_immr->im_cpm.cp_sicr) {
		if (mode == CPM_CLK_RTX) {
			bits |= bits << 3;
			mask |= mask << 3;
		} else if (mode == CPM_CLK_RX) {
			bits <<= 3;
			mask <<= 3;
		}
	}

	out_be32(reg, (in_be32(reg) & ~mask) | bits);

	return 0;
}

/*
 * GPIO LIB API implementation
 */
#ifdef CONFIG_8xx_GPIO

struct cpm1_gpio16_chip {
	struct gpio_chip gc;
	void __iomem *regs;
	spinlock_t lock;

	/* shadowed data register to clear/set bits safely */
	u16 cpdata;

	/* IRQ associated with Pins when relevant */
	int irq[16];
};

static void cpm1_gpio16_save_regs(struct cpm1_gpio16_chip *cpm1_gc)
{
	struct cpm_ioport16 __iomem *iop = cpm1_gc->regs;

	cpm1_gc->cpdata = in_be16(&iop->dat);
}

static int cpm1_gpio16_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct cpm1_gpio16_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport16 __iomem *iop = cpm1_gc->regs;
	u16 pin_mask;

	pin_mask = 1 << (15 - gpio);

	return !!(in_be16(&iop->dat) & pin_mask);
}

static void __cpm1_gpio16_set(struct cpm1_gpio16_chip *cpm1_gc, u16 pin_mask, int value)
{
	struct cpm_ioport16 __iomem *iop = cpm1_gc->regs;

	if (value)
		cpm1_gc->cpdata |= pin_mask;
	else
		cpm1_gc->cpdata &= ~pin_mask;

	out_be16(&iop->dat, cpm1_gc->cpdata);
}

static int cpm1_gpio16_set(struct gpio_chip *gc, unsigned int gpio, int value)
{
	struct cpm1_gpio16_chip *cpm1_gc = gpiochip_get_data(gc);
	unsigned long flags;
	u16 pin_mask = 1 << (15 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	__cpm1_gpio16_set(cpm1_gc, pin_mask, value);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

static int cpm1_gpio16_to_irq(struct gpio_chip *gc, unsigned int gpio)
{
	struct cpm1_gpio16_chip *cpm1_gc = gpiochip_get_data(gc);

	return cpm1_gc->irq[gpio] ? : -ENXIO;
}

static int cpm1_gpio16_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct cpm1_gpio16_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport16 __iomem *iop = cpm1_gc->regs;
	unsigned long flags;
	u16 pin_mask = 1 << (15 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	setbits16(&iop->dir, pin_mask);
	__cpm1_gpio16_set(cpm1_gc, pin_mask, val);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

static int cpm1_gpio16_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct cpm1_gpio16_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport16 __iomem *iop = cpm1_gc->regs;
	unsigned long flags;
	u16 pin_mask = 1 << (15 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	clrbits16(&iop->dir, pin_mask);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

int cpm1_gpiochip_add16(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct cpm1_gpio16_chip *cpm1_gc;
	struct gpio_chip *gc;
	u16 mask;

	cpm1_gc = devm_kzalloc(dev, sizeof(*cpm1_gc), GFP_KERNEL);
	if (!cpm1_gc)
		return -ENOMEM;

	spin_lock_init(&cpm1_gc->lock);

	if (!of_property_read_u16(np, "fsl,cpm1-gpio-irq-mask", &mask)) {
		int i, j;

		for (i = 0, j = 0; i < 16; i++)
			if (mask & (1 << (15 - i)))
				cpm1_gc->irq[i] = irq_of_parse_and_map(np, j++);
	}

	gc = &cpm1_gc->gc;
	gc->base = -1;
	gc->ngpio = 16;
	gc->direction_input = cpm1_gpio16_dir_in;
	gc->direction_output = cpm1_gpio16_dir_out;
	gc->get = cpm1_gpio16_get;
	gc->set_rv = cpm1_gpio16_set;
	gc->to_irq = cpm1_gpio16_to_irq;
	gc->parent = dev;
	gc->owner = THIS_MODULE;

	gc->label = devm_kasprintf(dev, GFP_KERNEL, "%pOF", np);
	if (!gc->label)
		return -ENOMEM;

	cpm1_gc->regs = devm_of_iomap(dev, np, 0, NULL);
	if (IS_ERR(cpm1_gc->regs))
		return PTR_ERR(cpm1_gc->regs);

	cpm1_gpio16_save_regs(cpm1_gc);

	return devm_gpiochip_add_data(dev, gc, cpm1_gc);
}

struct cpm1_gpio32_chip {
	struct gpio_chip gc;
	void __iomem *regs;
	spinlock_t lock;

	/* shadowed data register to clear/set bits safely */
	u32 cpdata;
};

static void cpm1_gpio32_save_regs(struct cpm1_gpio32_chip *cpm1_gc)
{
	struct cpm_ioport32b __iomem *iop = cpm1_gc->regs;

	cpm1_gc->cpdata = in_be32(&iop->dat);
}

static int cpm1_gpio32_get(struct gpio_chip *gc, unsigned int gpio)
{
	struct cpm1_gpio32_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport32b __iomem *iop = cpm1_gc->regs;
	u32 pin_mask;

	pin_mask = 1 << (31 - gpio);

	return !!(in_be32(&iop->dat) & pin_mask);
}

static void __cpm1_gpio32_set(struct cpm1_gpio32_chip *cpm1_gc, u32 pin_mask, int value)
{
	struct cpm_ioport32b __iomem *iop = cpm1_gc->regs;

	if (value)
		cpm1_gc->cpdata |= pin_mask;
	else
		cpm1_gc->cpdata &= ~pin_mask;

	out_be32(&iop->dat, cpm1_gc->cpdata);
}

static int cpm1_gpio32_set(struct gpio_chip *gc, unsigned int gpio, int value)
{
	struct cpm1_gpio32_chip *cpm1_gc = gpiochip_get_data(gc);
	unsigned long flags;
	u32 pin_mask = 1 << (31 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	__cpm1_gpio32_set(cpm1_gc, pin_mask, value);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

static int cpm1_gpio32_dir_out(struct gpio_chip *gc, unsigned int gpio, int val)
{
	struct cpm1_gpio32_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport32b __iomem *iop = cpm1_gc->regs;
	unsigned long flags;
	u32 pin_mask = 1 << (31 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	setbits32(&iop->dir, pin_mask);
	__cpm1_gpio32_set(cpm1_gc, pin_mask, val);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

static int cpm1_gpio32_dir_in(struct gpio_chip *gc, unsigned int gpio)
{
	struct cpm1_gpio32_chip *cpm1_gc = gpiochip_get_data(gc);
	struct cpm_ioport32b __iomem *iop = cpm1_gc->regs;
	unsigned long flags;
	u32 pin_mask = 1 << (31 - gpio);

	spin_lock_irqsave(&cpm1_gc->lock, flags);

	clrbits32(&iop->dir, pin_mask);

	spin_unlock_irqrestore(&cpm1_gc->lock, flags);

	return 0;
}

int cpm1_gpiochip_add32(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct cpm1_gpio32_chip *cpm1_gc;
	struct gpio_chip *gc;

	cpm1_gc = devm_kzalloc(dev, sizeof(*cpm1_gc), GFP_KERNEL);
	if (!cpm1_gc)
		return -ENOMEM;

	spin_lock_init(&cpm1_gc->lock);

	gc = &cpm1_gc->gc;
	gc->base = -1;
	gc->ngpio = 32;
	gc->direction_input = cpm1_gpio32_dir_in;
	gc->direction_output = cpm1_gpio32_dir_out;
	gc->get = cpm1_gpio32_get;
	gc->set_rv = cpm1_gpio32_set;
	gc->parent = dev;
	gc->owner = THIS_MODULE;

	gc->label = devm_kasprintf(dev, GFP_KERNEL, "%pOF", np);
	if (!gc->label)
		return -ENOMEM;

	cpm1_gc->regs = devm_of_iomap(dev, np, 0, NULL);
	if (IS_ERR(cpm1_gc->regs))
		return PTR_ERR(cpm1_gc->regs);

	cpm1_gpio32_save_regs(cpm1_gc);

	return devm_gpiochip_add_data(dev, gc, cpm1_gc);
}

#endif /* CONFIG_8xx_GPIO */
