/*
 * GPIO driver for Marvell SoCs
 *
 * Copyright (C) 2012 Marvell
 *
 * Thomas Petazzoni <thomas.petazzoni@free-electrons.com>
 * Andrew Lunn <andrew@lunn.ch>
 * Sebastian Hesselbarth <sebastian.hesselbarth@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 *
 * This driver is a fairly straightforward GPIO driver for the
 * complete family of Marvell EBU SoC platforms (Orion, Dove,
 * Kirkwood, Discovery, Armada 370/XP). The only complexity of this
 * driver is the different register layout that exists between the
 * non-SMP platforms (Orion, Dove, Kirkwood, Armada 370) and the SMP
 * platforms (MV78200 from the Discovery family and the Armada
 * XP). Therefore, this driver handles three variants of the GPIO
 * block:
 * - the basic variant, called "orion-gpio", with the simplest
 *   register set. Used on Orion, Dove, Kirkwoord, Armada 370 and
 *   non-SMP Discovery systems
 * - the mv78200 variant for MV78200 Discovery systems. This variant
 *   turns the edge mask and level mask registers into CPU0 edge
 *   mask/level mask registers, and adds CPU1 edge mask/level mask
 *   registers.
 * - the armadaxp variant for Armada XP systems. This variant keeps
 *   the normal cause/edge mask/level mask registers when the global
 *   interrupts are used, but adds per-CPU cause/edge mask/level mask
 *   registers n a separate memory area for the per-CPU GPIO
 *   interrupts.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/of_device.h>
#include <linux/pwm.h>
#include <linux/clk.h>
#include <linux/pinctrl/consumer.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/platform_device.h>

#include "gpiolib.h"

/*
 * GPIO unit register offsets.
 */
#define GPIO_OUT_OFF			0x0000
#define GPIO_IO_CONF_OFF		0x0004
#define GPIO_BLINK_EN_OFF		0x0008
#define GPIO_IN_POL_OFF			0x000c
#define GPIO_DATA_IN_OFF		0x0010
#define GPIO_EDGE_CAUSE_OFF		0x0014
#define GPIO_EDGE_MASK_OFF		0x0018
#define GPIO_LEVEL_MASK_OFF		0x001c
#define GPIO_BLINK_CNT_SELECT_OFF	0x0020

/*
 * PWM register offsets.
 */
#define PWM_BLINK_ON_DURATION_OFF	0x0
#define PWM_BLINK_OFF_DURATION_OFF	0x4


/* The MV78200 has per-CPU registers for edge mask and level mask */
#define GPIO_EDGE_MASK_MV78200_OFF(cpu)	  ((cpu) ? 0x30 : 0x18)
#define GPIO_LEVEL_MASK_MV78200_OFF(cpu)  ((cpu) ? 0x34 : 0x1C)

/*
 * The Armada XP has per-CPU registers for interrupt cause, interrupt
 * mask and interrupt level mask. Those are relative to the
 * percpu_membase.
 */
#define GPIO_EDGE_CAUSE_ARMADAXP_OFF(cpu) ((cpu) * 0x4)
#define GPIO_EDGE_MASK_ARMADAXP_OFF(cpu)  (0x10 + (cpu) * 0x4)
#define GPIO_LEVEL_MASK_ARMADAXP_OFF(cpu) (0x20 + (cpu) * 0x4)

#define MVEBU_GPIO_SOC_VARIANT_ORION	0x1
#define MVEBU_GPIO_SOC_VARIANT_MV78200	0x2
#define MVEBU_GPIO_SOC_VARIANT_ARMADAXP 0x3

#define MVEBU_MAX_GPIO_PER_BANK		32

struct mvebu_pwm {
	void __iomem		*membase;
	unsigned long		 clk_rate;
	struct gpio_desc	*gpiod;
	bool			 used;
	unsigned int		 pin;
	struct pwm_chip		 chip;
	int			 id;
	spinlock_t		 lock;
	struct mvebu_gpio_chip	*mvchip;

	/* Used to preserve GPIO/PWM registers across suspend/resume */
	u32			 blink_select;
	u32			 blink_on_duration;
	u32			 blink_off_duration;
};

struct mvebu_gpio_chip {
	struct gpio_chip   chip;
	spinlock_t	   lock;
	void __iomem	  *membase;
	void __iomem	  *percpu_membase;
	int		   irqbase;
	struct irq_domain *domain;
	int		   soc_variant;

	/* Used for PWM support */
	struct clk	  *clk;
	struct mvebu_pwm  *mvpwm;

	/* Used to preserve GPIO registers across suspend/resume */
	u32                out_reg;
	u32                io_conf_reg;
	u32                blink_en_reg;
	u32                in_pol_reg;
	u32                edge_mask_regs[4];
	u32                level_mask_regs[4];
};

/*
 * Functions returning addresses of individual registers for a given
 * GPIO controller.
 */
static inline void __iomem *mvebu_gpioreg_out(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_OUT_OFF;
}

