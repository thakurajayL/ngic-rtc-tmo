/*
 * Copyright (c) 2020 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include <sched.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include <rte_string_fns.h>
#include <rte_ring.h>
#include <rte_lcore.h>
#include <rte_ethdev.h>
#include <rte_port_ring.h>
#include <rte_port_ethdev.h>
#include <rte_table_hash.h>
#include <rte_table_stub.h>
#include <rte_byteorder.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_jhash.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_udp.h>
#include <rte_mbuf.h>
#include <rte_hash_crc.h>
#include <rte_port_ring.h>
#include <rte_kni.h>
#include <rte_arp.h>

#include "ngic_rtc_framework.h"
#include "mngtplane_handler.h"
#include "main.h"
#include "gtpu.h"
#ifdef UNIT_TEST
#include "pkt_proc.h"
#endif

/* Generate new pcap for s1u port */
#ifdef PCAP_GEN
extern pcap_dumper_t *pcap_dumper_west;
#endif /* PCAP_GEN */

extern struct kni_port_params *kni_port_params_array[RTE_MAX_ETHPORTS];
#ifdef PERF_ANALYSIS
#include "perf_timer.h"
extern _timer_t _init_time;
#endif /* PERF_ANALYSIS */

static inline void ul_set_flow_id(struct rte_mbuf *m)
{
	uint8_t *m_data = rte_pktmbuf_mtod(m, uint8_t *);
	struct ipv4_hdr *ext_ipv4_hdr =
		(struct ipv4_hdr *)&m_data[sizeof(struct ether_hdr)];
	struct udp_hdr *udph = NULL;
	uint32_t ip_len;
	struct ether_hdr *eth_h = (struct ether_hdr *)&m_data[0];
	/* Host Order ext_ipv4_hdr->dst_addr */
	uint32_t ho_addr;

	/* Flag BAD Checksum packets */
	if (unlikely(
			 (m->ol_flags & PKT_RX_IP_CKSUM_MASK) == PKT_RX_IP_CKSUM_BAD ||
			 (m->ol_flags & PKT_RX_L4_CKSUM_MASK) == PKT_RX_L4_CKSUM_BAD)) {
		RTE_LOG_DP(ERR, DP, "UL Bad checksum: %lu\n", m->ol_flags);
		s1u_pktyp = BAD_PKT;
		return;
	}

	/* Check if IPv4 packet */
	if (likely(eth_h->ether_type == htons(ETHER_TYPE_IPv4))) {
		/* Flag fragmented packets:
		 * check for unfragmented packets and packets with don't fragment
		 * bit set (0x40) */
		if(unlikely(ext_ipv4_hdr->fragment_offset != 0 &&
					ext_ipv4_hdr->fragment_offset != 64))
		{
			s1u_pktyp = JUMBO_PKT;
			return;
		}

		ho_addr = ntohl(ext_ipv4_hdr->dst_addr);
		/* Flag pkt destined to S1U_IP */
		if (likely(app.s1u_ip == ext_ipv4_hdr->dst_addr)) {
			RTE_LOG_DP(DEBUG, DP, "epc_ul.c:%s::"
					"\n\t@S1U:app.s1u_ip==ext_ipv4_hdr->dst_addr= %s\n",
					__func__,
					inet_ntoa(*(struct in_addr *)&ho_addr));

			/* Check UDP packet */
			if (unlikely(ext_ipv4_hdr->next_proto_id != IPPROTO_UDP))
			{
				s1u_pktyp = KNI_PKT;
				return;
			}

			/* UDP packet */
			/* Calculate IP length */
			ip_len = (ext_ipv4_hdr->version_ihl & 0xf) << 2;
			/* Get hold of UDP header */
			udph = (struct udp_hdr *)&m_data[sizeof(struct ether_hdr) + ip_len];
			/* Check UDP PORT == GTPU_PORT */
			if (unlikely(udph->dst_port != UDP_PORT_GTPU_NW_ORDER))
			{
				s1u_pktyp = KNI_PKT;
				return;
			}
			/* GTP PKT == GTPU data | ECHO | UNSUPPORTED */
			struct gtpu_hdr *gtpuhdr = get_mtogtpu(m);
			if (likely(gtpuhdr->msgtype == GTP_GPDU))
			{
				RTE_LOG_DP(DEBUG, DP, "UL: GTPU packet\n");
				s1u_pktyp = GTPU_PKT;
				return;
			}
			if (likely(gtpuhdr->msgtype == GTPU_ECHO_REQUEST))
			{
				RTE_LOG_DP(DEBUG, DP, "UL: GTPU ECHO packet\n");
				s1u_pktyp = GTPU_ECHO_REQ;
				return;
			}
			RTE_LOG_DP(DEBUG, DP, "UL: GTP UNSUPPORTED packet\n");
			s1u_pktyp = GTPU_UNSUPPORTED;
			return;
		}

		/* Flag MCAST pkt for linux handling */
		if (IS_IPV4_MCAST(ho_addr))
		{
			RTE_LOG_DP(DEBUG, DP, "epc_ul.c:%s::"
					"\n\t@S1U:IPV$_MCAST==ext_ipv4_hdr->dst_addr= %s\n",
					__func__,
					inet_ntoa(*(struct in_addr *)&ho_addr));
			s1u_pktyp = KNI_PKT;
			return;
		}

		/* Flag BCAST pkt for linux handling */
		if (likely(app.s1u_bcast_addr == ext_ipv4_hdr->dst_addr))
		{
			RTE_LOG_DP(DEBUG, DP, "epc_ul.c:%s::"
					"\n\t@S1U:app.s1u_bcast_addr==ext_ipv4_hdr->dst_addr= %s\n",
					__func__,
					inet_ntoa(*(struct in_addr *)&ho_addr));
			s1u_pktyp = KNI_PKT;
			return;
		}
		s1u_pktyp = UNKNOWN_PKT;
		return;
	} /* IPv4 packet */

	/* Flag packets destined to UL interface */
	if ((is_same_ether_addr(&eth_h->d_addr, &app.s1u_ether_addr)) ||
		(is_multicast_ether_addr(&eth_h->d_addr)) ||
		(is_universal_ether_addr(&eth_h->d_addr)) ||
		(is_broadcast_ether_addr(&eth_h->d_addr))) {
			s1u_pktyp = KNI_PKT;
			return;
	}
	s1u_pktyp = UNKNOWN_PKT;
}

