// SPDX-License-Identifier: GPL-2.0
/*
 * rtl8822ce-regd-shim - register the US regulatory domain on the
 * self-managed wiphy of NVIDIA's rtl8822ce vendor driver.
 *
 * Root cause: rtl8822ce v5.14.0.4-261 is built with
 * RTW_DEF_MODULE_REGULATORY_CERT=0x00, so its country_chplan_map
 * compiles down to only "00" -> 0x7F. Proc writes to chan_plan and
 * country_code return rc=0 but every non-0x7F plan is rejected by
 * table validation, and the self-managed wiphy never receives a
 * regulatory_set_wiphy_regd() call. phy#0 is therefore stuck on
 * country 00 / DFS-UNSET with a crippled rule table (5 GHz 6 dBm,
 * NO-IR), which is what blocks 5 GHz AP/client qualification.
 *
 * This shim performs exactly the call the vendor driver never makes:
 * it registers the US (DFS-FCC) regulatory domain on the wiphy of the
 * given netdev. Non-persistent by design: nothing is installed under
 * /etc, no depmod, and module exit leaves the applied domain as-is.
 *
 * API contract followed (include/net/cfg80211.h, 6.8.12-1021-tegra):
 *  - regulatory_set_wiphy_regd_sync() requires RTNL *and* the wiphy
 *    mutex held and applies the domain synchronously.
 *  - Its @rd parameter is a non-const "struct ieee80211_regdomain *".
 *    cfg80211 copies it (reg_copy_regd()) and stores the copy as
 *    wiphy->regd; the caller keeps ownership of the original and must
 *    free it after the call returns (success or failure).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <net/net_namespace.h>
#include <net/regulatory.h>

static char *ifname = "wlP1p1s0";
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname,
		 "netdev name of the wireless interface whose wiphy gets the US regdomain");

/*
 * US / DFS-FCC rule table, captured verbatim from the live global US
 * domain on this device (iw reg get). Units for REG_RULE() are
 * MHz / dBi / dBm; dfs_cac 0 means "default" and matches the "(0 ms)"
 * shown by iw for the two DFS rules.
 */
static const struct ieee80211_reg_rule us_rules[] = {
	REG_RULE(902, 904, 2, 0, 30, 0),
	REG_RULE(904, 920, 16, 0, 30, 0),
	REG_RULE(920, 928, 8, 0, 30, 0),
	REG_RULE(2400, 2472, 40, 0, 30, 0),
	REG_RULE(5150, 5250, 80, 0, 23, NL80211_RRF_AUTO_BW),
	REG_RULE(5250, 5350, 80, 0, 24, NL80211_RRF_DFS | NL80211_RRF_AUTO_BW),
	REG_RULE(5470, 5730, 160, 0, 24, NL80211_RRF_DFS),
	REG_RULE(5730, 5850, 80, 0, 30, NL80211_RRF_AUTO_BW),
	REG_RULE(5850, 5895, 40, 0, 27,
		 NL80211_RRF_NO_OUTDOOR | NL80211_RRF_AUTO_BW |
		 NL80211_RRF_PASSIVE_SCAN),
	REG_RULE(5925, 7125, 320, 0, 12,
		 NL80211_RRF_NO_OUTDOOR | NL80211_RRF_PASSIVE_SCAN),
	REG_RULE(57240, 71000, 2160, 0, 40, 0),
};

static struct ieee80211_regdomain *alloc_us_regd(void)
{
	struct ieee80211_regdomain *rd;

	rd = kzalloc(struct_size(rd, reg_rules, ARRAY_SIZE(us_rules)),
		     GFP_KERNEL);
	if (!rd)
		return NULL;

	rd->alpha2[0] = 'U';
	rd->alpha2[1] = 'S';
	rd->alpha2[2] = '\0';
	rd->dfs_region = NL80211_DFS_FCC;
	rd->n_reg_rules = ARRAY_SIZE(us_rules);
	memcpy(rd->reg_rules, us_rules, sizeof(us_rules));

	return rd;
}

static int __init rtl8822ce_regd_shim_init(void)
{
	struct ieee80211_regdomain *rd;
	struct net_device *dev;
	struct wiphy *wiphy;
	int ret;

	dev = dev_get_by_name(&init_net, ifname);
	if (!dev) {
		pr_err("netdev %s not found\n", ifname);
		return -ENODEV;
	}

	if (!dev->ieee80211_ptr || !dev->ieee80211_ptr->wiphy) {
		pr_err("%s has no wiphy\n", ifname);
		ret = -ENODEV;
		goto out_dev_put;
	}
	wiphy = dev->ieee80211_ptr->wiphy;

	/*
	 * Only self-managed wiphys may use the setter; check up front so
	 * a wrong netdev fails the insmod loudly instead of tripping the
	 * WARN inside cfg80211.
	 */
	if (!(wiphy->regulatory_flags & REGULATORY_WIPHY_SELF_MANAGED)) {
		pr_err("%s wiphy is not self-managed\n", ifname);
		ret = -EINVAL;
		goto out_dev_put;
	}

	rd = alloc_us_regd();
	if (!rd) {
		pr_err("cannot allocate regdomain\n");
		ret = -EINVAL;
		goto out_dev_put;
	}

	rtnl_lock();
	wiphy_lock(wiphy);
	ret = regulatory_set_wiphy_regd_sync(wiphy, rd);
	wiphy_unlock(wiphy);
	rtnl_unlock();

	/* cfg80211 stored its own copy; rd stays ours to free. */
	kfree(rd);

	if (ret) {
		pr_err("regulatory_set_wiphy_regd_sync failed: %d\n", ret);
		ret = -EINVAL;
		goto out_dev_put;
	}

	pr_info("US regdomain registered on %s (wiphy %s)\n",
		ifname, wiphy_name(wiphy));
	ret = 0;

out_dev_put:
	dev_put(dev);
	return ret;
}

static void __exit rtl8822ce_regd_shim_exit(void)
{
	/*
	 * Intentionally empty: the applied US domain stays in place so
	 * the owner can inspect it after rmmod. Restoring the previous
	 * domain is not trivially possible - cfg80211 keeps the previous
	 * wiphy->regd private and a self-managed wiphy has no restore
	 * entry point short of re-registering the wiphy.
	 */
}

module_init(rtl8822ce_regd_shim_init);
module_exit(rtl8822ce_regd_shim_exit);

MODULE_DESCRIPTION("Register the US regulatory domain on the rtl8822ce self-managed wiphy");
MODULE_LICENSE("GPL");
