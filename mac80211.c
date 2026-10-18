/*
 * Copyright (C) 2006-2018, Marvell International Ltd.
 *
 * This software file (the "File") is distributed by Marvell International
 * Ltd. under the terms of the GNU General Public License Version 2, June 1991
 * (the "License").  You may use, redistribute and/or modify this File in
 * accordance with the terms and conditions of the License, a copy of which
 * is available by writing to the Free Software Foundation, Inc.
 *
 * THE FILE IS DISTRIBUTED AS-IS, WITHOUT WARRANTY OF ANY KIND, AND THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE
 * ARE EXPRESSLY DISCLAIMED.  The License provides additional details about
 * this warranty disclaimer.
 */

/* Description:  This file implements mac80211 related functions. */

#include <linux/etherdevice.h>
#include <linux/math64.h>

#include "sysadpt.h"
#include "core.h"
#include "utils.h"
#include "hif/fwcmd.h"
#include "hif/hif-ops.h"

#define MAX_AMPDU_ATTEMPTS  5

static const struct ieee80211_rate mwl_rates_24[] = {
	{ .bitrate = 10, .hw_value = 2, },
	{ .bitrate = 20, .hw_value = 4, },
	{ .bitrate = 55, .hw_value = 11, },
	{ .bitrate = 110, .hw_value = 22, },
	{ .bitrate = 220, .hw_value = 44, },
	{ .bitrate = 60, .hw_value = 12, },
	{ .bitrate = 90, .hw_value = 18, },
	{ .bitrate = 120, .hw_value = 24, },
	{ .bitrate = 180, .hw_value = 36, },
	{ .bitrate = 240, .hw_value = 48, },
	{ .bitrate = 360, .hw_value = 72, },
	{ .bitrate = 480, .hw_value = 96, },
	{ .bitrate = 540, .hw_value = 108, },
};

static const struct ieee80211_rate mwl_rates_50[] = {
	{ .bitrate = 60, .hw_value = 12, },
	{ .bitrate = 90, .hw_value = 18, },
	{ .bitrate = 120, .hw_value = 24, },
	{ .bitrate = 180, .hw_value = 36, },
	{ .bitrate = 240, .hw_value = 48, },
	{ .bitrate = 360, .hw_value = 72, },
	{ .bitrate = 480, .hw_value = 96, },
	{ .bitrate = 540, .hw_value = 108, },
};

static void mwl_get_rateinfo(struct mwl_priv *priv, u8 *addr,
			     struct mwl_sta *sta_info)
{
	int table_size = (sizeof(__le32) * 2 * SYSADPT_MAX_RATE_ADAPT_RATES);
	u8 *rate_table, *rate_idx;
	u32 rate_info;
	struct mwl_tx_hist_data *tx_hist_data;
	int ret, idx;

	rate_table = kzalloc(table_size, GFP_KERNEL);
	if (!rate_table)
		return;

	ret = mwl_fwcmd_get_ratetable(priv->hw, addr, rate_table,
				      table_size, 0);
	if (ret) {
		kfree(rate_table);
		return;
	}

	idx = 0;
	rate_idx = rate_table;
	rate_info = le32_to_cpu(*(__le32 *)rate_idx);
	tx_hist_data = &sta_info->tx_hist.su_rate[0];
	while (rate_info) {
		if (idx < SYSADPT_MAX_RATE_ADAPT_RATES)
			tx_hist_data[idx].rateinfo = rate_info;
		idx++;
		rate_idx += (2 * sizeof(__le32));
		rate_info = le32_to_cpu(*(__le32 *)rate_idx);
	}

	kfree(rate_table);
}

static void mwl_mac80211_tx(struct ieee80211_hw *hw,
			    struct ieee80211_tx_control *control,
			    struct sk_buff *skb)
{
	struct mwl_priv *priv = hw->priv;

	if (!priv->radio_on) {
		wiphy_warn(hw->wiphy,
			   "dropped TX frame since radio is disabled\n");
		dev_kfree_skb_any(skb);
		return;
	}

	mwl_hif_tx_xmit(hw, control, skb);
}

static void mwl_csa_reset(struct ieee80211_hw *hw);
static void mwl_csa_fail(struct ieee80211_hw *hw, const char *reason);

static int mwl_mac80211_start(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;
	int rc;

	if (priv->csa_faulted)
		return -EIO;

	/* Enable TX and RX tasklets. */
	mwl_hif_enable_data_tasks(hw);

	/* Enable interrupts */
	mwl_hif_irq_enable(hw);

	rc = mwl_fwcmd_radio_enable(hw);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_rate_adapt_mode(hw, priv->rate_adapt_mode);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_wmm_mode(hw, true);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_ht_guard_interval(hw, GUARD_INTERVAL_AUTO);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_dwds_stamode(hw, priv->dwds_stamode);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_fw_flush_timer(hw, SYSADPT_AMSDU_FLUSH_TIME);
	if (rc)
		goto fwcmd_fail;
	rc = mwl_fwcmd_set_optimization_level(hw, priv->optimization_level);
	if (rc)
		goto fwcmd_fail;
	if (priv->chip_type == MWL8997) {
		rc = mwl_fwcmd_config_EDMACCtrl(hw, 0);
		if (rc)
			goto fwcmd_fail;
	}
	if (priv->chip_type == MWL8964) {
		rc = mwl_fwcmd_newdp_dmathread_start(hw);
		if (rc)
			goto fwcmd_fail;
		rc = mwl_fwcmd_set_bftype(hw, priv->bf_type);
		if (rc)
			goto fwcmd_fail;
	}

	ieee80211_wake_queues(hw);
	return 0;

fwcmd_fail:
	mwl_hif_irq_disable(hw);
	mwl_hif_disable_data_tasks(hw);

	return rc;
}

static void mwl_mac80211_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct mwl_priv *priv = hw->priv;

	mwl_csa_reset(hw);
	mwl_fwcmd_radio_disable(hw);

	ieee80211_stop_queues(hw);

	/* Disable interrupts */
	mwl_hif_irq_disable(hw);

	/* Disable TX and RX tasklets. */
	mwl_hif_disable_data_tasks(hw);

	/* Return all skbs to mac80211 */
	mwl_hif_tx_return_pkts(hw);
	priv->chanctx = NULL;
	priv->radio_conf_valid = false;
	priv->dfs_monitoring = false;
}

static int mwl_mac80211_add_interface(struct ieee80211_hw *hw,
				      struct ieee80211_vif *vif)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif;
	u32 macids_supported;
	int macid;

	if (vif->type == NL80211_IFTYPE_MONITOR)
		priv->rx_decrypt = true;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		if (vif->type == NL80211_IFTYPE_MESH_POINT)
			if (priv->chip_type != MWL8997)
				return -EPERM;
		macids_supported = priv->ap_macids_supported;
		break;
	case NL80211_IFTYPE_STATION:
		macids_supported = priv->sta_macids_supported;
		break;
	default:
		return -EINVAL;
	}

	macid = ffs(macids_supported & ~priv->macids_used);

	if (!macid) {
		wiphy_warn(hw->wiphy, "no macid can be allocated\n");
		return -EBUSY;
	}
	macid--;

	/* Setup driver private area. */
	mwl_vif = mwl_dev_get_vif(vif);
	memset(mwl_vif, 0, sizeof(*mwl_vif));
	mwl_vif->type = vif->type;
	mwl_vif->macid = macid;
	mwl_vif->seqno = 0;
	mwl_vif->is_hw_crypto_enabled = false;
	mwl_vif->beacon_info.valid = false;
	mwl_vif->set_beacon = false;
	mwl_vif->basic_rate_idx = 0;
	mwl_vif->broadcast_ssid = 0xFF;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
		ether_addr_copy(mwl_vif->bssid, vif->addr);
		mwl_fwcmd_set_new_stn_add_self(hw, vif);
		if (priv->chip_type == MWL8964) {
			/* allow firmware to really set channel */
			mwl_fwcmd_bss_start(hw, vif, true);
			mwl_fwcmd_bss_start(hw, vif, false);
		}
		break;
	case NL80211_IFTYPE_MESH_POINT:
		ether_addr_copy(mwl_vif->bssid, vif->addr);
		mwl_fwcmd_set_new_stn_add_self(hw, vif);
		break;
	case NL80211_IFTYPE_STATION:
		ether_addr_copy(mwl_vif->sta_mac, vif->addr);
		mwl_fwcmd_bss_start(hw, vif, true);
		mwl_fwcmd_set_infra_mode(hw, vif);
		mwl_fwcmd_set_mac_addr_client(hw, vif, vif->addr);
		break;
	default:
		return -EINVAL;
	}

	priv->macids_used |= 1 << mwl_vif->macid;
	spin_lock_bh(&priv->vif_lock);
	list_add_tail(&mwl_vif->list, &priv->vif_list);
	spin_unlock_bh(&priv->vif_lock);

	return 0;
}