static void __iomem *mvebu_gpioreg_blink_select(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_BLINK_CNT_SELECT_OFF;
}

static inline void __iomem *mvebu_gpioreg_blink(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_BLINK_EN_OFF;
}

static inline void __iomem *
mvebu_gpioreg_io_conf(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_IO_CONF_OFF;
}

static inline void __iomem *mvebu_gpioreg_in_pol(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_IN_POL_OFF;
}

static inline void __iomem *
mvebu_gpioreg_data_in(struct mvebu_gpio_chip *mvchip)
{
	return mvchip->membase + GPIO_DATA_IN_OFF;
}

static inline void __iomem *
mvebu_gpioreg_edge_cause(struct mvebu_gpio_chip *mvchip)
{
	int cpu;

	switch (mvchip->soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		return mvchip->membase + GPIO_EDGE_CAUSE_OFF;
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		cpu = smp_processor_id();
		return mvchip->percpu_membase +
			GPIO_EDGE_CAUSE_ARMADAXP_OFF(cpu);
	default:
		BUG();
	}
}

static inline void __iomem *
mvebu_gpioreg_edge_mask(struct mvebu_gpio_chip *mvchip)
{
	int cpu;

	switch (mvchip->soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
		return mvchip->membase + GPIO_EDGE_MASK_OFF;
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		cpu = smp_processor_id();
		return mvchip->membase + GPIO_EDGE_MASK_MV78200_OFF(cpu);
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		cpu = smp_processor_id();
		return mvchip->percpu_membase +
			GPIO_EDGE_MASK_ARMADAXP_OFF(cpu);
	default:
		BUG();
	}
}

static void __iomem *mvebu_gpioreg_level_mask(struct mvebu_gpio_chip *mvchip)
{
	int cpu;

	switch (mvchip->soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
		return mvchip->membase + GPIO_LEVEL_MASK_OFF;
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		cpu = smp_processor_id();
		return mvchip->membase + GPIO_LEVEL_MASK_MV78200_OFF(cpu);
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		cpu = smp_processor_id();
		return mvchip->percpu_membase +
			GPIO_LEVEL_MASK_ARMADAXP_OFF(cpu);
	default:
		BUG();
	}
}

/*
 * Functions returning addresses of individual registers for a given
 * PWM controller.
 */
static void __iomem *mvebu_pwmreg_blink_on_duration(struct mvebu_pwm *mvpwm)
{
	return mvpwm->membase + PWM_BLINK_ON_DURATION_OFF;
}

static void __iomem *mvebu_pwmreg_blink_off_duration(struct mvebu_pwm *mvpwm)
{
	return mvpwm->membase + PWM_BLINK_OFF_DURATION_OFF;
}

/*
 * Functions implementing the gpio_chip methods
 */

static void mvebu_gpio_set(struct gpio_chip *chip, unsigned pin, int value)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	unsigned long flags;
	u32 u;

	spin_lock_irqsave(&mvchip->lock, flags);
	u = readl_relaxed(mvebu_gpioreg_out(mvchip));
	if (value)
		u |= 1 << pin;
	else
		u &= ~(1 << pin);
	writel_relaxed(u, mvebu_gpioreg_out(mvchip));
	spin_unlock_irqrestore(&mvchip->lock, flags);
}

static int mvebu_gpio_get(struct gpio_chip *chip, unsigned pin)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	u32 u;

	if (readl_relaxed(mvebu_gpioreg_io_conf(mvchip)) & (1 << pin)) {
		u = readl_relaxed(mvebu_gpioreg_data_in(mvchip)) ^
			readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
	} else {
		u = readl_relaxed(mvebu_gpioreg_out(mvchip));
	}

	return (u >> pin) & 1;
}

static void mvebu_gpio_blink(struct gpio_chip *chip, unsigned pin, int value)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	unsigned long flags;
	u32 u;

	spin_lock_irqsave(&mvchip->lock, flags);
	u = readl_relaxed(mvebu_gpioreg_blink(mvchip));
	if (value)
		u |= 1 << pin;
	else
		u &= ~(1 << pin);
	writel_relaxed(u, mvebu_gpioreg_blink(mvchip));
	spin_unlock_irqrestore(&mvchip->lock, flags);
}

static int mvebu_gpio_direction_input(struct gpio_chip *chip, unsigned pin)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	unsigned long flags;
	int ret;
	u32 u;

	/* Check with the pinctrl driver whether this pin is usable as
	 * an input GPIO */
	ret = pinctrl_gpio_direction_input(chip->base + pin);
	if (ret)
		return ret;

	spin_lock_irqsave(&mvchip->lock, flags);
	u = readl_relaxed(mvebu_gpioreg_io_conf(mvchip));
	u |= 1 << pin;
	writel_relaxed(u, mvebu_gpioreg_io_conf(mvchip));
	spin_unlock_irqrestore(&mvchip->lock, flags);

	return 0;
}

