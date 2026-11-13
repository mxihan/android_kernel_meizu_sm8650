// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2017 Sean Young <sean@mess.org>
 * Clock/pinctrl transmitter support for Meizu 21 Pro.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <media/rc-core.h>

#define DRIVER_NAME "pwm-ir-tx"
#define DEVICE_NAME "PWM IR Transmitter"
#define PWM_IR_BUSY_WAIT_US 1000

struct pwm_ir {
	struct pwm_device *pwm;
	struct clk *clk;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active;
	struct pinctrl_state *sleep;
	struct kthread_worker *worker;
	unsigned int carrier;
	unsigned int duty_cycle;
};

struct pwm_ir_tx_work {
	struct kthread_work work;
	struct pwm_ir *pwm_ir;
	unsigned int *txbuf;
	unsigned int count;
	int result;
};

static const struct of_device_id pwm_ir_of_match[] = {
	{ .compatible = "pwm-ir-tx", },
	{ },
};
MODULE_DEVICE_TABLE(of, pwm_ir_of_match);

static int pwm_ir_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
	struct pwm_ir *pwm_ir = dev->priv;

	if (!duty_cycle || duty_cycle >= 100)
		return -EINVAL;

	pwm_ir->duty_cycle = duty_cycle;
	return 0;
}

static int pwm_ir_set_carrier(struct rc_dev *dev, u32 carrier)
{
	struct pwm_ir *pwm_ir = dev->priv;

	if (!carrier)
		return -EINVAL;

	if (pwm_ir->clk) {
		switch (carrier) {
		case 36000:
		case 38000:
		case 39500:
		case 40000:
		case 56000:
			break;
		default:
			return -EINVAL;
		}
	}

	pwm_ir->carrier = carrier;
	return 0;
}

static void pwm_ir_wait_until(ktime_t edge)
{
	long delta = ktime_us_delta(edge, ktime_get());

	/* Leave a busy-wait tail to avoid hrtimer wakeup jitter at IR edges. */
	if (delta > PWM_IR_BUSY_WAIT_US)
		usleep_range(delta - PWM_IR_BUSY_WAIT_US,
			     delta - PWM_IR_BUSY_WAIT_US + 10);
	while (ktime_before(ktime_get(), edge))
		cpu_relax();
}

static int pwm_ir_tx_clock(struct pwm_ir *pwm_ir, unsigned int *txbuf,
			   unsigned int count)
{
	ktime_t edge;
	long rounded;
	unsigned int i;
	int rc, sleep_rc;

	rc = pinctrl_select_state(pwm_ir->pinctrl, pwm_ir->sleep);
	if (rc)
		return rc;

	rounded = clk_round_rate(pwm_ir->clk, pwm_ir->carrier);
	if (rounded < 0)
		return rounded;
	/* Refuse an unpatched GCC that rounds a kHz carrier up to MHz. */
	if (abs(rounded - (long)pwm_ir->carrier) > pwm_ir->carrier / 100)
		return -ERANGE;

	rc = clk_set_rate(pwm_ir->clk, pwm_ir->carrier);
	if (rc)
		return rc;

	/* Safe-config RCGs program their M/N divider only when enabled.
	 * Keep the output pin low until both the rate and duty are ready.
	 */
	rc = clk_prepare_enable(pwm_ir->clk);
	if (rc)
		return rc;
	rc = clk_set_duty_cycle(pwm_ir->clk, pwm_ir->duty_cycle, 100);
	if (rc)
		goto disable;

	edge = ktime_get();
	for (i = 0; i < count; i++) {
		rc = pinctrl_select_state(pwm_ir->pinctrl,
					  i % 2 ? pwm_ir->sleep : pwm_ir->active);
		if (rc)
			goto disable;
		edge = ktime_add_us(edge, txbuf[i]);
		pwm_ir_wait_until(edge);
	}
	rc = count;

disable:
	sleep_rc = pinctrl_select_state(pwm_ir->pinctrl, pwm_ir->sleep);
	clk_disable_unprepare(pwm_ir->clk);
	if (rc >= 0 && sleep_rc)
		return sleep_rc;
	return rc;
}

static void pwm_ir_tx_worker(struct kthread_work *work)
{
	struct pwm_ir_tx_work *tx = container_of(work, struct pwm_ir_tx_work, work);

	tx->result = pwm_ir_tx_clock(tx->pwm_ir, tx->txbuf, tx->count);
}

static void pwm_ir_destroy_worker(void *worker)
{
	kthread_destroy_worker(worker);
}