static void mwl_mac80211_remove_vif(struct mwl_priv *priv,
				    struct ieee80211_vif *vif)
{
	struct mwl_vif *mwl_vif = mwl_dev_get_vif(vif);

	if (!priv->macids_used)
		return;

	/* mac80211 suppress 1 monitor interface */
	if (vif->type == NL80211_IFTYPE_MONITOR) {
		int counter = 0;
		struct ieee80211_vif *_vif;
		spin_lock_bh(&priv->vif_lock);
		list_for_each_entry(mwl_vif, &priv->vif_list, list) {
			_vif = container_of((void *)mwl_vif, struct ieee80211_vif,
					drv_priv);

			if (_vif->type == NL80211_IFTYPE_MONITOR)
				counter++;
		}
		spin_unlock_bh(&priv->vif_lock);
		/* but if existe more than 1,
		   so 1 interface is still activ*/
		if (counter <= 1)
			priv->rx_decrypt = false;
	}

	mwl_hif_tx_del_pkts_via_vif(priv->hw, vif);

	priv->macids_used &= ~(1 << mwl_vif->macid);
	spin_lock_bh(&priv->vif_lock);
	list_del(&mwl_vif->list);
	spin_unlock_bh(&priv->vif_lock);
}

static void mwl_mac80211_remove_interface(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif)
{
	struct mwl_priv *priv = hw->priv;

	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		mwl_fwcmd_set_new_stn_del(hw, vif, vif->addr);
		break;
	case NL80211_IFTYPE_STATION:
		mwl_fwcmd_remove_mac_addr(hw, vif, vif->addr);
		break;
	default:
		break;
	}

	mwl_mac80211_remove_vif(priv, vif);
}

static int mwl_mac80211_update_radar(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif;
	struct ieee80211_vif *vif;
	struct ieee80211_conf *conf = mwl_radio_conf(hw);
	u16 action = STOP_DETECT_RADAR;
	int ret;

	if (conf->radar_enabled && conf->chandef.chan &&
	    conf->chandef.chan->band == NL80211_BAND_5GHZ) {
		/*
		 * New DFS contexts start in CAC mode. Once beaconing starts,
		 * keep in-service monitoring until the radar requirement or
		 * context is removed, including temporary beacon pauses.
		 */
		action = priv->dfs_monitoring ? MONITOR_START : CAC_START;
		spin_lock_bh(&priv->vif_lock);
		list_for_each_entry(mwl_vif, &priv->vif_list, list) {
			vif = container_of((void *)mwl_vif,
					   struct ieee80211_vif, drv_priv);
			if ((vif->type == NL80211_IFTYPE_AP ||
			     vif->type == NL80211_IFTYPE_MESH_POINT) &&
			    vif->bss_conf.enable_beacon) {
				action = MONITOR_START;
				break;
			}
		}
		spin_unlock_bh(&priv->vif_lock);
	}

	wiphy_debug(hw->wiphy, "DFS action %u region %u freq %u\n",
		    action, priv->dfs_region,
		    conf->chandef.chan ? conf->chandef.chan->center_freq : 0);
	ret = mwl_fwcmd_set_radar_detect(hw, action);
	if (ret)
		wiphy_err(hw->wiphy, "failed to set radar action %u: %d\n",
			  action, ret);
	if (!ret && action == MONITOR_START)
		priv->dfs_monitoring = true;
	return ret;
}

static int mwl_configure_radio(struct ieee80211_hw *hw,
			       struct ieee80211_conf *conf, u32 changed)
{
	struct mwl_priv *priv = hw->priv;
	int rc;

	if (priv->csa_faulted && !(conf->flags & IEEE80211_CONF_IDLE))
		return -EIO;

	if (conf->flags & IEEE80211_CONF_IDLE)
		rc = mwl_fwcmd_radio_disable(hw);
	else
		rc = mwl_fwcmd_radio_enable(hw);

	if (rc)
		goto out;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL) {
		int rate = 0;

		if (!conf->chandef.chan) {
			rc = -EINVAL;
			goto out;
		}

		if (conf->chandef.chan->band == NL80211_BAND_2GHZ) {
			rc = mwl_fwcmd_set_apmode(hw, AP_MODE_2_4GHZ_11AC_MIXED);
			if (rc)
				goto out;
			rc = mwl_fwcmd_set_linkadapt_cs_mode(hw,
							   LINK_CS_STATE_CONSERV);
			rate = mwl_rates_24[0].hw_value;
		} else if (conf->chandef.chan->band == NL80211_BAND_5GHZ) {
			rc = mwl_fwcmd_set_apmode(hw, AP_MODE_11AC);
			if (rc)
				goto out;
			rc = mwl_fwcmd_set_linkadapt_cs_mode(hw,
							   LINK_CS_STATE_AUTO);
			rate = mwl_rates_50[0].hw_value;
		} else {
			rc = -EINVAL;
			goto out;
		}
		if (rc)
			goto out;

		rc = mwl_fwcmd_set_rf_channel(hw, conf);
		if (rc)
			goto out;
		rc = mwl_fwcmd_use_fixed_rate(hw, rate, rate);
		if (rc)
			goto out;
		rc = mwl_fwcmd_max_tx_power(hw, conf, 0);
		if (rc)
			goto out;
		rc = mwl_fwcmd_tx_power(hw, conf, 0);
		if (rc)
			goto out;
		rc = mwl_fwcmd_set_cdd(hw);
		if (rc)
			goto out;
	}

	/* Publish only after tuning succeeded; firmware helpers use this state. */
	if (conf->chandef.chan) {
		priv->radio_conf = *conf;
		WRITE_ONCE(priv->radio_conf_valid, true);
	}
	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		rc = mwl_mac80211_update_radar(hw);

out:
	if (rc) {
		/* A partially applied channel must not remain transmitting. */
		mwl_fwcmd_radio_disable(hw);
		priv->radio_conf_valid = false;
	}
	return rc;
}

static int mwl_mac80211_config(struct ieee80211_hw *hw, int radio_idx,
			       u32 changed)
{
	struct mwl_priv *priv = hw->priv;
	struct ieee80211_conf conf = hw->conf;

	if (priv->csa_faulted && !(conf.flags & IEEE80211_CONF_IDLE))
		return -EIO;
	if (priv->csa_state != MWL_CSA_IDLE &&
	    (conf.flags & IEEE80211_CONF_OFFCHANNEL))
		return -EBUSY;
	/* config() still controls software-scan/off-channel excursions. */
	if (priv->chanctx && !(conf.flags & IEEE80211_CONF_OFFCHANNEL)) {
		conf.chandef = priv->chanctx->def;
		conf.radar_enabled = priv->chanctx->radar_enabled;
		if (priv->csa_state == MWL_CSA_COMPLETING && priv->csa_old_ctx) {
			conf.chandef = priv->csa_request.chandef;
			conf.radar_enabled = cfg80211_chandef_dfs_required(hw->wiphy,
				&conf.chandef, NL80211_IFTYPE_AP) > 0;
		}
	} else {
		conf.radar_enabled = false;
	}
	return mwl_configure_radio(hw, &conf, changed);
}

