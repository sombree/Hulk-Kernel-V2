/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/errno.h>

#include <linux/clk.h>
#include <mach/clk-provider.h>
#include <mach/clock-generic.h>

/* ==================== Mux clock ==================== */

static int parent_to_src_sel(struct mux_clk *mux, struct clk *p)
{
	int i;

	for (i = 0; i < mux->num_parents; i++) {
		if (mux->parents[i].src == p)
			return mux->parents[i].sel;
	}

	return -EINVAL;
}

static int mux_set_parent(struct clk *c, struct clk *p)
{
	struct mux_clk *mux = to_mux_clk(c);
	int sel = parent_to_src_sel(mux, p);
	struct clk *old_parent;
	int rc = 0;
	unsigned long flags;

	if (sel < 0)
		return sel;

	rc = __clk_pre_reparent(c, p, &flags);
	if (rc)
		goto out;

	rc = mux->ops->set_mux_sel(mux, sel);
	if (rc)
		goto set_fail;

	old_parent = c->parent;
	c->parent = p;
	__clk_post_reparent(c, old_parent, &flags);

	return 0;

set_fail:
	__clk_post_reparent(c, p, &flags);
out:
	return rc;
}

static long mux_round_rate(struct clk *c, unsigned long rate)
{
	struct mux_clk *mux = to_mux_clk(c);
	int i;
	long prate, max_prate = 0, rrate = LONG_MAX;

	for (i = 0; i < mux->num_parents; i++) {
		prate = clk_round_rate(mux->parents[i].src, rate);
		if (prate < rate) {
			max_prate = max(prate, max_prate);
			continue;
		}

		rrate = min(rrate, prate);
	}
	if (rrate == LONG_MAX)
		rrate = max_prate;

	return rrate ? rrate : -EINVAL;
}

static int mux_set_rate(struct clk *c, unsigned long rate)
{
	struct mux_clk *mux = to_mux_clk(c);
	struct clk *new_parent = NULL;
	int rc = 0, i;
	unsigned long new_par_curr_rate;

	for (i = 0; i < mux->num_parents; i++) {
		if (clk_round_rate(mux->parents[i].src, rate) == rate) {
			new_parent = mux->parents[i].src;
			break;
		}
	}
	if (new_parent == NULL)
		return -EINVAL;

	/*
	 * Switch to safe parent since the old and new parent might be the
	 * same and the parent might temporarily turn off while switching
	 * rates.
	 */
	if (mux->safe_sel >= 0)
		rc = mux->ops->set_mux_sel(mux, mux->safe_sel);
	if (rc)
		return rc;

	new_par_curr_rate = clk_get_rate(new_parent);
	rc = clk_set_rate(new_parent, rate);
	if (rc)
		goto set_rate_fail;

	rc = mux_set_parent(c, new_parent);
	if (rc)
		goto set_par_fail;

	return 0;

set_par_fail:
	clk_set_rate(new_parent, new_par_curr_rate);
set_rate_fail:
	WARN(mux->ops->set_mux_sel(mux, parent_to_src_sel(mux, c->parent)),
		"Set rate failed for %s. Also in bad state!\n", c->dbg_name);
	return rc;
}

static int mux_enable(struct clk *c)
{
	struct mux_clk *mux = to_mux_clk(c);
	if (mux->ops->enable)
		return mux->ops->enable(mux);
	return 0;
}

static void mux_disable(struct clk *c)
{
	struct mux_clk *mux = to_mux_clk(c);
	if (mux->ops->disable)
		return mux->ops->disable(mux);
}

static struct clk *mux_get_parent(struct clk *c)
{
	struct mux_clk *mux = to_mux_clk(c);
	int sel = mux->ops->get_mux_sel(mux);
	int i;

	for (i = 0; i < mux->num_parents; i++) {
		if (mux->parents[i].sel == sel)
			return mux->parents[i].src;
	}

	/* Unfamiliar parent. */
	return NULL;
}

static enum handoff mux_handoff(struct clk *c)
{
	struct mux_clk *mux = to_mux_clk(c);

	c->rate = clk_get_rate(c->parent);
	mux->safe_sel = parent_to_src_sel(mux, mux->safe_parent);

	if (mux->en_mask && mux->ops && mux->ops->is_enabled)
		return mux->ops->is_enabled(mux)
			? HANDOFF_ENABLED_CLK
			: HANDOFF_DISABLED_CLK;

	/*
	 * If this function returns 'enabled' even when the clock downstream
	 * of this clock is disabled, then handoff code will unnecessarily
	 * enable the current parent of this clock. If this function always
	 * returns 'disabled' and a clock downstream is on, the clock handoff
	 * code will bump up the ref count for this clock and its current
	 * parent as necessary. So, clocks without an actual HW gate can
	 * always return disabled.
	 */
	return HANDOFF_DISABLED_CLK;
}

struct clk_ops clk_ops_gen_mux = {
	.enable = mux_enable,
	.disable = mux_disable,
	.set_parent = mux_set_parent,
	.round_rate = mux_round_rate,
	.set_rate = mux_set_rate,
	.handoff = mux_handoff,
	.get_parent = mux_get_parent,
};
