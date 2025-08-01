/*
 * Copyright (c) 2015-2016, Mellanox Technologies. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef __MLX5_EN_STATS_H__
#define __MLX5_EN_STATS_H__

#define MLX5E_READ_CTR64_CPU(ptr, dsc, i) \
	(*(u64 *)((char *)ptr + dsc[i].offset))
#define MLX5E_READ_CTR64_BE(ptr, dsc, i) \
	be64_to_cpu(*(__be64 *)((char *)ptr + dsc[i].offset))
#define MLX5E_READ_CTR32_CPU(ptr, dsc, i) \
	(*(u32 *)((char *)ptr + dsc[i].offset))
#define MLX5E_READ_CTR32_BE(ptr, dsc, i) \
	be32_to_cpu(*(__be32 *)((char *)ptr + dsc[i].offset))

#define MLX5E_DECLARE_STAT(type, fld) #fld, offsetof(type, fld)
#define MLX5E_DECLARE_RX_STAT(type, fld) "rx%d_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_TX_STAT(type, fld) "tx%d_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_XDPSQ_STAT(type, fld) "tx%d_xdp_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_RQ_XDPSQ_STAT(type, fld) "rx%d_xdp_tx_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_XSKRQ_STAT(type, fld) "rx%d_xsk_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_XSKSQ_STAT(type, fld) "tx%d_xsk_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_CH_STAT(type, fld) "ch%d_"#fld, offsetof(type, fld)

#define MLX5E_DECLARE_PTP_TX_STAT(type, fld) "ptp_tx%d_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_PTP_CH_STAT(type, fld) "ptp_ch_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_PTP_CQ_STAT(type, fld) "ptp_cq%d_"#fld, offsetof(type, fld)
#define MLX5E_DECLARE_PTP_RQ_STAT(type, fld) "ptp_rq0_"#fld, offsetof(type, fld)

#define MLX5E_DECLARE_QOS_TX_STAT(type, fld) "qos_tx%d_"#fld, offsetof(type, fld)

struct counter_desc {
	char		format[ETH_GSTRING_LEN];
	size_t		offset; /* Byte offset */
};

enum {
	MLX5E_NDO_UPDATE_STATS = BIT(0x1),
};

struct mlx5e_priv;
struct mlx5e_stats_grp {
	u16 update_stats_mask;
	int (*get_num_stats)(struct mlx5e_priv *priv);
	void (*fill_strings)(struct mlx5e_priv *priv, u8 **data);
	void (*fill_stats)(struct mlx5e_priv *priv, u64 **data);
	void (*update_stats)(struct mlx5e_priv *priv);
};

void mlx5e_ethtool_put_stat(u64 **data, u64 val);

typedef const struct mlx5e_stats_grp *const mlx5e_stats_grp_t;

#define MLX5E_STATS_GRP_OP(grp, name) mlx5e_stats_grp_ ## grp ## _ ## name

#define MLX5E_DECLARE_STATS_GRP_OP_NUM_STATS(grp) \
	int MLX5E_STATS_GRP_OP(grp, num_stats)(struct mlx5e_priv *priv)

#define MLX5E_DECLARE_STATS_GRP_OP_UPDATE_STATS(grp) \
	void MLX5E_STATS_GRP_OP(grp, update_stats)(struct mlx5e_priv *priv)

#define MLX5E_DECLARE_STATS_GRP_OP_FILL_STRS(grp) \
	void MLX5E_STATS_GRP_OP(grp, fill_strings)(struct mlx5e_priv *priv, u8 **data)

#define MLX5E_DECLARE_STATS_GRP_OP_FILL_STATS(grp) \
	void MLX5E_STATS_GRP_OP(grp, fill_stats)(struct mlx5e_priv *priv, u64 **data)

#define MLX5E_STATS_GRP(grp) mlx5e_stats_grp_ ## grp

#define MLX5E_DECLARE_STATS_GRP(grp) \
	const struct mlx5e_stats_grp MLX5E_STATS_GRP(grp)

#define MLX5E_DEFINE_STATS_GRP(grp, mask) \
MLX5E_DECLARE_STATS_GRP(grp) = { \
	.get_num_stats = MLX5E_STATS_GRP_OP(grp, num_stats), \
	.fill_stats    = MLX5E_STATS_GRP_OP(grp, fill_stats), \
	.fill_strings  = MLX5E_STATS_GRP_OP(grp, fill_strings), \
	.update_stats  = MLX5E_STATS_GRP_OP(grp, update_stats), \
	.update_stats_mask = mask, \
}