static int mwl_apply_chanctx(struct ieee80211_hw *hw,
			     struct ieee80211_chanctx_conf *ctx)
{
	struct ieee80211_conf conf = hw->conf;

	conf.chandef = ctx->def;
	conf.radar_enabled = ctx->radar_enabled;
	conf.flags &= ~(IEEE80211_CONF_IDLE | IEEE80211_CONF_OFFCHANNEL);
	return mwl_configure_radio(hw, &conf, IEEE80211_CONF_CHANGE_CHANNEL);
}

static int mwl_mac80211_add_chanctx(struct ieee80211_hw *hw,
				  struct ieee80211_chanctx_conf *ctx)
{
	struct mwl_priv *priv = hw->priv;
	int ret;

	/* One hardware channel; multiple VIFs may share this context. */
	if (priv->csa_faulted)
		return -EIO;
	if (priv->chanctx)
		return -EBUSY;
	priv->dfs_monitoring = false;
	ret = mwl_apply_chanctx(hw, ctx);
	if (!ret)
		priv->chanctx = ctx;
	return ret;
}

static void mwl_mac80211_remove_chanctx(struct ieee80211_hw *hw,
				      struct ieee80211_chanctx_conf *ctx)
{
	struct mwl_priv *priv = hw->priv;

	if (WARN_ON(priv->chanctx != ctx))
		return;
	mwl_csa_reset(hw);
	mwl_fwcmd_set_radar_detect(hw, STOP_DETECT_RADAR);
	mwl_fwcmd_radio_disable(hw);
	priv->chanctx = NULL;
	priv->radio_conf_valid = false;
	priv->dfs_monitoring = false;
}

static void mwl_mac80211_change_chanctx(struct ieee80211_hw *hw,
				      struct ieee80211_chanctx_conf *ctx,
				      u32 changed)
{
	struct mwl_priv *priv = hw->priv;
	int ret;

	if (WARN_ON(priv->chanctx != ctx) || priv->csa_faulted)
		return;
	if ((priv->csa_state == MWL_CSA_PENDING ||
	     priv->csa_state == MWL_CSA_COMPLETING) &&
	    (changed & (IEEE80211_CHANCTX_CHANGE_CHANNEL |
			IEEE80211_CHANCTX_CHANGE_WIDTH))) {
		if (!cfg80211_chandef_identical(&ctx->def, &priv->radio_conf.chandef)) {
			mwl_csa_fail(hw, "uncoordinated channel change during CSA");
			return;
		}
		/* Do not retune a channel already selected by firmware. */
		changed &= ~(IEEE80211_CHANCTX_CHANGE_CHANNEL |
			     IEEE80211_CHANCTX_CHANGE_WIDTH);
	}
	if (changed & (IEEE80211_CHANCTX_CHANGE_CHANNEL |
		       IEEE80211_CHANCTX_CHANGE_WIDTH)) {
		priv->dfs_monitoring = false;
		ret = mwl_apply_chanctx(hw, ctx);
	} else if (changed & IEEE80211_CHANCTX_CHANGE_RADAR) {
		priv->radio_conf.radar_enabled = ctx->radar_enabled;
		if (!ctx->radar_enabled)
			priv->dfs_monitoring = false;
		ret = mwl_mac80211_update_radar(hw);
	} else {
		return;
	}
	if (ret) {
		wiphy_err(hw->wiphy, "channel context update failed: %d\n", ret);
		if (priv->csa_state != MWL_CSA_IDLE) {
			mwl_csa_fail(hw, "channel update during CSA failed");
			return;
		}
		mwl_fwcmd_radio_disable(hw);
		ieee80211_restart_hw(hw);
	}
}

static int mwl_mac80211_assign_vif_chanctx(struct ieee80211_hw *hw,
					 struct ieee80211_vif *vif,
					 struct ieee80211_bss_conf *link_conf,
					 struct ieee80211_chanctx_conf *ctx)
{
	struct mwl_priv *priv = hw->priv;

	if (link_conf->link_id)
		return -EOPNOTSUPP;
	if (priv->csa_state != MWL_CSA_IDLE)
		return -EBUSY;
	if (!priv->radio_conf_valid)
		return -EIO;
	if (priv->chanctx != ctx)
		return -EINVAL;
	mwl_dev_get_vif(vif)->chanctx = ctx;
	return 0;
}

static void mwl_mac80211_unassign_vif_chanctx(struct ieee80211_hw *hw,
					    struct ieee80211_vif *vif,
					    struct ieee80211_bss_conf *link_conf,
					    struct ieee80211_chanctx_conf *ctx)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif = mwl_dev_get_vif(vif);

	if (priv->csa_state != MWL_CSA_IDLE &&
	    priv->csa_state != MWL_CSA_RECOVERING &&
	    (priv->csa_expected & BIT(mwl_vif->macid)))
		mwl_csa_fail(hw, "participating interface unassigned");
	mwl_vif->chanctx = NULL;
}

static void mwl_mac80211_bss_info_changed_sta(struct ieee80211_hw *hw,
					      struct ieee80211_vif *vif,
					      struct ieee80211_bss_conf *info,
					      u32 changed)
{
	struct mwl_priv *priv = hw->priv;

	if ((changed & BSS_CHANGED_ERP_SLOT) && (priv->chip_type == MWL8997)) {
		if (priv->use_short_slot != vif->bss_conf.use_short_slot) {
			mwl_fwcmd_set_slot_time(hw,
						vif->bss_conf.use_short_slot);
			priv->use_short_slot = vif->bss_conf.use_short_slot;
		}
	}

	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		if (priv->use_short_preamble !=
		    vif->bss_conf.use_short_preamble) {
			mwl_fwcmd_set_radio_preamble(
				hw, vif->bss_conf.use_short_preamble);
			priv->use_short_preamble =
				vif->bss_conf.use_short_preamble;
		}
	}

	if ((changed & BSS_CHANGED_ASSOC) && vif->cfg.assoc)
		mwl_fwcmd_set_aid(hw, vif, (u8 *)vif->bss_conf.bssid,
				  vif->cfg.aid);
}