static int mvebu_gpio_direction_output(struct gpio_chip *chip, unsigned pin,
				       int value)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	unsigned long flags;
	int ret;
	u32 u;

	/* Check with the pinctrl driver whether this pin is usable as
	 * an output GPIO */
	ret = pinctrl_gpio_direction_output(chip->base + pin);
	if (ret)
		return ret;

	mvebu_gpio_blink(chip, pin, 0);
	mvebu_gpio_set(chip, pin, value);

	spin_lock_irqsave(&mvchip->lock, flags);
	u = readl_relaxed(mvebu_gpioreg_io_conf(mvchip));
	u &= ~(1 << pin);
	writel_relaxed(u, mvebu_gpioreg_io_conf(mvchip));
	spin_unlock_irqrestore(&mvchip->lock, flags);

	return 0;
}

static int mvebu_gpio_to_irq(struct gpio_chip *chip, unsigned pin)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	return irq_create_mapping(mvchip->domain, pin);
}

/*
 * Functions implementing the irq_chip methods
 */
static void mvebu_gpio_irq_ack(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	u32 mask = ~(1 << (d->irq - gc->irq_base));

	irq_gc_lock(gc);
	writel_relaxed(mask, mvebu_gpioreg_edge_cause(mvchip));
	irq_gc_unlock(gc);
}

static void mvebu_gpio_edge_irq_mask(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	u32 mask = 1 << (d->irq - gc->irq_base);

	irq_gc_lock(gc);
	ct->mask_cache_priv &= ~mask;

	writel_relaxed(ct->mask_cache_priv, mvebu_gpioreg_edge_mask(mvchip));
	irq_gc_unlock(gc);
}

static void mvebu_gpio_edge_irq_unmask(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	struct irq_chip_type *ct = irq_data_get_chip_type(d);

	u32 mask = 1 << (d->irq - gc->irq_base);

	irq_gc_lock(gc);
	ct->mask_cache_priv |= mask;
	writel_relaxed(ct->mask_cache_priv, mvebu_gpioreg_edge_mask(mvchip));
	irq_gc_unlock(gc);
}

static void mvebu_gpio_level_irq_mask(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	struct irq_chip_type *ct = irq_data_get_chip_type(d);

	u32 mask = 1 << (d->irq - gc->irq_base);

	irq_gc_lock(gc);
	ct->mask_cache_priv &= ~mask;
	writel_relaxed(ct->mask_cache_priv, mvebu_gpioreg_level_mask(mvchip));
	irq_gc_unlock(gc);
}

static void mvebu_gpio_level_irq_unmask(struct irq_data *d)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	struct irq_chip_type *ct = irq_data_get_chip_type(d);

	u32 mask = 1 << (d->irq - gc->irq_base);

	irq_gc_lock(gc);
	ct->mask_cache_priv |= mask;
	writel_relaxed(ct->mask_cache_priv, mvebu_gpioreg_level_mask(mvchip));
	irq_gc_unlock(gc);
}

/*****************************************************************************
 * MVEBU GPIO IRQ
 *
 * GPIO_IN_POL register controls whether GPIO_DATA_IN will hold the same
 * value of the line or the opposite value.
 *
 * Level IRQ handlers: DATA_IN is used directly as cause register.
 *		       Interrupt are masked by LEVEL_MASK registers.
 * Edge IRQ handlers:  Change in DATA_IN are latched in EDGE_CAUSE.
 *		       Interrupt are masked by EDGE_MASK registers.
 * Both-edge handlers: Similar to regular Edge handlers, but also swaps
 *		       the polarity to catch the next line transaction.
 *		       This is a race condition that might not perfectly
 *		       work on some use cases.
 *
 * Every eight GPIO lines are grouped (OR'ed) before going up to main
 * cause register.
 *
 *		      EDGE  cause    mask
 *	  data-in   /--------| |-----| |----\
 *     -----| |-----			     ---- to main cause reg
 *	     X	    \----------------| |----/
 *	  polarity    LEVEL	     mask
 *
 ****************************************************************************/