unsigned int mlx5e_stats_total_num(struct mlx5e_priv *priv);
void mlx5e_stats_update(struct mlx5e_priv *priv);
void mlx5e_stats_fill(struct mlx5e_priv *priv, u64 *data, int idx);
void mlx5e_stats_fill_strings(struct mlx5e_priv *priv, u8 *data);
void mlx5e_stats_update_ndo_stats(struct mlx5e_priv *priv);

void mlx5e_stats_pause_get(struct mlx5e_priv *priv,
			   struct ethtool_pause_stats *pause_stats);
void mlx5e_stats_fec_get(struct mlx5e_priv *priv,
			 struct ethtool_fec_stats *fec_stats);

void mlx5e_stats_eth_phy_get(struct mlx5e_priv *priv,
			     struct ethtool_eth_phy_stats *phy_stats);
void mlx5e_stats_eth_mac_get(struct mlx5e_priv *priv,
			     struct ethtool_eth_mac_stats *mac_stats);
void mlx5e_stats_eth_ctrl_get(struct mlx5e_priv *priv,
			      struct ethtool_eth_ctrl_stats *ctrl_stats);
void mlx5e_stats_rmon_get(struct mlx5e_priv *priv,
			  struct ethtool_rmon_stats *rmon,
			  const struct ethtool_rmon_hist_range **ranges);
void mlx5e_stats_ts_get(struct mlx5e_priv *priv,
			struct ethtool_ts_stats *ts_stats);
void mlx5e_get_link_ext_stats(struct net_device *dev,
			      struct ethtool_link_ext_stats *stats);

/* Concrete NIC Stats */

struct mlx5e_sw_stats {
	u64 rx_packets;
	u64 rx_bytes;
	u64 tx_packets;
	u64 tx_bytes;
	u64 tx_tso_packets;
	u64 tx_tso_bytes;
	u64 tx_tso_inner_packets;
	u64 tx_tso_inner_bytes;
	u64 tx_added_vlan_packets;
	u64 tx_nop;
	u64 tx_mpwqe_blks;
	u64 tx_mpwqe_pkts;
	u64 rx_lro_packets;
	u64 rx_lro_bytes;
	u64 rx_gro_packets;
	u64 rx_gro_bytes;
	u64 rx_gro_skbs;
	u64 rx_gro_large_hds;
	u64 rx_hds_nodata_packets;
	u64 rx_hds_nodata_bytes;
	u64 rx_hds_nosplit_packets;
	u64 rx_hds_nosplit_bytes;
	u64 rx_mcast_packets;
	u64 rx_ecn_mark;
	u64 rx_removed_vlan_packets;
	u64 rx_csum_unnecessary;
	u64 rx_csum_none;
	u64 rx_csum_complete;
	u64 rx_csum_complete_tail;
	u64 rx_csum_complete_tail_slow;
	u64 rx_csum_unnecessary_inner;
	u64 rx_xdp_drop;
	u64 rx_xdp_redirect;
	u64 rx_xdp_tx_xmit;
	u64 rx_xdp_tx_mpwqe;
	u64 rx_xdp_tx_inlnw;
	u64 rx_xdp_tx_nops;
	u64 rx_xdp_tx_full;
	u64 rx_xdp_tx_err;
	u64 rx_xdp_tx_cqe;
	u64 tx_csum_none;
	u64 tx_csum_partial;
	u64 tx_csum_partial_inner;
	u64 tx_queue_stopped;
	u64 tx_queue_dropped;
	u64 tx_xmit_more;
	u64 tx_recover;
	u64 tx_cqes;
	u64 tx_queue_wake;
	u64 tx_cqe_err;
	u64 tx_xdp_xmit;
	u64 tx_xdp_mpwqe;
	u64 tx_xdp_inlnw;
	u64 tx_xdp_nops;
	u64 tx_xdp_full;
	u64 tx_xdp_err;
	u64 tx_xdp_cqes;
	u64 rx_wqe_err;
	u64 rx_mpwqe_filler_cqes;
	u64 rx_mpwqe_filler_strides;
	u64 rx_oversize_pkts_sw_drop;
	u64 rx_buff_alloc_err;
	u64 rx_cqe_compress_blks;
	u64 rx_cqe_compress_pkts;
	u64 rx_congst_umr;
#ifdef CONFIG_MLX5_EN_ARFS
	u64 rx_arfs_add;
	u64 rx_arfs_request_in;
	u64 rx_arfs_request_out;
	u64 rx_arfs_expired;
	u64 rx_arfs_err;
#endif
	u64 rx_recover;
	u64 ch_events;
	u64 ch_poll;
	u64 ch_arm;
	u64 ch_aff_change;
	u64 ch_force_irq;
	u64 ch_eq_rearm;
	u64 rx_pp_alloc_fast;
	u64 rx_pp_alloc_slow;
	u64 rx_pp_alloc_slow_high_order;
	u64 rx_pp_alloc_empty;
	u64 rx_pp_alloc_refill;
	u64 rx_pp_alloc_waive;
	u64 rx_pp_recycle_cached;
	u64 rx_pp_recycle_cache_full;
	u64 rx_pp_recycle_ring;
	u64 rx_pp_recycle_ring_full;
	u64 rx_pp_recycle_released_ref;
#ifdef CONFIG_MLX5_EN_TLS
	u64 tx_tls_encrypted_packets;
	u64 tx_tls_encrypted_bytes;
	u64 tx_tls_ooo;
	u64 tx_tls_dump_packets;
	u64 tx_tls_dump_bytes;
	u64 tx_tls_resync_bytes;
	u64 tx_tls_skip_no_sync_data;
	u64 tx_tls_drop_no_sync_data;
	u64 tx_tls_drop_bypass_req;