static void mwl_mac80211_bss_info_changed_ap(struct ieee80211_hw *hw,
					     struct ieee80211_vif *vif,
					     struct ieee80211_bss_conf *info,
					     u64 changed)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mwl_vif;

	mwl_vif = mwl_dev_get_vif(vif);

	if ((changed & BSS_CHANGED_ERP_SLOT) && (priv->chip_type == MWL8997)) {
		if (priv->use_short_slot != vif->bss_conf.use_short_slot) {
			mwl_fwcmd_set_slot_time(hw,
						vif->bss_conf.use_short_slot);
			priv->use_short_slot = vif->bss_conf.use_short_slot;
		}
	}

	if (changed & BSS_CHANGED_ERP_PREAMBLE) {
		if (priv->use_short_preamble !=
		    vif->bss_conf.use_short_preamble) {
			mwl_fwcmd_set_radio_preamble(
				hw, vif->bss_conf.use_short_preamble);
			priv->use_short_preamble =
				vif->bss_conf.use_short_preamble;
		}
	}

	if (changed & BSS_CHANGED_BASIC_RATES) {
		int idx;
		int rate;

		/* Use lowest supported basic rate for multicasts
		 * and management frames (such as probe responses --
		 * beacons will always go out at 1 Mb/s).
		 */
		idx = ffs(vif->bss_conf.basic_rates);
		if (idx)
			idx--;
		if (mwl_vif->basic_rate_idx != idx) {
			if (mwl_radio_conf(hw)->chandef.chan->band == NL80211_BAND_2GHZ)
				rate = mwl_rates_24[idx].hw_value;
			else
				rate = mwl_rates_50[idx].hw_value;

			mwl_fwcmd_use_fixed_rate(hw, rate, rate);
			mwl_vif->basic_rate_idx = idx;
		}
	}

	if (changed & (BSS_CHANGED_BEACON_INT | BSS_CHANGED_BEACON)) {
		struct sk_buff *skb;

		if ((vif->cfg.ssid[0] != '\0') &&
		    (vif->cfg.ssid_len != 0) &&
		    (!info->hidden_ssid)) {
			if (mwl_vif->broadcast_ssid != true) {
				mwl_fwcmd_broadcast_ssid_enable(hw, vif, true);
				mwl_vif->broadcast_ssid = true;
			}
		} else {
			if (mwl_vif->broadcast_ssid != false) {
				mwl_fwcmd_broadcast_ssid_enable(hw, vif, false);
				mwl_vif->broadcast_ssid = false;
			}
		}

		if (!mwl_vif->set_beacon || (changed & BSS_CHANGED_BEACON)) {
			skb = ieee80211_beacon_get(hw, vif, 0);

			if (skb) {
				int ret = mwl_fwcmd_set_beacon(hw, vif, skb->data, skb->len);

				dev_kfree_skb_any(skb);
				if (ret) {
					wiphy_err(hw->wiphy, "beacon upload failed: %d\n", ret);
					mwl_csa_fail(hw, "CSA/after-CSA beacon upload failed");
					return;
				}
				mwl_vif->set_beacon = true;
			} else if (priv->csa_state != MWL_CSA_IDLE) {
				mwl_csa_fail(hw, "CSA/after-CSA beacon unavailable");
				return;
			}
		}
	}

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		int ret;

		if (info->enable_beacon && priv->csa_faulted)
			return;
		if (!info->enable_beacon && priv->csa_state != MWL_CSA_IDLE &&
		    priv->csa_state != MWL_CSA_RECOVERING &&
		    (priv->csa_expected & BIT(mwl_vif->macid))) {
			mwl_csa_fail(hw, "participating AP stopped beaconing");
			return;
		}

		/* Enter in-service monitoring before enabling DFS beacons. */
		if (info->enable_beacon && mwl_radio_conf(hw)->radar_enabled) {
			ret = mwl_mac80211_update_radar(hw);
			if (ret)
				return;
		}
		ret = mwl_fwcmd_bss_start(hw, vif, info->enable_beacon);
		if (ret) {
			wiphy_err(hw->wiphy, "failed to change beacon state: %d\n",
				  ret);
			return;
		}
		if (!info->enable_beacon && mwl_radio_conf(hw)->radar_enabled)
			mwl_mac80211_update_radar(hw);
	}
}

static void mwl_mac80211_bss_info_changed(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  struct ieee80211_bss_conf *info,
					  u64 changed)
{
	switch (vif->type) {
	case NL80211_IFTYPE_AP:
	case NL80211_IFTYPE_MESH_POINT:
		mwl_mac80211_bss_info_changed_ap(hw, vif, info, changed);
		break;
	case NL80211_IFTYPE_STATION:
		mwl_mac80211_bss_info_changed_sta(hw, vif, info, changed);
		break;
	default:
		break;
	}
}

static void mwl_mac80211_configure_filter(struct ieee80211_hw *hw,
					  unsigned int changed_flags,
					  unsigned int *total_flags,
					  u64 multicast)
{
	/* AP firmware doesn't allow fine-grained control over
	 * the receive filter.
	 */
	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC;
}

static int mwl_mac80211_set_key(struct ieee80211_hw *hw,
				enum set_key_cmd cmd_param,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta,
				struct ieee80211_key_conf *key)
{
	struct mwl_vif *mwl_vif;
	struct mwl_sta *sta_info;
	struct mwl_priv *priv=hw->priv;
	int rc = 0;
	u8 encr_type;
	u8 *addr;

	mwl_vif = mwl_dev_get_vif(vif);
	addr = sta ? sta->addr : vif->addr;

	if (cmd_param == SET_KEY) {
		if ((key->cipher == WLAN_CIPHER_SUITE_WEP40) ||
		    (key->cipher == WLAN_CIPHER_SUITE_WEP104)) {
			encr_type = ENCR_TYPE_WEP;
		} else if (key->cipher == WLAN_CIPHER_SUITE_CCMP) {
			encr_type = ENCR_TYPE_AES;
			if (priv->chip_type != MWL8964)
				key->flags |= IEEE80211_KEY_FLAG_GENERATE_IV;
		} else if (key->cipher == WLAN_CIPHER_SUITE_TKIP) {
			if (priv->chip_type != MWL8964)
				key->flags |= IEEE80211_KEY_FLAG_GENERATE_MMIC | IEEE80211_KEY_FLAG_GENERATE_IV;
			encr_type = ENCR_TYPE_TKIP;
		} else {
			encr_type = ENCR_TYPE_DISABLE;
		}

		rc = mwl_fwcmd_update_encryption_enable(hw, vif, addr,
							encr_type);
		if (rc)
			goto out;
		rc = mwl_fwcmd_encryption_set_key(hw, vif, addr, key);
		if (rc)
			goto out;

		mwl_vif->is_hw_crypto_enabled = true;
		if (sta) {
			sta_info = mwl_dev_get_sta(sta);
			sta_info->is_key_set = true;
		}
	} else {
		rc = mwl_fwcmd_encryption_remove_key(hw, vif, addr, key);
		if (vif->type == NL80211_IFTYPE_STATION)
			mwl_vif->is_hw_crypto_enabled = false;
		if (rc)
			goto out;
	}

out:

	return rc;
}

static int mwl_mac80211_set_rts_threshold(struct ieee80211_hw *hw, int radio_idx,
					  u32 value)
{
	return mwl_fwcmd_set_rts_threshold(hw, value);
}