static int mvebu_gpio_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct irq_chip_generic *gc = irq_data_get_irq_chip_data(d);
	struct irq_chip_type *ct = irq_data_get_chip_type(d);
	struct mvebu_gpio_chip *mvchip = gc->private;
	int pin;
	u32 u;

	pin = d->hwirq;

	u = readl_relaxed(mvebu_gpioreg_io_conf(mvchip)) & (1 << pin);
	if (!u)
		return -EINVAL;

	type &= IRQ_TYPE_SENSE_MASK;
	if (type == IRQ_TYPE_NONE)
		return -EINVAL;

	/* Check if we need to change chip and handler */
	if (!(ct->type & type))
		if (irq_setup_alt_chip(d, type))
			return -EINVAL;

	/*
	 * Configure interrupt polarity.
	 */
	switch (type) {
	case IRQ_TYPE_EDGE_RISING:
	case IRQ_TYPE_LEVEL_HIGH:
		u = readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
		u &= ~(1 << pin);
		writel_relaxed(u, mvebu_gpioreg_in_pol(mvchip));
		break;
	case IRQ_TYPE_EDGE_FALLING:
	case IRQ_TYPE_LEVEL_LOW:
		u = readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
		u |= 1 << pin;
		writel_relaxed(u, mvebu_gpioreg_in_pol(mvchip));
		break;
	case IRQ_TYPE_EDGE_BOTH: {
		u32 v;

		v = readl_relaxed(mvebu_gpioreg_in_pol(mvchip)) ^
			readl_relaxed(mvebu_gpioreg_data_in(mvchip));

		/*
		 * set initial polarity based on current input level
		 */
		u = readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
		if (v & (1 << pin))
			u |= 1 << pin;		/* falling */
		else
			u &= ~(1 << pin);	/* rising */
		writel_relaxed(u, mvebu_gpioreg_in_pol(mvchip));
		break;
	}
	}
	return 0;
}

static void mvebu_gpio_irq_handler(struct irq_desc *desc)
{
	struct mvebu_gpio_chip *mvchip = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 cause, type;
	int i;

	if (mvchip == NULL)
		return;

	chained_irq_enter(chip, desc);

	cause = readl_relaxed(mvebu_gpioreg_data_in(mvchip)) &
		readl_relaxed(mvebu_gpioreg_level_mask(mvchip));
	cause |= readl_relaxed(mvebu_gpioreg_edge_cause(mvchip)) &
		readl_relaxed(mvebu_gpioreg_edge_mask(mvchip));

	for (i = 0; i < mvchip->chip.ngpio; i++) {
		int irq;

		irq = mvchip->irqbase + i;

		if (!(cause & (1 << i)))
			continue;

		type = irq_get_trigger_type(irq);
		if ((type & IRQ_TYPE_SENSE_MASK) == IRQ_TYPE_EDGE_BOTH) {
			/* Swap polarity (race with GPIO line) */
			u32 polarity;

			polarity = readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
			polarity ^= 1 << i;
			writel_relaxed(polarity, mvebu_gpioreg_in_pol(mvchip));
		}

		generic_handle_irq(irq);
	}

	chained_irq_exit(chip, desc);
}

/*
 * Functions implementing the pwm_chip methods
 */
static struct mvebu_pwm *to_mvebu_pwm(struct pwm_chip *chip)
{
	return container_of(chip, struct mvebu_pwm, chip);
}

static int mvebu_pwm_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct mvebu_pwm *mvpwm = to_mvebu_pwm(chip);
	struct mvebu_gpio_chip *mvchip = mvpwm->mvchip;
	struct gpio_desc *desc;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&mvpwm->lock, flags);
	if (mvpwm->gpiod) {
		ret = -EBUSY;
	} else {
		desc = gpio_to_desc(mvchip->chip.base + pwm->hwpwm);
		if (!desc) {
			ret = -ENODEV;
			goto out;
		}
		ret = gpiod_request(desc, "mvebu-pwm");
		if (ret)
			goto out;

		ret = gpiod_direction_output(desc, 0);
		if (ret) {
			gpiod_free(desc);
			goto out;
		}

		mvpwm->gpiod = desc;
		mvpwm->pin = pwm->pwm - mvchip->chip.base;
		mvpwm->used = true;
	}

out:
	spin_unlock_irqrestore(&mvpwm->lock, flags);
	return ret;
}

static void mvebu_pwm_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct mvebu_pwm *mvpwm = to_mvebu_pwm(chip);
	unsigned long flags;

	spin_lock_irqsave(&mvpwm->lock, flags);
	gpiod_free(mvpwm->gpiod);
	mvpwm->used = false;
	spin_unlock_irqrestore(&mvpwm->lock, flags);
}

static int mvebu_pwm_config(struct pwm_chip *chip, struct pwm_device *pwmd,
			    int duty_ns, int period_ns)
{
	struct mvebu_pwm *pwm = to_mvebu_pwm(chip);
	struct mvebu_gpio_chip *mvchip = pwm->mvchip;
	unsigned int on, off;
	unsigned long long val;
	u32 u;

	val = (unsigned long long) pwm->clk_rate * duty_ns;
	do_div(val, NSEC_PER_SEC);
	if (val > UINT_MAX)
		return -EINVAL;
	if (val)
		on = val;
	else
		on = 1;

	val = (unsigned long long) pwm->clk_rate * (period_ns - duty_ns);
	do_div(val, NSEC_PER_SEC);
	if (val > UINT_MAX)
		return -EINVAL;
	if (val)
		off = val;
	else
		off = 1;

	u = readl_relaxed(mvebu_gpioreg_blink_select(mvchip));
	u &= ~(1 << pwm->pin);
	u |= (pwm->id << pwm->pin);
	writel_relaxed(u, mvebu_gpioreg_blink_select(mvchip));

	writel_relaxed(on, mvebu_pwmreg_blink_on_duration(pwm));
	writel_relaxed(off, mvebu_pwmreg_blink_off_duration(pwm));

	return 0;
}

