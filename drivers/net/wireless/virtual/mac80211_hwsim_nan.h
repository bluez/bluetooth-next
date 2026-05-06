// SPDX-License-Identifier: GPL-2.0-only
/*
 * mac80211_hwsim_nan - NAN software simulation for mac80211_hwsim
 * Copyright (C) 2025 Intel Corporation
 */

#ifndef __MAC80211_HWSIM_NAN_H
#define __MAC80211_HWSIM_NAN_H

struct mac80211_hwsim_nan_data {
	struct ieee80211_vif *device_vif;
	u8 bands;

	struct hrtimer slot_timer;
	struct hrtimer resume_txqs_timer;
	bool notify_dw;
};

enum hrtimer_restart
mac80211_hwsim_nan_slot_timer(struct hrtimer *timer);
enum hrtimer_restart
mac80211_hwsim_nan_resume_txqs_timer(struct hrtimer *timer);

int mac80211_hwsim_nan_start(struct ieee80211_hw *hw,
			     struct ieee80211_vif *vif,
			     struct cfg80211_nan_conf *conf);

int mac80211_hwsim_nan_stop(struct ieee80211_hw *hw,
			    struct ieee80211_vif *vif);

int mac80211_hwsim_nan_change_config(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct cfg80211_nan_conf *conf,
				     u32 changes);

bool mac80211_hwsim_nan_txq_transmitting(struct ieee80211_hw *hw,
					 struct ieee80211_txq *txq);

struct ieee80211_channel *
mac80211_hwsim_nan_get_tx_channel(struct ieee80211_hw *hw);

bool mac80211_hwsim_nan_receive(struct ieee80211_hw *hw,
				struct ieee80211_channel *channel,
				struct ieee80211_rx_status *rx_status);

#endif /* __MAC80211_HWSIM_NAN_H */