static int mwl_mac80211_sta_add(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	struct mwl_priv *priv = hw->priv;
	u16 stnid, sta_stnid = 0;
	struct mwl_vif *mwl_vif;
	struct wireless_dev *wdev = ieee80211_vif_to_wdev(vif);
	bool use_4addr = wdev->use_4addr;
	struct mwl_sta *sta_info;
	struct ieee80211_key_conf *key;
	int rc;
	int i;

	if (vif->type == NL80211_IFTYPE_STATION)
		sta->aid = 1;
	mwl_vif = mwl_dev_get_vif(vif);
	stnid = utils_assign_stnid(priv, mwl_vif->macid, sta->aid);
	if (!stnid)
		return -EPERM;
	if (vif->type == NL80211_IFTYPE_STATION) {
		sta_stnid = utils_assign_stnid(priv, mwl_vif->macid,
					       sta->aid + 1);
		if (!sta_stnid) {
			utils_free_stnid(priv, stnid);
			return -EPERM;
		}
		ether_addr_copy(mwl_vif->bssid, sta->addr);
	}
	sta_info = mwl_dev_get_sta(sta);
	memset(sta_info, 0, sizeof(*sta_info));

	if (vif->type == NL80211_IFTYPE_MESH_POINT)
		sta_info->is_mesh_node = true;

	if (sta->deflink.ht_cap.ht_supported) {
		sta_info->is_ampdu_allowed = true;
		sta_info->is_amsdu_allowed = false;
		if (sta->deflink.ht_cap.cap & IEEE80211_HT_CAP_MAX_AMSDU) {
			sta_info->amsdu_ctrl.cap = MWL_AMSDU_SIZE_8K;
			sta_info->amsdu_ctrl.amsdu_allow_size = SYSADPT_AMSDU_8K_MAX_SIZE;
		}
		else {
			sta_info->amsdu_ctrl.cap = MWL_AMSDU_SIZE_4K;
			sta_info->amsdu_ctrl.amsdu_allow_size = SYSADPT_AMSDU_4K_MAX_SIZE;
		}
		if ((sta->tdls) && (!sta->wme))
			sta->wme = true;
	}
	sta_info->mwl_vif = mwl_vif;
	sta_info->stnid = stnid;
	if (vif->type == NL80211_IFTYPE_STATION)
		sta_info->sta_stnid = sta_stnid;
	sta_info->tx_rate_info = utils_get_init_tx_rate(priv, mwl_radio_conf(hw), sta);
	spin_lock_init(&sta_info->amsdu_lock);
	spin_lock_bh(&priv->sta_lock);
	list_add_tail(&sta_info->list, &priv->sta_list);
	spin_unlock_bh(&priv->sta_lock);

	if (vif->type == NL80211_IFTYPE_STATION)
		mwl_fwcmd_set_new_stn_del(hw, vif, sta->addr);

	if (priv->chip_type == MWL8964) {
		if (use_4addr) {
			sta_info->wds = true;
			rc = mwl_fwcmd_set_new_stn_add_sc4(hw, vif, sta,
							   WDS_MODE);
		} else
			rc = mwl_fwcmd_set_new_stn_add_sc4(hw, vif, sta, 0);
	} else
		rc = mwl_fwcmd_set_new_stn_add(hw, vif, sta);

	if ((vif->type == NL80211_IFTYPE_STATION) && !use_4addr)
		mwl_hif_set_sta_id(hw, sta, true, true);
	else
		mwl_hif_set_sta_id(hw, sta, false, true);

	for (i = 0; i < NUM_WEP_KEYS; i++) {
		key = (struct ieee80211_key_conf *)mwl_vif->wep_key_conf[i].key;

		if (mwl_vif->wep_key_conf[i].enabled)
			mwl_mac80211_set_key(hw, SET_KEY, vif, sta, key);
	}

	mwl_get_rateinfo(priv, sta->addr, sta_info);

	return rc;
}

static int mwl_mac80211_sta_remove(struct ieee80211_hw *hw,
				   struct ieee80211_vif *vif,
				   struct ieee80211_sta *sta)
{
	struct mwl_priv *priv = hw->priv;
	int rc;
	struct mwl_sta *sta_info = mwl_dev_get_sta(sta);

	mwl_hif_tx_del_sta_amsdu_pkts(hw, sta);
	mwl_fwcmd_del_sta_streams(hw, sta);
	mwl_hif_tx_del_pkts_via_sta(hw, sta);

	rc = mwl_fwcmd_set_new_stn_del(hw, vif, sta->addr);

	if (vif->type == NL80211_IFTYPE_STATION)
		mwl_hif_set_sta_id(hw, sta, true, false);
	else
		mwl_hif_set_sta_id(hw, sta, false, false);

	if (priv->chip_type != MWL8964)
		utils_free_stnid(priv, sta_info->stnid);
	if (vif->type == NL80211_IFTYPE_STATION)
		utils_free_stnid(priv, sta_info->sta_stnid);

	spin_lock_bh(&priv->sta_lock);
	list_del(&sta_info->list);
	spin_unlock_bh(&priv->sta_lock);

	return rc;
}

static int mwl_mac80211_conf_tx(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				unsigned int link_id, u16 queue,
				const struct ieee80211_tx_queue_params *params)
{
	struct mwl_priv *priv = hw->priv;
	int rc = 0;

	if (WARN_ON(queue > SYSADPT_TX_WMM_QUEUES - 1))
		return -EINVAL;

	memcpy(&priv->wmm_params[queue], params, sizeof(*params));

	if (!priv->wmm_enabled) {
		rc = mwl_fwcmd_set_wmm_mode(hw, true);
		priv->wmm_enabled = true;
	}

	if (!rc) {
		int q = SYSADPT_TX_WMM_QUEUES - 1 - queue;

		rc = mwl_fwcmd_set_edca_params(hw, q,
					       params->cw_min, params->cw_max,
					       params->aifs, params->txop);
	}

	return rc;
}

static int mwl_mac80211_get_stats(struct ieee80211_hw *hw,
				  struct ieee80211_low_level_stats *stats)
{
	return mwl_fwcmd_get_stat(hw, stats);
}

static int mwl_mac80211_get_survey(struct ieee80211_hw *hw,
				   int idx,
				   struct survey_info *survey)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_survey_info *survey_info;

	if (!mwl_radio_conf(hw)->chandef.chan)
		return -ENOENT;
	if (priv->survey_info_idx) {
		if (idx >= priv->survey_info_idx) {
			priv->survey_info_idx = 0;
			return -ENOENT;
		}
		survey_info = &priv->survey_info[idx];
	} else {
		if (idx != 0)
			return -ENOENT;
		mwl_fwcmd_get_survey(hw, 0);
		survey_info = &priv->cur_survey_info;
		if (!(mwl_radio_conf(hw)->flags & IEEE80211_CONF_OFFCHANNEL))
			survey->filled |= SURVEY_INFO_IN_USE;
	}

	survey->channel = &survey_info->channel;
	survey->filled |= survey_info->filled;
	survey->time = survey_info->time_period / 1000;
	survey->time_busy = survey_info->time_busy / 1000;
	survey->time_tx = survey_info->time_tx / 1000;
	survey->noise = survey_info->noise;

	return 0;
}

static int mwl_mac80211_ampdu_action(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_ampdu_params *params)
{
	int rc = 0;
	struct mwl_priv *priv = hw->priv;
	struct mwl_ampdu_stream *stream;
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct ieee80211_sta *sta = params->sta;
	u16 tid = params->tid;
	u8 buf_size = params->buf_size;
	u8 *addr = sta->addr;
	struct mwl_sta *sta_info;
	struct mwl_vif *mwl_vif = mwl_dev_get_vif(vif);

	if (priv->debug_ampdu)
		wiphy_debug(hw->wiphy, "ampdu macid %i sta %pM tid %u action %d\n",
			    mwl_vif->macid, sta->addr, tid, action);

	sta_info = mwl_dev_get_sta(sta);

	spin_lock_bh(&priv->stream_lock);

	stream = mwl_fwcmd_lookup_stream(hw, sta, tid);

	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		if (priv->chip_type == MWL8964) {
			struct mwl_ampdu_stream tmp;

			tmp.sta = sta;
			tmp.tid = tid;
			spin_unlock_bh(&priv->stream_lock);
			mwl_fwcmd_create_ba(hw, &tmp, vif,
					    BA_FLAG_DIRECTION_DOWN,
					    buf_size, params->ssn,
					    params->amsdu);
			spin_lock_bh(&priv->stream_lock);
		}
		break;
	case IEEE80211_AMPDU_RX_STOP:
		if (priv->chip_type == MWL8964) {
			struct mwl_ampdu_stream tmp;

			tmp.sta = sta;
			tmp.tid = tid;
			spin_unlock_bh(&priv->stream_lock);
			mwl_fwcmd_destroy_ba(hw, &tmp,
					     BA_FLAG_DIRECTION_DOWN);
			spin_lock_bh(&priv->stream_lock);
		}
		break;
	case IEEE80211_AMPDU_TX_START:
		if (!sta_info->is_ampdu_allowed) {
			wiphy_warn(hw->wiphy, "ampdu not allowed\n");
			rc = -EPERM;
			break;
		}

		if (!stream) {
			stream = mwl_fwcmd_add_stream(hw, sta, tid);
			if (!stream) {
				wiphy_warn(hw->wiphy, "no stream found\n");
				rc = -EPERM;
				break;
			}
		}

		if (priv->chip_type != MWL8964) {
			spin_unlock_bh(&priv->stream_lock);
			rc = mwl_fwcmd_check_ba(hw, stream, vif,
					BA_FLAG_DIRECTION_UP);
			spin_lock_bh(&priv->stream_lock);
			if (rc) {
				mwl_fwcmd_remove_stream(hw, stream);
				sta_info->check_ba_failed[tid]++;
				break;
			}
		}
		stream->state = AMPDU_STREAM_IN_PROGRESS;
		spin_unlock_bh(&priv->stream_lock);
		rc = mwl_fwcmd_get_seqno(hw, stream, &params->ssn);
		spin_lock_bh(&priv->stream_lock);
		if (rc)
			break;
		ieee80211_start_tx_ba_cb_irqsafe(vif, addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		if (stream) {
			if (stream->state == AMPDU_STREAM_ACTIVE) {
				stream->state = AMPDU_STREAM_IN_PROGRESS;
				mwl_hif_tx_del_ampdu_pkts(hw, sta, stream->desc_num);
				spin_unlock_bh(&priv->stream_lock);
				mwl_fwcmd_destroy_ba(hw, stream,
						     BA_FLAG_DIRECTION_UP);
				spin_lock_bh(&priv->stream_lock);
				sta_info->is_amsdu_allowed = false;
			}

			mwl_fwcmd_remove_stream(hw, stream);
			ieee80211_stop_tx_ba_cb_irqsafe(vif, addr, tid);
		} else {
			rc = -EPERM;
		}
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		if (stream) {
			if (WARN_ON(stream->state !=
				    AMPDU_STREAM_IN_PROGRESS)) {
				rc = -EPERM;
				break;
			}
			spin_unlock_bh(&priv->stream_lock);
			rc = mwl_fwcmd_create_ba(hw, stream, vif,
						 BA_FLAG_DIRECTION_UP,
						 buf_size, params->ssn,
						 params->amsdu);

			if (rc) {
				mwl_fwcmd_destroy_ba(hw, stream, BA_FLAG_DIRECTION_UP);
				spin_lock_bh(&priv->stream_lock);
				mwl_fwcmd_remove_stream(hw, stream);
				wiphy_err(hw->wiphy,
					  "ampdu operation error code: %d\n",
					  rc);
			} else {
				spin_lock_bh(&priv->stream_lock);
				stream->state = AMPDU_STREAM_ACTIVE;
				sta_info->check_ba_failed[tid] = 0;
				if (priv->tx_amsdu)
					sta_info->is_amsdu_allowed = params->amsdu;
				else
					sta_info->is_amsdu_allowed = false;
			}
		} else {
			rc = -EPERM;
		}
		break;
	default:
		rc = -ENOTSUPP;
		break;
	}

	spin_unlock_bh(&priv->stream_lock);

	return rc;
}