	u64 rx_tls_decrypted_packets;
	u64 rx_tls_decrypted_bytes;
	u64 rx_tls_resync_req_pkt;
	u64 rx_tls_resync_req_start;
	u64 rx_tls_resync_req_end;
	u64 rx_tls_resync_req_skip;
	u64 rx_tls_resync_res_ok;
	u64 rx_tls_resync_res_retry;
	u64 rx_tls_resync_res_skip;
	u64 rx_tls_err;
#endif

	u64 rx_xsk_packets;
	u64 rx_xsk_bytes;
	u64 rx_xsk_csum_complete;
	u64 rx_xsk_csum_unnecessary;
	u64 rx_xsk_csum_unnecessary_inner;
	u64 rx_xsk_csum_none;
	u64 rx_xsk_ecn_mark;
	u64 rx_xsk_removed_vlan_packets;
	u64 rx_xsk_xdp_drop;
	u64 rx_xsk_xdp_redirect;
	u64 rx_xsk_wqe_err;
	u64 rx_xsk_mpwqe_filler_cqes;
	u64 rx_xsk_mpwqe_filler_strides;
	u64 rx_xsk_oversize_pkts_sw_drop;
	u64 rx_xsk_buff_alloc_err;
	u64 rx_xsk_cqe_compress_blks;
	u64 rx_xsk_cqe_compress_pkts;
	u64 rx_xsk_congst_umr;
	u64 tx_xsk_xmit;
	u64 tx_xsk_mpwqe;
	u64 tx_xsk_inlnw;
	u64 tx_xsk_full;
	u64 tx_xsk_err;
	u64 tx_xsk_cqes;
};

struct mlx5e_qcounter_stats {
	u32 rx_out_of_buffer;
	u32 rx_if_down_packets;
};

#define VNIC_ENV_GET(vnic_env_stats, c) \
	MLX5_GET(query_vnic_env_out, (vnic_env_stats)->query_vnic_env_out, \
		 vport_env.c)

struct mlx5e_vnic_env_stats {
	__be64 query_vnic_env_out[MLX5_ST_SZ_QW(query_vnic_env_out)];
};

#define VPORT_COUNTER_GET(vstats, c) MLX5_GET64(query_vport_counter_out, \
						vstats->query_vport_out, c)

struct mlx5e_vport_stats {
	__be64 query_vport_out[MLX5_ST_SZ_QW(query_vport_counter_out)];
};

#define PPORT_802_3_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, pstats->IEEE_802_3_counters, \
		   counter_set.eth_802_3_cntrs_grp_data_layout.c##_high)
#define PPORT_2863_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, pstats->RFC_2863_counters, \
		   counter_set.eth_2863_cntrs_grp_data_layout.c##_high)
#define PPORT_2819_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, pstats->RFC_2819_counters, \
		   counter_set.eth_2819_cntrs_grp_data_layout.c##_high)
#define PPORT_PHY_STATISTICAL_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, (pstats)->phy_statistical_counters, \
		   counter_set.phys_layer_statistical_cntrs.c##_high)
#define PPORT_PHY_RECOVERY_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, (pstats)->phy_recovery_counters, \
		   counter_set.phys_layer_recovery_cntrs.c)