static int pwm_ir_tx(struct rc_dev *dev, unsigned int *txbuf,
		     unsigned int count)
{
	struct pwm_ir *pwm_ir = dev->priv;
	struct pwm_device *pwm = pwm_ir->pwm;
	struct pwm_state state;
	ktime_t edge;
	long delta;
	unsigned int i;

	if (pwm_ir->clk) {
		struct pwm_ir_tx_work tx = {
			.pwm_ir = pwm_ir,
			.txbuf = txbuf,
			.count = count,
		};

		kthread_init_work(&tx.work, pwm_ir_tx_worker);
		kthread_queue_work(pwm_ir->worker, &tx.work);
		kthread_flush_work(&tx.work);
		return tx.result;
	}

	pwm_init_state(pwm, &state);
	state.period = DIV_ROUND_CLOSEST(NSEC_PER_SEC, pwm_ir->carrier);
	pwm_set_relative_duty_cycle(&state, pwm_ir->duty_cycle, 100);
	edge = ktime_get();
	for (i = 0; i < count; i++) {
		state.enabled = !(i % 2);
		pwm_apply_state(pwm, &state);
		edge = ktime_add_us(edge, txbuf[i]);
		delta = ktime_us_delta(edge, ktime_get());
		if (delta > 0)
			usleep_range(delta, delta + 10);
	}
	state.enabled = false;
	pwm_apply_state(pwm, &state);
	return count;
}

static int pwm_ir_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_ir *pwm_ir;
	struct rc_dev *rcdev;
	int rc;

	pwm_ir = devm_kzalloc(dev, sizeof(*pwm_ir), GFP_KERNEL);
	if (!pwm_ir)
		return -ENOMEM;

	if (of_find_property(dev->of_node, "pwms", NULL)) {
		pwm_ir->pwm = devm_pwm_get(dev, NULL);
		if (IS_ERR(pwm_ir->pwm))
			return dev_err_probe(dev, PTR_ERR(pwm_ir->pwm),
					     "failed to get PWM\n");
	} else {
		pwm_ir->clk = devm_clk_get(dev, "gpio-pwm-clk");
		if (IS_ERR(pwm_ir->clk))
			return dev_err_probe(dev, PTR_ERR(pwm_ir->clk),
					     "failed to get IR carrier clock\n");
		pwm_ir->pinctrl = devm_pinctrl_get(dev);
		if (IS_ERR(pwm_ir->pinctrl))
			return dev_err_probe(dev, PTR_ERR(pwm_ir->pinctrl),
					     "failed to get pinctrl\n");
		pwm_ir->active = pinctrl_lookup_state(pwm_ir->pinctrl, "ir_active");
		if (IS_ERR(pwm_ir->active))
			return dev_err_probe(dev, PTR_ERR(pwm_ir->active),
					     "failed to get active pins\n");
		pwm_ir->sleep = pinctrl_lookup_state(pwm_ir->pinctrl, "ir_sleep");
		if (IS_ERR(pwm_ir->sleep))
			return dev_err_probe(dev, PTR_ERR(pwm_ir->sleep),
					     "failed to get sleep pins\n");
		rc = pinctrl_select_state(pwm_ir->pinctrl, pwm_ir->sleep);
		if (rc)
			return dev_err_probe(dev, rc, "failed to idle IR output\n");

		/* Keep envelope timing independent of Binder caller scheduling. */
		pwm_ir->worker = kthread_create_worker(0, DRIVER_NAME);
		if (IS_ERR(pwm_ir->worker))
			return PTR_ERR(pwm_ir->worker);
		sched_set_fifo_low(pwm_ir->worker->task);
		rc = devm_add_action_or_reset(dev, pwm_ir_destroy_worker, pwm_ir->worker);
		if (rc)
			return rc;
	}

	pwm_ir->carrier = 38000;
	pwm_ir->duty_cycle = 50;
	rcdev = devm_rc_allocate_device(dev, RC_DRIVER_IR_RAW_TX);
	if (!rcdev)
		return -ENOMEM;
	rcdev->priv = pwm_ir;
	rcdev->driver_name = DRIVER_NAME;
	rcdev->device_name = DEVICE_NAME;
	rcdev->tx_ir = pwm_ir_tx;
	rcdev->s_tx_duty_cycle = pwm_ir_set_duty_cycle;
	rcdev->s_tx_carrier = pwm_ir_set_carrier;
	return devm_rc_register_device(dev, rcdev);
}

static struct platform_driver pwm_ir_driver = {
	.probe = pwm_ir_probe,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_match_ptr(pwm_ir_of_match),
	},
};
module_platform_driver(pwm_ir_driver);

MODULE_DESCRIPTION("PWM and clock/pinctrl IR Transmitter");
MODULE_AUTHOR("Sean Young <sean@mess.org>");
MODULE_LICENSE("GPL");