/* All CSA state except IRQ event tagging is protected by the wiphy lock. */
static void mwl_csa_reset(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;

	cancel_delayed_work(&priv->csa_timeout_work);
	WRITE_ONCE(priv->csa_state, MWL_CSA_IDLE);
	WRITE_ONCE(priv->csa_active, false);
	WRITE_ONCE(priv->csa_generation, priv->csa_generation + 1);
	priv->csa_old_ctx = NULL;
	priv->csa_expected = 0;
	priv->csa_prepared = 0;
	priv->csa_ready = 0;
	priv->csa_posted = 0;
	priv->csa_bss_stopped = 0;
}

static void mwl_csa_arm_timeout(struct mwl_priv *priv, unsigned long delay)
{
	priv->csa_deadline = jiffies + delay;
	mod_delayed_work(system_wq, &priv->csa_timeout_work, delay);
}

static void mwl_csa_fail(struct ieee80211_hw *hw, const char *reason)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mv;
	int ret;

	if (priv->csa_state == MWL_CSA_IDLE ||
	    priv->csa_state == MWL_CSA_RECOVERING)
		return;
	wiphy_err(hw->wiphy, "CSA failed: %s; stopping radio/interfaces\n", reason);
	WRITE_ONCE(priv->csa_state, MWL_CSA_RECOVERING);
	priv->csa_faulted = true;
	WRITE_ONCE(priv->csa_active, false);
	WRITE_ONCE(priv->csa_generation, priv->csa_generation + 1);
	cancel_delayed_work(&priv->csa_timeout_work);
	ieee80211_stop_queues(hw);
	/* Never recover by resuming an old channel that may be in NOP. */
	list_for_each_entry(mv, &priv->vif_list, list) {
		struct ieee80211_vif *vif =
			container_of((void *)mv, struct ieee80211_vif, drv_priv);

		if (!mv->chanctx)
			continue;
		ret = mwl_fwcmd_bss_start(hw, vif, false);
		if (ret)
			wiphy_err(hw->wiphy, "CSA recovery BSS stop failed: %d\n", ret);
		mv->set_beacon = false;
		cfg80211_stop_iface(hw->wiphy, ieee80211_vif_to_wdev(vif),
				    GFP_KERNEL);
	}
	ret = mwl_fwcmd_radio_disable(hw);
	if (ret)
		wiphy_err(hw->wiphy, "CSA recovery radio stop failed: %d\n", ret);
	WRITE_ONCE(priv->radio_conf_valid, false);
	priv->dfs_monitoring = false;
}

void mwl_csa_timeout(struct work_struct *work)
{
	struct mwl_priv *priv = container_of(to_delayed_work(work),
					   struct mwl_priv, csa_timeout_work);

	wiphy_lock(priv->hw->wiphy);
	if (priv->csa_state != MWL_CSA_IDLE &&
	    priv->csa_state != MWL_CSA_RECOVERING) {
		/* A cancelled/running timeout must not expire a newer request. */
		if (time_before(jiffies, priv->csa_deadline))
			mod_delayed_work(system_wq, &priv->csa_timeout_work,
					 priv->csa_deadline - jiffies);
		else
			mwl_csa_fail(priv->hw, "preparation/firmware/finalization timeout");
	}
	wiphy_unlock(priv->hw->wiphy);
}

void mwl_queue_csa_event(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;

	if (READ_ONCE(priv->csa_state) != MWL_CSA_PENDING)
		return;
	WRITE_ONCE(priv->csa_event_generation, READ_ONCE(priv->csa_generation));
	ieee80211_queue_work(hw, &priv->chnl_switch_handle);
}

static int mwl_csa_target_allowed(struct ieee80211_hw *hw,
				const struct cfg80211_chan_def *target)
{
	struct mwl_priv *priv = hw->priv;
	struct ieee80211_channel *old = mwl_radio_conf(hw)->chandef.chan;
	int radar;

	if (!target->chan || !old || !cfg80211_chandef_valid(target) ||
	    target->chan->band != old->band || target->chan->freq_offset ||
	    target->punctured)
		return -EINVAL;
	switch (target->width) {
	case NL80211_CHAN_WIDTH_20_NOHT:
	case NL80211_CHAN_WIDTH_20:
	case NL80211_CHAN_WIDTH_40:
	case NL80211_CHAN_WIDTH_80:
		break;
	default:
		return -EOPNOTSUPP;
	}
	/* Strict check includes all subchannels and DFS availability. */
	if (!cfg80211_reg_can_beacon(hw->wiphy, (struct cfg80211_chan_def *)target,
				    NL80211_IFTYPE_AP))
		return -EPERM;
	radar = cfg80211_chandef_dfs_required(hw->wiphy, target,
					    NL80211_IFTYPE_AP);
	if (radar < 0)
		return radar;
	if (!radar)
		return 0;
	/* CSA does not perform CAC. Keep an already-running detector across
	 * DFS-to-DFS switches; never enable target beacons needing new CAC.
	 */
	if (cfg80211_chandef_dfs_usable(hw->wiphy, target))
		return -EPERM;
	if (!priv->radio_conf.radar_enabled || !priv->dfs_monitoring)
		return -EOPNOTSUPP;
	/* Japan uses distinct profiles in channels 52-64 and 100-140. */
	if (priv->dfs_region == NL80211_DFS_JP &&
	    ((old->hw_value <= 64) != (target->chan->hw_value <= 64)))
		return -EOPNOTSUPP;
	return 0;
}