static int mvebu_pwm_enable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct mvebu_pwm *mvpwm = to_mvebu_pwm(chip);
	struct mvebu_gpio_chip *mvchip = mvpwm->mvchip;

	mvebu_gpio_blink(&mvchip->chip, mvpwm->pin, 1);

	return 0;
}

static void mvebu_pwm_disable(struct pwm_chip *chip, struct pwm_device *pwm)
{
	struct mvebu_pwm *mvpwm = to_mvebu_pwm(chip);
	struct mvebu_gpio_chip *mvchip = mvpwm->mvchip;

	mvebu_gpio_blink(&mvchip->chip, mvpwm->pin, 0);
}

static const struct pwm_ops mvebu_pwm_ops = {
	.request = mvebu_pwm_request,
	.free = mvebu_pwm_free,
	.config = mvebu_pwm_config,
	.enable = mvebu_pwm_enable,
	.disable = mvebu_pwm_disable,
	.owner = THIS_MODULE,
};

static void mvebu_pwm_suspend(struct mvebu_gpio_chip *mvchip)
{
	struct mvebu_pwm *mvpwm = mvchip->mvpwm;

	mvpwm->blink_select =
		readl_relaxed(mvebu_gpioreg_blink_select(mvchip));
	mvpwm->blink_on_duration =
		readl_relaxed(mvebu_pwmreg_blink_on_duration(mvpwm));
	mvpwm->blink_off_duration =
		readl_relaxed(mvebu_pwmreg_blink_off_duration(mvpwm));
}

static void mvebu_pwm_resume(struct mvebu_gpio_chip *mvchip)
{
	struct mvebu_pwm *mvpwm = mvchip->mvpwm;

	writel_relaxed(mvpwm->blink_select,
		       mvebu_gpioreg_blink_select(mvchip));
	writel_relaxed(mvpwm->blink_on_duration,
		       mvebu_pwmreg_blink_on_duration(mvpwm));
	writel_relaxed(mvpwm->blink_off_duration,
		       mvebu_pwmreg_blink_off_duration(mvpwm));
}

/*
 * Armada 370/XP has simple PWM support for gpio lines. Other SoCs
 * don't have this hardware. So if we don't have the necessary
 * resource, it is not an error.
 */
static int mvebu_pwm_probe(struct platform_device *pdev,
		    struct mvebu_gpio_chip *mvchip,
		    int id)
{
	struct device *dev = &pdev->dev;
	struct mvebu_pwm *mvpwm;
	struct resource *res;
	u32 set;

	if (!of_device_is_compatible(mvchip->chip.of_node,
				     "marvell,armada-370-gpio"))
		return 0;

	if (IS_ERR(mvchip->clk))
		return PTR_ERR(mvchip->clk);

	/*
	 * There are only two sets of PWM configuration registers for
	 * all the GPIO lines on those SoCs which this driver reserves
	 * for the first two GPIO chips. So if the resource is missing
	 * we can't treat it as an error.
	 */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pwm");
	if (!res)
		return 0;

	/*
	 * Use set A for lines of GPIO chip with id 0, B for GPIO chip
	 * with id 1. Don't allow further GPIO chips to be used for PWM.
	 */
	if (id == 0)
		set = 0;
	else if (id == 1)
		set = U32_MAX;
	else
		return -EINVAL;
	writel_relaxed(set, mvebu_gpioreg_blink_select(mvchip));
	mvpwm->id = id;

	mvpwm = devm_kzalloc(dev, sizeof(struct mvebu_pwm), GFP_KERNEL);
	if (!mvpwm)
		return -ENOMEM;
	mvchip->mvpwm = mvpwm;
	mvpwm->mvchip = mvchip;

	mvpwm->membase = devm_ioremap_resource(dev, res);
	if (IS_ERR(mvpwm->membase))
		return PTR_ERR(mvpwm->membase);

	mvpwm->clk_rate = clk_get_rate(mvchip->clk);
	if (!mvpwm->clk_rate) {
		dev_err(dev, "failed to get clock rate\n");
		return -EINVAL;
	}

	mvpwm->chip.dev = dev;
	mvpwm->chip.ops = &mvebu_pwm_ops;
	mvpwm->chip.npwm = mvchip->chip.ngpio;

	spin_lock_init(&mvpwm->lock);

	return pwmchip_add(&mvpwm->chip);
}

#ifdef CONFIG_DEBUG_FS
#include <linux/seq_file.h>