#ifdef FRAG
struct rte_mbuf *
ip_reassemble(struct rte_mbuf *m)
{
	struct ether_hdr *eth_hdr;
	struct rte_ip_frag_tbl *tbl;
	struct rte_ip_frag_death_row *dr;

	/* if packet is IPv4 */
	if (RTE_ETH_IS_IPV4_HDR(m->packet_type)) {
		struct ipv4_hdr *ip_hdr;

		eth_hdr = rte_pktmbuf_mtod(m, struct ether_hdr *);
		ip_hdr = (struct ipv4_hdr *)(eth_hdr + 1);

		/* if it is a fragmented packet, then try ot reassemble */
		if (rte_ipv4_frag_pkt_is_fragmented(ip_hdr)) {
			struct rte_mbuf *mo;

			tbl = s1u_frag_tbl;
			dr = &s1u_death_row;

			/* prepare mbuf: setup l2_len/l3_len */
			m->l2_len = sizeof(*eth_hdr);
			m->l3_len = sizeof(*ip_hdr);

			/* process this fragment */
			mo = rte_ipv4_frag_reassemble_packet(tbl, dr, m, s1u_cur_tsc, ip_hdr);
			if (mo == NULL)
				/* no packet to process just yet */
				m = NULL;

			/* we have our packet reassembled */
			if (mo != m) {
				/* move mbuf data in the first segment */
				if (rte_pktmbuf_linearize(mo) == 0)
					m = mo;
				else
					rte_panic("Failed to linearize rte_mbuf. "
						  "Is there enough tail room?\n");
			}
		}
	}

	return m;
}
#endif /* FRAG */