static int mwl_mac80211_chnl_switch(struct ieee80211_hw *hw,
				    struct ieee80211_vif *vif,
				    struct ieee80211_channel_switch *request)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mv = mwl_dev_get_vif(vif), *iter;
	u32 expected = 0, bit = BIT(mv->macid);
	u64 timeout_ms;
	int ret;

	if (vif->type != NL80211_IFTYPE_AP || !vif->bss_conf.beacon_int ||
	    request->link_id ||
	    !priv->chanctx || mv->chanctx != priv->chanctx ||
	    !priv->radio_conf_valid || priv->sw_scanning)
		return -EOPNOTSUPP;
	if (priv->csa_state != MWL_CSA_IDLE &&
	    priv->csa_state != MWL_CSA_PREPARED)
		return -EBUSY;
	ret = mwl_csa_target_allowed(hw, &request->chandef);
	if (ret)
		return ret;
	/* Firmware countdown may resume beacons before its interrupt. Until
	 * detector persistence across that retune is verified, use only a
	 * non-DFS target. Immediate switches stop BSSes and can configure
	 * detection before restarting them on an already-available target.
	 */
	if (request->count > 1 &&
	    cfg80211_chandef_dfs_required(hw->wiphy, &request->chandef,
					 NL80211_IFTYPE_AP) > 0)
		return -EOPNOTSUPP;
	if (cfg80211_chandef_identical(&priv->radio_conf.chandef,
					 &request->chandef))
		return -EINVAL;
	list_for_each_entry(iter, &priv->vif_list, list) {
		struct ieee80211_vif *ivif =
			container_of((void *)iter, struct ieee80211_vif, drv_priv);

		if (!iter->chanctx)
			continue;
		/* Every user of this physical channel must participate. */
		if (iter->chanctx != priv->chanctx ||
		    ivif->type != NL80211_IFTYPE_AP ||
		    !ivif->bss_conf.enable_beacon ||
		    ivif->bss_conf.beacon_int != vif->bss_conf.beacon_int)
			return -EBUSY;
		expected |= BIT(iter->macid);
	}
	if (!(expected & bit) || (!is_power_of_2(expected) && request->count <= 1))
		return -EOPNOTSUPP;
	timeout_ms = div_u64(((u64)request->count + 2) *
			     vif->bss_conf.beacon_int * 1024, 1000) + 3000;
	if (timeout_ms > 60000)
		return -EINVAL;
	if (priv->csa_state == MWL_CSA_PREPARED) {
		if (expected != priv->csa_expected ||
		    !cfg80211_chandef_identical(&request->chandef,
						 &priv->csa_request.chandef) ||
		    request->count != priv->csa_request.count ||
		    request->block_tx != priv->csa_request.block_tx)
			return -EBUSY;
	} else {
		priv->csa_request = *request;
		priv->csa_old_ctx = priv->chanctx;
		priv->csa_expected = expected;
		priv->csa_prepared = 0;
		priv->csa_ready = 0;
		priv->csa_posted = 0;
		priv->csa_bss_stopped = 0;
		priv->csa_timeout_jiffies = msecs_to_jiffies(timeout_ms);
		WRITE_ONCE(priv->csa_generation, priv->csa_generation + 1);
		WRITE_ONCE(priv->csa_state, MWL_CSA_PREPARED);
		mwl_csa_arm_timeout(priv, msecs_to_jiffies(5000));
	}
	priv->csa_prepared |= bit;
	wiphy_debug(hw->wiphy, "CSA prepare gen %u vif %pM target %u count %u block %u mask %x/%x\n",
		   priv->csa_generation, vif->addr,
		   request->chandef.chan->center_freq, request->count,
		   request->block_tx, priv->csa_prepared, expected);
	return 0;
}

static void mwl_mac80211_channel_switch_beacon(struct ieee80211_hw *hw,
					     struct ieee80211_vif *vif,
					     struct cfg80211_chan_def *target)
{
	struct mwl_priv *priv = hw->priv;
	u32 bit = BIT(mwl_dev_get_vif(vif)->macid);
	int ret;

	if (priv->csa_state != MWL_CSA_PREPARED ||
	    !(priv->csa_prepared & bit) ||
	    !cfg80211_chandef_identical(target, &priv->csa_request.chandef)) {
		mwl_csa_fail(hw, "unexpected CSA beacon callback");
		return;
	}
	priv->csa_ready |= bit;
	if (priv->csa_ready != priv->csa_expected)
		return;
	ret = mwl_csa_target_allowed(hw, target);
	if (ret) {
		mwl_csa_fail(hw, "target no longer available");
		return;
	}
	/* Reservation and CSA beacons now exist for every participating VIF.
	 * Publish pending before submission: a completion can interrupt it.
	 */
	WRITE_ONCE(priv->csa_state, MWL_CSA_PENDING);
	WRITE_ONCE(priv->csa_active, true);
	mwl_csa_arm_timeout(priv, priv->csa_timeout_jiffies);
	ret = mwl_fwcmd_set_switch_channel(hw, &priv->csa_request);
	if (ret)
		mwl_csa_fail(hw, "firmware switch command rejected");
	else
		wiphy_debug(hw->wiphy, "CSA submitted gen %u target %u\n",
			   priv->csa_generation, target->chan->center_freq);
}

void mwl_csa_event(struct ieee80211_hw *hw)
{
	struct mwl_priv *priv = hw->priv;
	struct mwl_vif *mv;

	if (priv->csa_state != MWL_CSA_PENDING ||
	    READ_ONCE(priv->csa_event_generation) != priv->csa_generation) {
		wiphy_debug(hw->wiphy, "ignoring stale/duplicate CSA completion\n");
		return;
	}
	if (mwl_csa_target_allowed(hw, &priv->csa_request.chandef)) {
		mwl_csa_fail(hw, "target invalid at firmware completion");
		return;
	}
	WRITE_ONCE(priv->csa_state, MWL_CSA_COMPLETING);
	ieee80211_stop_queues(hw);
	/* Firmware has already tuned. Publish RX/TX channel before notifying
	 * mac80211; switch_vif_chanctx must not issue a second RF switch.
	 */
	priv->radio_conf.chandef = priv->csa_request.chandef;
	mwl_csa_arm_timeout(priv, msecs_to_jiffies(5000));
	list_for_each_entry(mv, &priv->vif_list, list) {
		struct ieee80211_vif *vif =
			container_of((void *)mv, struct ieee80211_vif, drv_priv);

		if (!(priv->csa_expected & BIT(mv->macid)))
			continue;
		if (mv->chanctx != priv->csa_old_ctx || !vif->bss_conf.csa_active) {
			mwl_csa_fail(hw, "completion without participating interface");
			return;
		}
	}
	list_for_each_entry(mv, &priv->vif_list, list) {
		struct ieee80211_vif *vif =
			container_of((void *)mv, struct ieee80211_vif, drv_priv);

		if (priv->csa_expected & BIT(mv->macid)) {
			mv->set_beacon = false;
			ieee80211_csa_finish(vif, 0);
		}
	}
	wiphy_debug(hw->wiphy, "CSA firmware completion gen %u\n",
		   priv->csa_generation);
}