static void mvebu_gpio_dbg_show(struct seq_file *s, struct gpio_chip *chip)
{
	struct mvebu_gpio_chip *mvchip =
		container_of(chip, struct mvebu_gpio_chip, chip);
	u32 out, io_conf, blink, in_pol, data_in, cause, edg_msk, lvl_msk;
	int i;

	out	= readl_relaxed(mvebu_gpioreg_out(mvchip));
	io_conf	= readl_relaxed(mvebu_gpioreg_io_conf(mvchip));
	blink	= readl_relaxed(mvebu_gpioreg_blink(mvchip));
	in_pol	= readl_relaxed(mvebu_gpioreg_in_pol(mvchip));
	data_in	= readl_relaxed(mvebu_gpioreg_data_in(mvchip));
	cause	= readl_relaxed(mvebu_gpioreg_edge_cause(mvchip));
	edg_msk	= readl_relaxed(mvebu_gpioreg_edge_mask(mvchip));
	lvl_msk	= readl_relaxed(mvebu_gpioreg_level_mask(mvchip));

	for (i = 0; i < chip->ngpio; i++) {
		const char *label;
		u32 msk;
		bool is_out;

		label = gpiochip_is_requested(chip, i);
		if (!label)
			continue;

		msk = 1 << i;
		is_out = !(io_conf & msk);

		seq_printf(s, " gpio-%-3d (%-20.20s)", chip->base + i, label);

		if (is_out) {
			seq_printf(s, " out %s %s\n",
				   out & msk ? "hi" : "lo",
				   blink & msk ? "(blink )" : "");
			continue;
		}

		seq_printf(s, " in  %s (act %s) - IRQ",
			   (data_in ^ in_pol) & msk  ? "hi" : "lo",
			   in_pol & msk ? "lo" : "hi");
		if (!((edg_msk | lvl_msk) & msk)) {
			seq_puts(s, " disabled\n");
			continue;
		}
		if (edg_msk & msk)
			seq_puts(s, " edge ");
		if (lvl_msk & msk)
			seq_puts(s, " level");
		seq_printf(s, " (%s)\n", cause & msk ? "pending" : "clear  ");
	}
}
#else
#define mvebu_gpio_dbg_show NULL
#endif

static const struct of_device_id mvebu_gpio_of_match[] = {
	{
		.compatible = "marvell,orion-gpio",
		.data	    = (void *) MVEBU_GPIO_SOC_VARIANT_ORION,
	},
	{
		.compatible = "marvell,mv78200-gpio",
		.data	    = (void *) MVEBU_GPIO_SOC_VARIANT_MV78200,
	},
	{
		.compatible = "marvell,armadaxp-gpio",
		.data	    = (void *) MVEBU_GPIO_SOC_VARIANT_ARMADAXP,
	},
	{
		.compatible = "marvell,armada-370-gpio",
		.data	    = (void *) MVEBU_GPIO_SOC_VARIANT_ORION,
	},
	{
		/* sentinel */
	},
};
MODULE_DEVICE_TABLE(of, mvebu_gpio_of_match);

static int mvebu_gpio_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mvebu_gpio_chip *mvchip = platform_get_drvdata(pdev);
	int i;

	mvchip->out_reg = readl(mvebu_gpioreg_out(mvchip));
	mvchip->io_conf_reg = readl(mvebu_gpioreg_io_conf(mvchip));
	mvchip->blink_en_reg = readl(mvebu_gpioreg_blink(mvchip));
	mvchip->in_pol_reg = readl(mvebu_gpioreg_in_pol(mvchip));

	switch (mvchip->soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
		mvchip->edge_mask_regs[0] =
			readl(mvchip->membase + GPIO_EDGE_MASK_OFF);
		mvchip->level_mask_regs[0] =
			readl(mvchip->membase + GPIO_LEVEL_MASK_OFF);
		break;
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		for (i = 0; i < 2; i++) {
			mvchip->edge_mask_regs[i] =
				readl(mvchip->membase +
				      GPIO_EDGE_MASK_MV78200_OFF(i));
			mvchip->level_mask_regs[i] =
				readl(mvchip->membase +
				      GPIO_LEVEL_MASK_MV78200_OFF(i));
		}
		break;
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		for (i = 0; i < 4; i++) {
			mvchip->edge_mask_regs[i] =
				readl(mvchip->membase +
				      GPIO_EDGE_MASK_ARMADAXP_OFF(i));
			mvchip->level_mask_regs[i] =
				readl(mvchip->membase +
				      GPIO_LEVEL_MASK_ARMADAXP_OFF(i));
		}
		break;
	default:
		BUG();
	}

	if (IS_ENABLED(CONFIG_PWM))
		mvebu_pwm_suspend(mvchip);

	return 0;
}