static ul_handler ul_pkt_handler[NUM_SPGW_PORTS];
/**
 * UL ngic input action handler function
 * @param pkts
 *   The address of an array of pointers to *rte_mbuf* structures that
 *   must be large enough to store *n* pointers in it.
 * @param n
 *   The maximum number of packets to retrieve.
 * @param pkts_mask
 *   pointer to pkts_mask for received burst
 * @param data_pkts
 *   The address of an array of pointers to *rte_mbuf* data packets
 * @param dpkts_mask
 *   pointer to data pkts mask in received burst
 * @param pid
 *   port_id of function called
 * @return
 *   number of data pkts
 */
static uint32_t ul_in_ah(struct rte_mbuf **pkts,
		uint32_t n, uint64_t *pkts_mask,
		struct rte_mbuf **data_pkts, uint64_t *dpkts_mask,
		int8_t pid)
{
#ifdef PERF_ANALYSIS
	TIMER_GET_CURRENT_TP(_init_time);
#endif /* PERF_ANALYSIS */

	uint32_t i, j;
	uint32_t nb_data_pkts = 0;

#ifdef FRAG
	/* retire outdated frags (if needed) */
	s1u_cur_tsc = rte_rdtsc();
	rte_ip_frag_free_death_row(&s1u_death_row, PREFETCH_OFFSET);
#endif /* FRAG */

	for (i = 0, j=0; i < n; i++) {
		struct rte_mbuf *m = pkts[i];
#ifdef FRAG
		/* if pkt is fragmented, then wait for reassembly */
		m = ip_reassemble(m);
		if (m == NULL) continue;
#endif /* FRAG */

		ul_set_flow_id(m);
		switch (s1u_pktyp) {
			case GTPU_PKT:
				nb_data_pkts++;
				data_pkts[j] = m;
				j++;
				break;
#ifdef STATIC_ARP
			case GTPU_ECHO_REQ:
				RESET_BIT(*pkts_mask, i);
				mngt_ingress(pkts[i], pid);
				/* Update GTP_ECHO alloc UL count */
				epc_app.ul_params[pid].ul_mbuf_rtime.gtp_echo++;
				break;
#else /* !STATIC_ARP == KNI */
			case GTPU_ECHO_REQ:
				RESET_BIT(*pkts_mask, i);
				mngt_ingress(pkts[i], pid);
				/* Update GTP_ECHO alloc UL count */
				epc_app.ul_params[pid].ul_mbuf_rtime.gtp_echo++;
				break;
			case KNI_PKT:
				RESET_BIT(*pkts_mask, i);
				RTE_LOG(DEBUG, DP, "KNI: UL send pkts to kni\n");
				kni_ingress(kni_port_params_array[pid], pid,
						&pkts[i], 1);
				/* Update KNI alloc UL count */
				epc_app.ul_params[pid].ul_mbuf_rtime.kni++;
				break;
#endif /* STATIC_ARP */
			/* RESET_BIT::
			 * STATIC_ARP: KNI_PKT | GTPU_UNSUPPORTED | BAD_PKT | UNKNOWN_PKT
			 * !STATIC_ARP: GTPU_UNSUPPORTED | BAD_PKT | UNKNOWN_PKT
			 * */
			default:
				RESET_BIT(*pkts_mask, i);
				/* Update BAD_PKT alloc UL count */
				epc_app.ul_params[pid].ul_mbuf_rtime.bad_pkt++;
				RTE_LOG(DEBUG, DP, "s1u_pktyp::"
						"\n\tGTPU_UNSUPPORTED | BAD_PKT | UNKNOWN_PKT\n");
		}
	}

	/* Update UL fastpath packets count */
	epc_app.ul_params[S1U_PORT_ID].pkts_in += nb_data_pkts;
	/* Update GTPU alloc UL mbuf count */
	epc_app.ul_params[pid].ul_mbuf_rtime.gtpu += nb_data_pkts;

/* Capture packets on s1u_port.*/
#ifdef PCAP_GEN
	dump_pcap(pkts, n, pcap_dumper_west);
#endif /* PCAP_GEN */

	if (nb_data_pkts) {
		*dpkts_mask = (~0LLU) >> (64 - nb_data_pkts);
		ul_handler f = ul_pkt_handler[pid];
		f(data_pkts, nb_data_pkts, dpkts_mask);
	}

#ifdef PERF_ANALYSIS
	ul_stat_info.port_in_out_delta = TIMER_GET_ELAPSED_NS(_init_time);
#ifdef TIMER_STATS
	/* Export stats into file. */
	ul_timer_stats(nb_data_pkts, &ul_stat_info);
#endif /* TIMER_STATS */

	/* calculate min time, max time, min_burst_sz, max_burst_sz
	 * perf_stats.op_time[12] = port_in_out_time */
	SET_PERF_MAX_MIN_TIME(ul_perf_stats.op_time[12], _init_time, nb_data_pkts, 0);
#endif /* PERF_ANALYSIS */

	return nb_data_pkts;
}