static int mwl_mac80211_switch_vif_chanctx(struct ieee80211_hw *hw,
				struct ieee80211_vif_chanctx_switch *vifs,
				int n_vifs, enum ieee80211_chanctx_switch_mode mode)
{
	struct mwl_priv *priv = hw->priv;
	struct ieee80211_chanctx_conf *target;
	u32 mask = 0;
	int i, ret;
	bool firmware_done = priv->csa_state == MWL_CSA_COMPLETING;

	if (mode != CHANCTX_SWMODE_SWAP_CONTEXTS) {
		if (firmware_done)
			mwl_csa_fail(hw, "unsupported replacement mode after switch");
		return -EOPNOTSUPP;
	}
	if (n_vifs <= 0 || (priv->csa_state != MWL_CSA_PREPARED && !firmware_done)) {
		if (firmware_done)
			mwl_csa_fail(hw, "invalid completion replacement list");
		return -EINVAL;
	}
	if (!firmware_done && priv->csa_request.count > 1) {
		mwl_csa_fail(hw, "context replacement before firmware completion");
		return -EBUSY;
	}
	target = vifs[0].new_ctx;
	if (!target || priv->chanctx != priv->csa_old_ctx ||
	    !cfg80211_chandef_identical(&target->def, &priv->csa_request.chandef)) {
		ret = -EINVAL;
		goto fail;
	}
	for (i = 0; i < n_vifs; i++) {
		struct mwl_vif *mv = mwl_dev_get_vif(vifs[i].vif);
		u32 bit = BIT(mv->macid);

		if (!vifs[i].link_conf || vifs[i].link_conf->link_id ||
		    vifs[i].old_ctx != priv->chanctx || vifs[i].new_ctx != target ||
		    mv->chanctx != priv->chanctx || (mask & bit)) {
			ret = -EINVAL;
			goto fail;
		}
		mask |= bit;
	}
	if (mask != priv->csa_expected || mask != priv->csa_prepared) {
		ret = -EBUSY;
		goto fail;
	}
	ret = mwl_csa_target_allowed(hw, &target->def);
	if (ret)
		goto fail;
	ret = cfg80211_chandef_dfs_required(hw->wiphy, &target->def,
					  NL80211_IFTYPE_AP);
	if (ret < 0 || !!ret != target->radar_enabled) {
		ret = -EINVAL;
		goto fail;
	}
	ieee80211_stop_queues(hw);
	if (!firmware_done) {
		/* Count <= 1 has no CSA beacon callback in mac80211. Stop BSS
		 * transmission for the immediate software-controlled retune.
		 */
		for (i = 0; i < n_vifs; i++) {
			ret = mwl_fwcmd_bss_start(hw, vifs[i].vif, false);
			if (ret)
				goto fail;
			priv->csa_bss_stopped |= BIT(mwl_dev_get_vif(vifs[i].vif)->macid);
		}
		ret = mwl_apply_chanctx(hw, target);
	} else {
		/* Firmware has tuned; only apply target power and detection. */
		priv->radio_conf.chandef = target->def;
		priv->radio_conf.radar_enabled = target->radar_enabled;
		ret = mwl_fwcmd_max_tx_power(hw, &priv->radio_conf, 0);
		if (!ret)
			ret = mwl_fwcmd_tx_power(hw, &priv->radio_conf, 0);
		if (!ret)
			ret = mwl_mac80211_update_radar(hw);
	}
	if (ret)
		goto fail;
	if (!target->radar_enabled)
		priv->dfs_monitoring = false;
	priv->chanctx = target;
	for (i = 0; i < n_vifs; i++)
		mwl_dev_get_vif(vifs[i].vif)->chanctx = target;
	/* Old context ceases to exist when this callback succeeds. */
	priv->csa_old_ctx = NULL;
	WRITE_ONCE(priv->csa_state, MWL_CSA_COMPLETING);
	mwl_csa_arm_timeout(priv, msecs_to_jiffies(5000));
	wiphy_debug(hw->wiphy, "CSA context committed gen %u target %u\n",
		   priv->csa_generation, target->def.chan->center_freq);
	return 0;
fail:
	mwl_csa_fail(hw, "context switch configuration failed");
	return ret;
}

static int mwl_mac80211_post_channel_switch(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif,
					  struct ieee80211_bss_conf *link_conf)
{
	struct mwl_priv *priv = hw->priv;
	u32 bit = BIT(mwl_dev_get_vif(vif)->macid);
	int ret;

	if (priv->csa_state != MWL_CSA_COMPLETING ||
	    !(priv->csa_expected & bit) || link_conf->link_id ||
	    mwl_dev_get_vif(vif)->chanctx != priv->chanctx ||
	    !cfg80211_chandef_identical(&priv->radio_conf.chandef,
					 &priv->csa_request.chandef)) {
		mwl_csa_fail(hw, "unexpected post-switch callback");
		return -EINVAL;
	}
	ret = mwl_csa_target_allowed(hw, &priv->radio_conf.chandef);
	if (ret)
		goto fail;
	if (priv->csa_bss_stopped & bit) {
		ret = mwl_fwcmd_bss_start(hw, vif, true);
		if (ret)
			goto fail;
	}
	priv->csa_posted |= bit;
	if (priv->csa_posted == priv->csa_expected) {
		wiphy_debug(hw->wiphy, "CSA finished gen %u\n", priv->csa_generation);
		mwl_csa_reset(hw);
		ieee80211_wake_queues(hw);
	}
	return 0;
fail:
	mwl_csa_fail(hw, "post-switch target/BSS validation failed");
	return ret;
}

static void mwl_mac80211_abort_channel_switch(struct ieee80211_hw *hw,
					    struct ieee80211_vif *vif,
					    struct ieee80211_bss_conf *link_conf)
{
	struct mwl_priv *priv = hw->priv;

	if (priv->csa_expected & BIT(mwl_dev_get_vif(vif)->macid))
		mwl_csa_fail(hw, "channel switch aborted");
}

static void mwl_mac80211_sw_scan_start(struct ieee80211_hw *hw,
				       struct ieee80211_vif *vif,
				       const u8 *mac_addr)
{
	struct mwl_priv *priv = hw->priv;

	priv->sw_scanning = true;
	priv->survey_info_idx = 0;
}

static void mwl_mac80211_sw_scan_complete(struct ieee80211_hw *hw,
					  struct ieee80211_vif *vif)
{
	struct mwl_priv *priv = hw->priv;

	priv->sw_scanning = false;
}

const struct ieee80211_ops mwl_mac80211_ops = {
	.tx                 = mwl_mac80211_tx,
	.start              = mwl_mac80211_start,
	.stop               = mwl_mac80211_stop,
	.add_interface      = mwl_mac80211_add_interface,
	.remove_interface   = mwl_mac80211_remove_interface,
	.config             = mwl_mac80211_config,
	.bss_info_changed   = mwl_mac80211_bss_info_changed,
	.configure_filter   = mwl_mac80211_configure_filter,
	.set_key            = mwl_mac80211_set_key,
	.set_rts_threshold  = mwl_mac80211_set_rts_threshold,
	.sta_add            = mwl_mac80211_sta_add,
	.sta_remove         = mwl_mac80211_sta_remove,
	.conf_tx            = mwl_mac80211_conf_tx,
	.get_stats          = mwl_mac80211_get_stats,
	.get_survey         = mwl_mac80211_get_survey,
	.ampdu_action       = mwl_mac80211_ampdu_action,
	.pre_channel_switch = mwl_mac80211_chnl_switch,
	.channel_switch_beacon = mwl_mac80211_channel_switch_beacon,
	.post_channel_switch = mwl_mac80211_post_channel_switch,
	.abort_channel_switch = mwl_mac80211_abort_channel_switch,
	.sw_scan_start      = mwl_mac80211_sw_scan_start,
	.sw_scan_complete   = mwl_mac80211_sw_scan_complete,
	.wake_tx_queue	    = ieee80211_handle_wake_tx_queue,
	.add_chanctx        = mwl_mac80211_add_chanctx,
	.remove_chanctx     = mwl_mac80211_remove_chanctx,
	.change_chanctx     = mwl_mac80211_change_chanctx,
	.assign_vif_chanctx = mwl_mac80211_assign_vif_chanctx,
	.unassign_vif_chanctx = mwl_mac80211_unassign_vif_chanctx,
	.switch_vif_chanctx = mwl_mac80211_switch_vif_chanctx,
};