static int mvebu_gpio_resume(struct platform_device *pdev)
{
	struct mvebu_gpio_chip *mvchip = platform_get_drvdata(pdev);
	int i;

	writel(mvchip->out_reg, mvebu_gpioreg_out(mvchip));
	writel(mvchip->io_conf_reg, mvebu_gpioreg_io_conf(mvchip));
	writel(mvchip->blink_en_reg, mvebu_gpioreg_blink(mvchip));
	writel(mvchip->in_pol_reg, mvebu_gpioreg_in_pol(mvchip));

	switch (mvchip->soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
		writel(mvchip->edge_mask_regs[0],
		       mvchip->membase + GPIO_EDGE_MASK_OFF);
		writel(mvchip->level_mask_regs[0],
		       mvchip->membase + GPIO_LEVEL_MASK_OFF);
		break;
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		for (i = 0; i < 2; i++) {
			writel(mvchip->edge_mask_regs[i],
			       mvchip->membase + GPIO_EDGE_MASK_MV78200_OFF(i));
			writel(mvchip->level_mask_regs[i],
			       mvchip->membase +
			       GPIO_LEVEL_MASK_MV78200_OFF(i));
		}
		break;
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		for (i = 0; i < 4; i++) {
			writel(mvchip->edge_mask_regs[i],
			       mvchip->membase +
			       GPIO_EDGE_MASK_ARMADAXP_OFF(i));
			writel(mvchip->level_mask_regs[i],
			       mvchip->membase +
			       GPIO_LEVEL_MASK_ARMADAXP_OFF(i));
		}
		break;
	default:
		BUG();
	}

	if (IS_ENABLED(CONFIG_PWM))
		mvebu_pwm_resume(mvchip);

	return 0;
}