#define PPORT_PER_PRIO_GET(pstats, prio, c) \
	MLX5_GET64(ppcnt_reg, pstats->per_prio_counters[prio], \
		   counter_set.eth_per_prio_grp_data_layout.c##_high)
#define NUM_PPORT_PRIO				8
#define PPORT_ETH_EXT_GET(pstats, c) \
	MLX5_GET64(ppcnt_reg, (pstats)->eth_ext_counters, \
		   counter_set.eth_extended_cntrs_grp_data_layout.c##_high)

struct mlx5e_pport_stats {
	__be64 IEEE_802_3_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 RFC_2863_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 RFC_2819_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 per_prio_counters[NUM_PPORT_PRIO][MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 phy_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 phy_statistical_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 phy_recovery_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 eth_ext_counters[MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 per_tc_prio_counters[NUM_PPORT_PRIO][MLX5_ST_SZ_QW(ppcnt_reg)];
	__be64 per_tc_congest_prio_counters[NUM_PPORT_PRIO][MLX5_ST_SZ_QW(ppcnt_reg)];
};

#define PCIE_PERF_GET(pcie_stats, c) \
	MLX5_GET(mpcnt_reg, (pcie_stats)->pcie_perf_counters, \
		 counter_set.pcie_perf_cntrs_grp_data_layout.c)

#define PCIE_PERF_GET64(pcie_stats, c) \
	MLX5_GET64(mpcnt_reg, (pcie_stats)->pcie_perf_counters, \
		   counter_set.pcie_perf_cntrs_grp_data_layout.c##_high)

struct mlx5e_pcie_stats {
	__be64 pcie_perf_counters[MLX5_ST_SZ_QW(mpcnt_reg)];
};

struct mlx5e_rq_stats {
	u64 packets;
	u64 bytes;
	u64 csum_complete;
	u64 csum_complete_tail;
	u64 csum_complete_tail_slow;
	u64 csum_unnecessary;
	u64 csum_unnecessary_inner;
	u64 csum_none;
	u64 lro_packets;
	u64 lro_bytes;
	u64 gro_packets;
	u64 gro_bytes;
	u64 gro_skbs;
	u64 gro_large_hds;
	u64 hds_nodata_packets;
	u64 hds_nodata_bytes;
	u64 hds_nosplit_packets;
	u64 hds_nosplit_bytes;
	u64 mcast_packets;
	u64 ecn_mark;
	u64 removed_vlan_packets;
	u64 xdp_drop;
	u64 xdp_redirect;
	u64 wqe_err;
	u64 mpwqe_filler_cqes;
	u64 mpwqe_filler_strides;
	u64 oversize_pkts_sw_drop;
	u64 buff_alloc_err;
	u64 cqe_compress_blks;
	u64 cqe_compress_pkts;
	u64 congst_umr;
#ifdef CONFIG_MLX5_EN_ARFS
	u64 arfs_add;
	u64 arfs_request_in;
	u64 arfs_request_out;
	u64 arfs_expired;
	u64 arfs_err;
#endif
	u64 recover;
	u64 pp_alloc_fast;
	u64 pp_alloc_slow;
	u64 pp_alloc_slow_high_order;
	u64 pp_alloc_empty;
	u64 pp_alloc_refill;
	u64 pp_alloc_waive;
	u64 pp_recycle_cached;
	u64 pp_recycle_cache_full;
	u64 pp_recycle_ring;
	u64 pp_recycle_ring_full;
	u64 pp_recycle_released_ref;
#ifdef CONFIG_MLX5_EN_TLS
	u64 tls_decrypted_packets;
	u64 tls_decrypted_bytes;
	u64 tls_resync_req_pkt;
	u64 tls_resync_req_start;
	u64 tls_resync_req_end;
	u64 tls_resync_req_skip;
	u64 tls_resync_res_ok;
	u64 tls_resync_res_retry;
	u64 tls_resync_res_skip;
	u64 tls_err;
#endif
};

struct mlx5e_sq_stats {
	/* commonly accessed in data path */
	u64 packets;
	u64 bytes;
	u64 xmit_more;
	u64 tso_packets;
	u64 tso_bytes;
	u64 tso_inner_packets;
	u64 tso_inner_bytes;
	u64 csum_partial;
	u64 csum_partial_inner;
	u64 added_vlan_packets;
	u64 nop;
	u64 mpwqe_blks;
	u64 mpwqe_pkts;
#ifdef CONFIG_MLX5_EN_TLS
	u64 tls_encrypted_packets;
	u64 tls_encrypted_bytes;
	u64 tls_ooo;
	u64 tls_dump_packets;
	u64 tls_dump_bytes;
	u64 tls_resync_bytes;
	u64 tls_skip_no_sync_data;
	u64 tls_drop_no_sync_data;
	u64 tls_drop_bypass_req;
#endif
	/* less likely accessed in data path */
	u64 csum_none;
	u64 stopped;
	u64 dropped;
	u64 recover;
	u64 timestamps;
	/* dirtied @completion */
	u64 cqes ____cacheline_aligned_in_smp;
	u64 wake;
	u64 cqe_err;
};

struct mlx5e_xdpsq_stats {
	u64 xmit;
	u64 mpwqe;
	u64 inlnw;
	u64 nops;
	u64 full;
	u64 err;
	/* dirtied @completion */
	u64 cqes ____cacheline_aligned_in_smp;
};

struct mlx5e_ch_stats {
	u64 events;
	u64 poll;
	u64 arm;
	u64 aff_change;
	u64 force_irq;
	u64 eq_rearm;
};

struct mlx5e_ptp_cq_stats {
	u64 cqe;
	u64 err_cqe;
	u64 abort;
	u64 abort_abs_diff_ns;
	u64 late_cqe;
	u64 lost_cqe;
};

struct mlx5e_rep_stats {
	u64 vport_rx_packets;
	u64 vport_tx_packets;
	u64 vport_rx_bytes;
	u64 vport_tx_bytes;
	u64 rx_vport_rdma_unicast_packets;
	u64 tx_vport_rdma_unicast_packets;
	u64 rx_vport_rdma_unicast_bytes;
	u64 tx_vport_rdma_unicast_bytes;
	u64 rx_vport_rdma_multicast_packets;
	u64 tx_vport_rdma_multicast_packets;
	u64 rx_vport_rdma_multicast_bytes;
	u64 tx_vport_rdma_multicast_bytes;
	u64 vport_loopback_packets;
	u64 vport_loopback_bytes;
	u64 rx_vport_out_of_buffer;
};

struct mlx5e_stats {
	struct mlx5e_sw_stats sw;
	struct mlx5e_qcounter_stats qcnt;
	struct mlx5e_vnic_env_stats vnic;
	struct mlx5e_vport_stats vport;
	struct mlx5e_pport_stats pport;
	struct mlx5e_pcie_stats pcie;
	struct mlx5e_rep_stats rep_stats;
};

static inline void mlx5e_stats_copy_rep_stats(struct rtnl_link_stats64 *vf_vport,
					      struct mlx5e_rep_stats *rep_stats)
{
	memset(vf_vport, 0, sizeof(*vf_vport));
	vf_vport->rx_packets = rep_stats->vport_rx_packets;
	vf_vport->tx_packets = rep_stats->vport_tx_packets;
	vf_vport->rx_bytes = rep_stats->vport_rx_bytes;
	vf_vport->tx_bytes = rep_stats->vport_tx_bytes;
	vf_vport->rx_missed_errors = rep_stats->rx_vport_out_of_buffer;
}

extern mlx5e_stats_grp_t mlx5e_nic_stats_grps[];
unsigned int mlx5e_nic_stats_grps_num(struct mlx5e_priv *priv);

extern MLX5E_DECLARE_STATS_GRP(sw);
extern MLX5E_DECLARE_STATS_GRP(qcnt);
extern MLX5E_DECLARE_STATS_GRP(vnic_env);
extern MLX5E_DECLARE_STATS_GRP(vport);
extern MLX5E_DECLARE_STATS_GRP(802_3);
extern MLX5E_DECLARE_STATS_GRP(2863);
extern MLX5E_DECLARE_STATS_GRP(2819);
extern MLX5E_DECLARE_STATS_GRP(phy);
extern MLX5E_DECLARE_STATS_GRP(eth_ext);
extern MLX5E_DECLARE_STATS_GRP(pcie);
extern MLX5E_DECLARE_STATS_GRP(per_prio);
extern MLX5E_DECLARE_STATS_GRP(pme);
extern MLX5E_DECLARE_STATS_GRP(channels);
extern MLX5E_DECLARE_STATS_GRP(per_port_buff_congest);
extern MLX5E_DECLARE_STATS_GRP(ipsec_hw);
extern MLX5E_DECLARE_STATS_GRP(ipsec_sw);
extern MLX5E_DECLARE_STATS_GRP(ptp);
extern MLX5E_DECLARE_STATS_GRP(macsec_hw);
extern MLX5E_DECLARE_STATS_GRP(pcie_cong);

#endif /* __MLX5_EN_STATS_H__ */