/**
 * UL ngic_rtc function
 *
 * @param arg
 *	Argument to ngic_rtc function
 * @param ip_op
 *	Input-Outport[ports, queues] for function called
 */
void epc_ul(void *args, port_pairs_t ip_op)
{
	uint16_t i, nb_ulrx, nb_ultx, nb_sent, nb_burst;
	uint32_t nb_data_pkts = 0;
	struct rte_mbuf *ul_procmbuf[PKT_BURST_SZ] = {NULL};
	struct rte_mbuf *data_pkts[PKT_BURST_SZ] = {NULL};
	uint64_t pkts_mask = 0, dpkts_mask =0;

	/* rte_eth_rx_burst(uint16_t port_id, uint16_t queue_id,
			 struct rte_mbuf **rx_pkts, const uint16_t nb_pkts)::
	 * - Allocate & Initialize *rte_mbuf* data structure associated w/ RX Descriptors
	 * - Store *rte_mbuf* into the next entry of *rx_pkts* array.
	 * - Return number of packets actually retrieved
	 * - Replenish RX descriptor w/ new *rte_mbuf* buffer
	 *	 allocated from memory pool associated w/ receive queue by init.c
	 */
	nb_ulrx = rte_eth_rx_burst(ip_op.in_pid, ip_op.in_qid,
							ul_procmbuf, PKT_BURST_SZ);

	if (nb_ulrx > 0) {
#ifdef UNIT_TEST
		/* Manipulate mixed packets */
		create_mixed_bursts(ul_procmbuf, nb_ulrx, ip_op.in_pid);
#endif
		pkts_mask = (~0LLU) >> (64 - nb_ulrx);
		/* Update total RX-ALLOC UL mbuf count */
		epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.rx_alloc += nb_ulrx;
		nb_data_pkts=
			ul_in_ah(ul_procmbuf, nb_ulrx, &pkts_mask,
					data_pkts, &dpkts_mask, ip_op.in_pid);

		/* rte_eth_tx_burst(uint16_t port_id, uint16_t queue_id,
				 struct rte_mbuf **tx_pkts, uint16_t nb_pkts)::
		 * - Initialize *rte_mbuf* data structure associated w/ TX Descriptors
		 * - *tx_pkts* allocated from memory pool associated w/ receive queue by init.c
		 * - Free the network buffer previously sent with that descriptor, if any.
		 * - Return number of packets actually sent.
		 * - Transparently free memory buffers of packets sent based on *tx_free_thresh*
		 */
		nb_sent = 0;
		while (nb_sent < nb_data_pkts) {
			/* Find the position of the first fastpath pkt to be freed.
			 * i.e find the first invalid fastpath packet */
			nb_burst = __builtin_ffsl(~dpkts_mask);

			/* If all packets are marked to be sent in dpkts_mask, i.e no
			 * intermediate pkts in the fastpath burst are marked to be freed */
			if ((nb_burst == 0) || (nb_burst > nb_data_pkts)) {
				nb_ultx = rte_eth_tx_burst(ip_op.out_pid, ip_op.out_qid,
								&data_pkts[nb_sent], nb_data_pkts);
				/* Update TX+FREE UL mbuf count */
				epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free += nb_ultx;
				for (i = nb_ultx; i < nb_data_pkts; i++) {
					rte_pktmbuf_free(data_pkts[i]);
					/* Update TX+FREE UL mbuf count */
					epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free++;
				}
				if (nb_ultx < nb_data_pkts) {
					printf("ASR- Probe::%s::"
						"\n\teth_tx descriptors/tx_ring full!!!"
						"\n\tnb_ulrx= %u; nb_ultx= %u\n",
						__func__, nb_data_pkts, nb_ultx);
				}
				break;
			}

			/* If fastpath packets are marked to be sent, but there is atleast one
			 * intermediate pkt in the fastpath burst marked to be freed in dpkts_mask */
			if ((nb_burst > 1) && (nb_burst <= nb_data_pkts)) {
				nb_ultx = rte_eth_tx_burst(ip_op.out_pid, ip_op.out_qid,
								&data_pkts[nb_sent], nb_burst - 1);
				/* Update TX+FREE UL mbuf count */
				epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free += nb_ultx;
				for (i = nb_ultx; i < (nb_burst -1); i++) {
					rte_pktmbuf_free(data_pkts[i]);
					/* Update TX+FREE UL mbuf count */
					epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free++;
				}
				if (nb_ultx < (nb_burst - 1)) {
					printf("ASR- Probe::%s::"
						"\n\teth_tx descriptors/tx_ring full!!!"
						"\n\tnb_ulrx= %u; nb_ultx= %u\n",
						__func__, (nb_burst - 1), nb_ultx);
				}
			}

			nb_sent += nb_burst;

			/* Free intermediate pkts in the fastpath burst marked
			 * to be freed in dpkts_mask */
			if (nb_sent <= nb_data_pkts) {
				rte_pktmbuf_free(data_pkts[nb_sent - 1]);
				/* Update TX+FREE UL mbuf count */
				epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free++;
				dpkts_mask >>= nb_burst ;
			}
		}

		/* Free all non-fastpath packets */
		for (i = 0; i < nb_ulrx; i++) {
			if (ISSET_BIT(pkts_mask, i))
				continue;
			rte_pktmbuf_free(ul_procmbuf[i]);
		}
	}

#ifndef STATIC_ARP
	/** Handle the request mbufs sent from kernel space,
	 *  Then analysis it and calls the specific actions for the specific requests.
	 *  Finally constructs the response mbuf and puts it back to the resp_q.
	 */
	struct rte_mbuf *pkt_rxburst[PKT_BURST_SZ] = {NULL};
	rte_kni_handle_request(kni_port_params_array[ip_op.in_pid]->kni[0]);

	uint16_t pkt_rx = kni_egress(kni_port_params_array[ip_op.out_pid], pkt_rxburst);
#ifdef PCAP_GEN
	dump_pcap(pkt_rxburst, pkt_rx, pcap_dumper_west);
#endif /* PCAP_GEN */
	uint16_t pkt_tx = rte_eth_tx_burst(ip_op.out_pid, ip_op.out_qid, pkt_rxburst, pkt_rx);
	/* Update TX+FREE UL mbuf count */
	epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free += pkt_tx;
	for (i = pkt_tx; i < pkt_rx; i++) {
		rte_pktmbuf_free(pkt_rxburst[i]);
		/* Update TX+FREE UL mbuf count */
		epc_app.ul_params[ip_op.in_pid].ul_mbuf_rtime.tx_free++;
	}
	if (pkt_tx < pkt_rx) {
		printf("ASR- Probe::%s::"
				"\n\teth_tx descriptors/tx_ring full!!!"
				"\n\tpkt_rx= %u; pkt_tx= %u\n",
				__func__, pkt_rx, pkt_tx);
	}
#endif /* !STATIC_ARP */
}

void register_ul_worker(ul_handler f, int port)
{
	ul_pkt_handler[port] = f;
}