static int mvebu_gpio_probe(struct platform_device *pdev)
{
	struct mvebu_gpio_chip *mvchip;
	const struct of_device_id *match;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	struct irq_chip_generic *gc;
	struct irq_chip_type *ct;
	unsigned int ngpios;
	unsigned int gpio_base = -1;
	int soc_variant;
	int i, cpu, id;
	int err;

	match = of_match_device(mvebu_gpio_of_match, &pdev->dev);
	if (match)
		soc_variant = (uintptr_t) match->data;
	else
		soc_variant = MVEBU_GPIO_SOC_VARIANT_ORION;

	mvchip = devm_kzalloc(&pdev->dev, sizeof(struct mvebu_gpio_chip),
			      GFP_KERNEL);
	if (!mvchip)
		return -ENOMEM;

	platform_set_drvdata(pdev, mvchip);

	if (of_property_read_u32(pdev->dev.of_node, "ngpios", &ngpios)) {
		dev_err(&pdev->dev, "Missing ngpios OF property\n");
		return -ENODEV;
	}

	if (of_property_read_u32(pdev->dev.of_node, "gpiobase", &gpio_base))
		gpio_base = -1;

	id = of_alias_get_id(pdev->dev.of_node, "gpio");
	if (id < 0) {
		dev_err(&pdev->dev, "Couldn't get OF id\n");
		return id;
	}

	mvchip->clk = devm_clk_get(&pdev->dev, NULL);
	/* Not all SoCs require a clock.*/
	if (!IS_ERR(mvchip->clk))
		clk_prepare_enable(mvchip->clk);

	mvchip->soc_variant = soc_variant;
	mvchip->chip.label = dev_name(&pdev->dev);
	mvchip->chip.parent = &pdev->dev;
	mvchip->chip.request = gpiochip_generic_request;
	mvchip->chip.free = gpiochip_generic_free;
	mvchip->chip.direction_input = mvebu_gpio_direction_input;
	mvchip->chip.get = mvebu_gpio_get;
	mvchip->chip.direction_output = mvebu_gpio_direction_output;
	mvchip->chip.set = mvebu_gpio_set;
	if (gpio_base != -1)
		mvchip->chip.base = gpio_base;
	else
		mvchip->chip.base = id * MVEBU_MAX_GPIO_PER_BANK;
	mvchip->chip.dbg_show = mvebu_gpio_dbg_show;
	mvchip->chip.to_irq = mvebu_gpio_to_irq;
	mvchip->chip.ngpio = ngpios;
	mvchip->chip.can_sleep = false;
	mvchip->chip.of_node = np;

	spin_lock_init(&mvchip->lock);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	mvchip->membase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(mvchip->membase))
		return PTR_ERR(mvchip->membase);

	/* The Armada XP has a second range of registers for the
	 * per-CPU registers */
	if (soc_variant == MVEBU_GPIO_SOC_VARIANT_ARMADAXP) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		mvchip->percpu_membase = devm_ioremap_resource(&pdev->dev,
							       res);
		if (IS_ERR(mvchip->percpu_membase))
			return PTR_ERR(mvchip->percpu_membase);
	}

	/*
	 * Mask and clear GPIO interrupts.
	 */
	switch (soc_variant) {
	case MVEBU_GPIO_SOC_VARIANT_ORION:
		writel_relaxed(0, mvchip->membase + GPIO_EDGE_CAUSE_OFF);
		writel_relaxed(0, mvchip->membase + GPIO_EDGE_MASK_OFF);
		writel_relaxed(0, mvchip->membase + GPIO_LEVEL_MASK_OFF);
		break;
	case MVEBU_GPIO_SOC_VARIANT_MV78200:
		writel_relaxed(0, mvchip->membase + GPIO_EDGE_CAUSE_OFF);
		for (cpu = 0; cpu < 2; cpu++) {
			writel_relaxed(0, mvchip->membase +
				       GPIO_EDGE_MASK_MV78200_OFF(cpu));
			writel_relaxed(0, mvchip->membase +
				       GPIO_LEVEL_MASK_MV78200_OFF(cpu));
		}
		break;
	case MVEBU_GPIO_SOC_VARIANT_ARMADAXP:
		writel_relaxed(0, mvchip->membase + GPIO_EDGE_CAUSE_OFF);
		writel_relaxed(0, mvchip->membase + GPIO_EDGE_MASK_OFF);
		writel_relaxed(0, mvchip->membase + GPIO_LEVEL_MASK_OFF);
		for (cpu = 0; cpu < 4; cpu++) {
			writel_relaxed(0, mvchip->percpu_membase +
				       GPIO_EDGE_CAUSE_ARMADAXP_OFF(cpu));
			writel_relaxed(0, mvchip->percpu_membase +
				       GPIO_EDGE_MASK_ARMADAXP_OFF(cpu));
			writel_relaxed(0, mvchip->percpu_membase +
				       GPIO_LEVEL_MASK_ARMADAXP_OFF(cpu));
		}
		break;
	default:
		BUG();
	}

	gpiochip_add(&mvchip->chip);

	/* Some gpio controllers do not provide irq support */
	if (!of_irq_count(np))
		return 0;

	/* Setup the interrupt handlers. Each chip can have up to 4
	 * interrupt handlers, with each handler dealing with 8 GPIO
	 * pins. */
	for (i = 0; i < 4; i++) {
		int irq = platform_get_irq(pdev, i);

		if (irq < 0)
			continue;
		irq_set_chained_handler_and_data(irq, mvebu_gpio_irq_handler,
						 mvchip);
	}

	mvchip->irqbase = irq_alloc_descs(-1, 0, ngpios, -1);
	if (mvchip->irqbase < 0) {
		dev_err(&pdev->dev, "no irqs\n");
		err = mvchip->irqbase;
		goto err_gpiochip_add;
	}

	gc = irq_alloc_generic_chip("mvebu_gpio_irq", 2, mvchip->irqbase,
				    mvchip->membase, handle_level_irq);
	if (!gc) {
		dev_err(&pdev->dev, "Cannot allocate generic irq_chip\n");
		err = -ENOMEM;
		goto err_gpiochip_add;
	}

	gc->private = mvchip;
	ct = &gc->chip_types[0];
	ct->type = IRQ_TYPE_LEVEL_HIGH | IRQ_TYPE_LEVEL_LOW;
	ct->chip.irq_mask = mvebu_gpio_level_irq_mask;
	ct->chip.irq_unmask = mvebu_gpio_level_irq_unmask;
	ct->chip.irq_set_type = mvebu_gpio_irq_set_type;
	ct->chip.name = mvchip->chip.label;

	ct = &gc->chip_types[1];
	ct->type = IRQ_TYPE_EDGE_RISING | IRQ_TYPE_EDGE_FALLING;
	ct->chip.irq_ack = mvebu_gpio_irq_ack;
	ct->chip.irq_mask = mvebu_gpio_edge_irq_mask;
	ct->chip.irq_unmask = mvebu_gpio_edge_irq_unmask;
	ct->chip.irq_set_type = mvebu_gpio_irq_set_type;
	ct->handler = handle_edge_irq;
	ct->chip.name = mvchip->chip.label;

	irq_setup_generic_chip(gc, IRQ_MSK(ngpios), 0,
			       IRQ_NOREQUEST, IRQ_LEVEL | IRQ_NOPROBE);

	/* Setup irq domain on top of the generic chip. */
	mvchip->domain = irq_domain_add_simple(np, mvchip->chip.ngpio,
					       mvchip->irqbase,
					       &irq_domain_simple_ops,
					       mvchip);
	if (!mvchip->domain) {
		dev_err(&pdev->dev, "couldn't allocate irq domain %s (DT).\n",
			mvchip->chip.label);
		err = -ENODEV;
		goto err_generic_chip;
	}

	/* Armada 370/XP has simple PWM support for GPIO lines */
	if (IS_ENABLED(CONFIG_PWM))
		return mvebu_pwm_probe(pdev, mvchip, id);

	return 0;

err_generic_chip:
	irq_remove_generic_chip(gc, IRQ_MSK(ngpios), IRQ_NOREQUEST,
				IRQ_LEVEL | IRQ_NOPROBE);
	kfree(gc);

err_gpiochip_add:
	gpiochip_remove(&mvchip->chip);

	return err;
}

static struct platform_driver mvebu_gpio_driver = {
	.driver		= {
		.name		= "mvebu-gpio",
		.of_match_table = mvebu_gpio_of_match,
	},
	.probe		= mvebu_gpio_probe,
	.suspend        = mvebu_gpio_suspend,
	.resume         = mvebu_gpio_resume,
};
module_platform_driver(mvebu_gpio_driver);
