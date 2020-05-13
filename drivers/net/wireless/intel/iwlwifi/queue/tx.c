/******************************************************************************
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2020 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2020 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  * Neither the name Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/
#include <net/tso.h>
#include <linux/tcp.h>

#include "iwl-debug.h"
#include "iwl-io.h"
#include "fw/api/tx.h"
#include "queue/tx.h"
#include "iwl-fh.h"
#include "iwl-scd.h"
#include <linux/dmapool.h>

/*
 * iwl_txq_gen2_tx_stop - Stop all Tx DMA channels
 */
void iwl_txq_gen2_tx_stop(struct iwl_trans *trans)
{
	int txq_id;

	/*
	 * This function can be called before the op_mode disabled the
	 * queues. This happens when we have an rfkill interrupt.
	 * Since we stop Tx altogether - mark the queues as stopped.
	 */
	memset(trans->txqs.queue_stopped, 0,
	       sizeof(trans->txqs.queue_stopped));
	memset(trans->txqs.queue_used, 0, sizeof(trans->txqs.queue_used));

	/* Unmap DMA from host system and free skb's */
	for (txq_id = 0; txq_id < ARRAY_SIZE(trans->txqs.txq); txq_id++) {
		if (!trans->txqs.txq[txq_id])
			continue;
		iwl_txq_gen2_unmap(trans, txq_id);
	}
}

/*
 * iwl_txq_update_byte_tbl - Set up entry in Tx byte-count array
 */
static void iwl_pcie_gen2_update_byte_tbl(struct iwl_trans *trans,
					  struct iwl_txq *txq, u16 byte_cnt,
					  int num_tbs)
{
	int idx = iwl_txq_get_cmd_index(txq, txq->write_ptr);
	u8 filled_tfd_size, num_fetch_chunks;
	u16 len = byte_cnt;
	__le16 bc_ent;

	if (WARN(idx >= txq->n_window, "%d >= %d\n", idx, txq->n_window))
		return;

	filled_tfd_size = offsetof(struct iwl_tfh_tfd, tbs) +
			  num_tbs * sizeof(struct iwl_tfh_tb);
	/*
	 * filled_tfd_size contains the number of filled bytes in the TFD.
	 * Dividing it by 64 will give the number of chunks to fetch
	 * to SRAM- 0 for one chunk, 1 for 2 and so on.
	 * If, for example, TFD contains only 3 TBs then 32 bytes
	 * of the TFD are used, and only one chunk of 64 bytes should
	 * be fetched
	 */
	num_fetch_chunks = DIV_ROUND_UP(filled_tfd_size, 64) - 1;

	if (trans->trans_cfg->device_family >= IWL_DEVICE_FAMILY_AX210) {
		struct iwl_gen3_bc_tbl *scd_bc_tbl_gen3 = txq->bc_tbl.addr;

		/* Starting from AX210, the HW expects bytes */
		WARN_ON(trans->txqs.bc_table_dword);
		WARN_ON(len > 0x3FFF);
		bc_ent = cpu_to_le16(len | (num_fetch_chunks << 14));
		scd_bc_tbl_gen3->tfd_offset[idx] = bc_ent;
	} else {
		struct iwlagn_scd_bc_tbl *scd_bc_tbl = txq->bc_tbl.addr;

		/* Before AX210, the HW expects DW */
		WARN_ON(!trans->txqs.bc_table_dword);
		len = DIV_ROUND_UP(len, 4);
		WARN_ON(len > 0xFFF);
		bc_ent = cpu_to_le16(len | (num_fetch_chunks << 12));
		scd_bc_tbl->tfd_offset[idx] = bc_ent;
	}
}

/*
 * iwl_txq_inc_wr_ptr - Send new write index to hardware
 */
void iwl_txq_inc_wr_ptr(struct iwl_trans *trans, struct iwl_txq *txq)
{
	lockdep_assert_held(&txq->lock);

	IWL_DEBUG_TX(trans, "Q:%d WR: 0x%x\n", txq->id, txq->write_ptr);

	/*
	 * if not in power-save mode, uCode will never sleep when we're
	 * trying to tx (during RFKILL, we're not trying to tx).
	 */
	iwl_write32(trans, HBUS_TARG_WRPTR, txq->write_ptr | (txq->id << 16));
}

static u8 iwl_txq_gen2_get_num_tbs(struct iwl_trans *trans,
				   struct iwl_tfh_tfd *tfd)
{
	return le16_to_cpu(tfd->num_tbs) & 0x1f;
}

void iwl_txq_gen2_tfd_unmap(struct iwl_trans *trans, struct iwl_cmd_meta *meta,
			    struct iwl_tfh_tfd *tfd)
{
	int i, num_tbs;

	/* Sanity check on number of chunks */
	num_tbs = iwl_txq_gen2_get_num_tbs(trans, tfd);

	if (num_tbs > trans->txqs.tfd.max_tbs) {
		IWL_ERR(trans, "Too many chunks: %i\n", num_tbs);
		return;
	}

	/* first TB is never freed - it's the bidirectional DMA data */
	for (i = 1; i < num_tbs; i++) {
		if (meta->tbs & BIT(i))
			dma_unmap_page(trans->dev,
				       le64_to_cpu(tfd->tbs[i].addr),
				       le16_to_cpu(tfd->tbs[i].tb_len),
				       DMA_TO_DEVICE);
		else
			dma_unmap_single(trans->dev,
					 le64_to_cpu(tfd->tbs[i].addr),
					 le16_to_cpu(tfd->tbs[i].tb_len),
					 DMA_TO_DEVICE);
	}

	tfd->num_tbs = 0;
}

void iwl_txq_gen2_free_tfd(struct iwl_trans *trans, struct iwl_txq *txq)
{
	/* rd_ptr is bounded by TFD_QUEUE_SIZE_MAX and
	 * idx is bounded by n_window
	 */
	int idx = iwl_txq_get_cmd_index(txq, txq->read_ptr);

	lockdep_assert_held(&txq->lock);

	iwl_txq_gen2_tfd_unmap(trans, &txq->entries[idx].meta,
			       iwl_txq_get_tfd(trans, txq, idx));

	/* free SKB */
	if (txq->entries) {
		struct sk_buff *skb;

		skb = txq->entries[idx].skb;

		/* Can be called from irqs-disabled context
		 * If skb is not NULL, it means that the whole queue is being
		 * freed and that the queue is not empty - free the skb
		 */
		if (skb) {
			iwl_op_mode_free_skb(trans->op_mode, skb);
			txq->entries[idx].skb = NULL;
		}
	}
}

int iwl_txq_gen2_set_tb(struct iwl_trans *trans, struct iwl_tfh_tfd *tfd,
			dma_addr_t addr, u16 len)
{
	int idx = iwl_txq_gen2_get_num_tbs(trans, tfd);
	struct iwl_tfh_tb *tb;

	/*
	 * Only WARN here so we know about the issue, but we mess up our
	 * unmap path because not every place currently checks for errors
	 * returned from this function - it can only return an error if
	 * there's no more space, and so when we know there is enough we
	 * don't always check ...
	 */
	WARN(iwl_txq_crosses_4g_boundary(addr, len),
	     "possible DMA problem with iova:0x%llx, len:%d\n",
	     (unsigned long long)addr, len);

	if (WARN_ON(idx >= IWL_TFH_NUM_TBS))
		return -EINVAL;
	tb = &tfd->tbs[idx];

	/* Each TFD can point to a maximum max_tbs Tx buffers */
	if (le16_to_cpu(tfd->num_tbs) >= trans->txqs.tfd.max_tbs) {
		IWL_ERR(trans, "Error can not send more than %d chunks\n",
			trans->txqs.tfd.max_tbs);
		return -EINVAL;
	}

	put_unaligned_le64(addr, &tb->addr);
	tb->tb_len = cpu_to_le16(len);

	tfd->num_tbs = cpu_to_le16(idx + 1);

	return idx;
}

static struct page *get_workaround_page(struct iwl_trans *trans,
					struct sk_buff *skb)
{
	struct page **page_ptr;
	struct page *ret;

	page_ptr = (void *)((u8 *)skb->cb + trans->txqs.page_offs);

	ret = alloc_page(GFP_ATOMIC);
	if (!ret)
		return NULL;

	/* set the chaining pointer to the previous page if there */
	*(void **)(page_address(ret) + PAGE_SIZE - sizeof(void *)) = *page_ptr;
	*page_ptr = ret;

	return ret;
}

/*
 * Add a TB and if needed apply the FH HW bug workaround;
 * meta != NULL indicates that it's a page mapping and we
 * need to dma_unmap_page() and set the meta->tbs bit in
 * this case.
 */
static int iwl_txq_gen2_set_tb_with_wa(struct iwl_trans *trans,
				       struct sk_buff *skb,
				       struct iwl_tfh_tfd *tfd,
				       dma_addr_t phys, void *virt,
				       u16 len, struct iwl_cmd_meta *meta)
{
	dma_addr_t oldphys = phys;
	struct page *page;
	int ret;

	if (unlikely(dma_mapping_error(trans->dev, phys)))
		return -ENOMEM;

	if (likely(!iwl_txq_crosses_4g_boundary(phys, len))) {
		ret = iwl_txq_gen2_set_tb(trans, tfd, phys, len);

		if (ret < 0)
			goto unmap;

		if (meta)
			meta->tbs |= BIT(ret);

		ret = 0;
		goto trace;
	}

	/*
	 * Work around a hardware bug. If (as expressed in the
	 * condition above) the TB ends on a 32-bit boundary,
	 * then the next TB may be accessed with the wrong
	 * address.
	 * To work around it, copy the data elsewhere and make
	 * a new mapping for it so the device will not fail.
	 */

	if (WARN_ON(len > PAGE_SIZE - sizeof(void *))) {
		ret = -ENOBUFS;
		goto unmap;
	}

	page = get_workaround_page(trans, skb);
	if (!page) {
		ret = -ENOMEM;
		goto unmap;
	}

	memcpy(page_address(page), virt, len);

	phys = dma_map_single(trans->dev, page_address(page), len,
			      DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(trans->dev, phys)))
		return -ENOMEM;
	ret = iwl_txq_gen2_set_tb(trans, tfd, phys, len);
	if (ret < 0) {
		/* unmap the new allocation as single */
		oldphys = phys;
		meta = NULL;
		goto unmap;
	}
	IWL_WARN(trans,
		 "TB bug workaround: copied %d bytes from 0x%llx to 0x%llx\n",
		 len, (unsigned long long)oldphys, (unsigned long long)phys);

	ret = 0;
unmap:
	if (meta)
		dma_unmap_page(trans->dev, oldphys, len, DMA_TO_DEVICE);
	else
		dma_unmap_single(trans->dev, oldphys, len, DMA_TO_DEVICE);
trace:
	trace_iwlwifi_dev_tx_tb(trans->dev, skb, virt, phys, len);

	return ret;
}

#ifdef CONFIG_INET
struct iwl_tso_hdr_page *get_page_hdr(struct iwl_trans *trans, size_t len,
				      struct sk_buff *skb)
{
	struct iwl_tso_hdr_page *p = this_cpu_ptr(trans->txqs.tso_hdr_page);
	struct page **page_ptr;

	page_ptr = (void *)((u8 *)skb->cb + trans->txqs.page_offs);

	if (WARN_ON(*page_ptr))
		return NULL;

	if (!p->page)
		goto alloc;

	/*
	 * Check if there's enough room on this page
	 *
	 * Note that we put a page chaining pointer *last* in the
	 * page - we need it somewhere, and if it's there then we
	 * avoid DMA mapping the last bits of the page which may
	 * trigger the 32-bit boundary hardware bug.
	 *
	 * (see also get_workaround_page() in tx-gen2.c)
	 */
	if (p->pos + len < (u8 *)page_address(p->page) + PAGE_SIZE -
			   sizeof(void *))
		goto out;

	/* We don't have enough room on this page, get a new one. */
	__free_page(p->page);

alloc:
	p->page = alloc_page(GFP_ATOMIC);
	if (!p->page)
		return NULL;
	p->pos = page_address(p->page);
	/* set the chaining pointer to NULL */
	*(void **)(page_address(p->page) + PAGE_SIZE - sizeof(void *)) = NULL;
out:
	*page_ptr = p->page;
	get_page(p->page);
	return p;
}
#endif

static int iwl_txq_gen2_build_amsdu(struct iwl_trans *trans,
				    struct sk_buff *skb,
				    struct iwl_tfh_tfd *tfd, int start_len,
				    u8 hdr_len,
				    struct iwl_device_tx_cmd *dev_cmd)
{
#ifdef CONFIG_INET
	struct iwl_tx_cmd_gen2 *tx_cmd = (void *)dev_cmd->payload;
	struct ieee80211_hdr *hdr = (void *)skb->data;
	unsigned int snap_ip_tcp_hdrlen, ip_hdrlen, total_len, hdr_room;
	unsigned int mss = skb_shinfo(skb)->gso_size;
	u16 length, amsdu_pad;
	u8 *start_hdr;
	struct iwl_tso_hdr_page *hdr_page;
	struct tso_t tso;

	trace_iwlwifi_dev_tx(trans->dev, skb, tfd, sizeof(*tfd),
			     &dev_cmd->hdr, start_len, 0);

	ip_hdrlen = skb_transport_header(skb) - skb_network_header(skb);
	snap_ip_tcp_hdrlen = 8 + ip_hdrlen + tcp_hdrlen(skb);
	total_len = skb->len - snap_ip_tcp_hdrlen - hdr_len;
	amsdu_pad = 0;

	/* total amount of header we may need for this A-MSDU */
	hdr_room = DIV_ROUND_UP(total_len, mss) *
		(3 + snap_ip_tcp_hdrlen + sizeof(struct ethhdr));

	/* Our device supports 9 segments at most, it will fit in 1 page */
	hdr_page = get_page_hdr(trans, hdr_room, skb);
	if (!hdr_page)
		return -ENOMEM;

	start_hdr = hdr_page->pos;

	/*
	 * Pull the ieee80211 header to be able to use TSO core,
	 * we will restore it for the tx_status flow.
	 */
	skb_pull(skb, hdr_len);

	/*
	 * Remove the length of all the headers that we don't actually
	 * have in the MPDU by themselves, but that we duplicate into
	 * all the different MSDUs inside the A-MSDU.
	 */
	le16_add_cpu(&tx_cmd->len, -snap_ip_tcp_hdrlen);

	tso_start(skb, &tso);

	while (total_len) {
		/* this is the data left for this subframe */
		unsigned int data_left = min_t(unsigned int, mss, total_len);
		struct sk_buff *csum_skb = NULL;
		unsigned int tb_len;
		dma_addr_t tb_phys;
		u8 *subf_hdrs_start = hdr_page->pos;

		total_len -= data_left;

		memset(hdr_page->pos, 0, amsdu_pad);
		hdr_page->pos += amsdu_pad;
		amsdu_pad = (4 - (sizeof(struct ethhdr) + snap_ip_tcp_hdrlen +
				  data_left)) & 0x3;
		ether_addr_copy(hdr_page->pos, ieee80211_get_DA(hdr));
		hdr_page->pos += ETH_ALEN;
		ether_addr_copy(hdr_page->pos, ieee80211_get_SA(hdr));
		hdr_page->pos += ETH_ALEN;

		length = snap_ip_tcp_hdrlen + data_left;
		*((__be16 *)hdr_page->pos) = cpu_to_be16(length);
		hdr_page->pos += sizeof(length);

		/*
		 * This will copy the SNAP as well which will be considered
		 * as MAC header.
		 */
		tso_build_hdr(skb, hdr_page->pos, &tso, data_left, !total_len);

		hdr_page->pos += snap_ip_tcp_hdrlen;

		tb_len = hdr_page->pos - start_hdr;
		tb_phys = dma_map_single(trans->dev, start_hdr,
					 tb_len, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(trans->dev, tb_phys))) {
			dev_kfree_skb(csum_skb);
			goto out_err;
		}
		/*
		 * No need for _with_wa, this is from the TSO page and
		 * we leave some space at the end of it so can't hit
		 * the buggy scenario.
		 */
		iwl_txq_gen2_set_tb(trans, tfd, tb_phys, tb_len);
		trace_iwlwifi_dev_tx_tb(trans->dev, skb, start_hdr,
					tb_phys, tb_len);
		/* add this subframe's headers' length to the tx_cmd */
		le16_add_cpu(&tx_cmd->len, hdr_page->pos - subf_hdrs_start);

		/* prepare the start_hdr for the next subframe */
		start_hdr = hdr_page->pos;

		/* put the payload */
		while (data_left) {
			int ret;

			tb_len = min_t(unsigned int, tso.size, data_left);
			tb_phys = dma_map_single(trans->dev, tso.data,
						 tb_len, DMA_TO_DEVICE);
			ret = iwl_txq_gen2_set_tb_with_wa(trans, skb, tfd,
							  tb_phys, tso.data,
							  tb_len, NULL);
			if (ret) {
				dev_kfree_skb(csum_skb);
				goto out_err;
			}

			data_left -= tb_len;
			tso_build_data(skb, &tso, tb_len);
		}
	}

	/* re -add the WiFi header */
	skb_push(skb, hdr_len);

	return 0;

out_err:
#endif
	return -EINVAL;
}

static struct
iwl_tfh_tfd *iwl_txq_gen2_build_tx_amsdu(struct iwl_trans *trans,
					 struct iwl_txq *txq,
					 struct iwl_device_tx_cmd *dev_cmd,
					 struct sk_buff *skb,
					 struct iwl_cmd_meta *out_meta,
					 int hdr_len,
					 int tx_cmd_len)
{
	int idx = iwl_txq_get_cmd_index(txq, txq->write_ptr);
	struct iwl_tfh_tfd *tfd = iwl_txq_get_tfd(trans, txq, idx);
	dma_addr_t tb_phys;
	int len;
	void *tb1_addr;

	tb_phys = iwl_txq_get_first_tb_dma(txq, idx);

	/*
	 * No need for _with_wa, the first TB allocation is aligned up
	 * to a 64-byte boundary and thus can't be at the end or cross
	 * a page boundary (much less a 2^32 boundary).
	 */
	iwl_txq_gen2_set_tb(trans, tfd, tb_phys, IWL_FIRST_TB_SIZE);

	/*
	 * The second TB (tb1) points to the remainder of the TX command
	 * and the 802.11 header - dword aligned size
	 * (This calculation modifies the TX command, so do it before the
	 * setup of the first TB)
	 */
	len = tx_cmd_len + sizeof(struct iwl_cmd_header) + hdr_len -
	      IWL_FIRST_TB_SIZE;

	/* do not align A-MSDU to dword as the subframe header aligns it */

	/* map the data for TB1 */
	tb1_addr = ((u8 *)&dev_cmd->hdr) + IWL_FIRST_TB_SIZE;
	tb_phys = dma_map_single(trans->dev, tb1_addr, len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(trans->dev, tb_phys)))
		goto out_err;
	/*
	 * No need for _with_wa(), we ensure (via alignment) that the data
	 * here can never cross or end at a page boundary.
	 */
	iwl_txq_gen2_set_tb(trans, tfd, tb_phys, len);

	if (iwl_txq_gen2_build_amsdu(trans, skb, tfd, len + IWL_FIRST_TB_SIZE,
				     hdr_len, dev_cmd))
		goto out_err;

	/* building the A-MSDU might have changed this data, memcpy it now */
	memcpy(&txq->first_tb_bufs[idx], dev_cmd, IWL_FIRST_TB_SIZE);
	return tfd;

out_err:
	iwl_txq_gen2_tfd_unmap(trans, out_meta, tfd);
	return NULL;
}

static int iwl_txq_gen2_tx_add_frags(struct iwl_trans *trans,
				     struct sk_buff *skb,
				     struct iwl_tfh_tfd *tfd,
				     struct iwl_cmd_meta *out_meta)
{
	int i;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		dma_addr_t tb_phys;
		unsigned int fragsz = skb_frag_size(frag);
		int ret;

		if (!fragsz)
			continue;

		tb_phys = skb_frag_dma_map(trans->dev, frag, 0,
					   fragsz, DMA_TO_DEVICE);
		ret = iwl_txq_gen2_set_tb_with_wa(trans, skb, tfd, tb_phys,
						  skb_frag_address(frag),
						  fragsz, out_meta);
		if (ret)
			return ret;
	}

	return 0;
}

static struct
iwl_tfh_tfd *iwl_txq_gen2_build_tx(struct iwl_trans *trans,
				   struct iwl_txq *txq,
				   struct iwl_device_tx_cmd *dev_cmd,
				   struct sk_buff *skb,
				   struct iwl_cmd_meta *out_meta,
				   int hdr_len,
				   int tx_cmd_len,
				   bool pad)
{
	int idx = iwl_txq_get_cmd_index(txq, txq->write_ptr);
	struct iwl_tfh_tfd *tfd = iwl_txq_get_tfd(trans, txq, idx);
	dma_addr_t tb_phys;
	int len, tb1_len, tb2_len;
	void *tb1_addr;
	struct sk_buff *frag;

	tb_phys = iwl_txq_get_first_tb_dma(txq, idx);

	/* The first TB points to bi-directional DMA data */
	memcpy(&txq->first_tb_bufs[idx], dev_cmd, IWL_FIRST_TB_SIZE);

	/*
	 * No need for _with_wa, the first TB allocation is aligned up
	 * to a 64-byte boundary and thus can't be at the end or cross
	 * a page boundary (much less a 2^32 boundary).
	 */
	iwl_txq_gen2_set_tb(trans, tfd, tb_phys, IWL_FIRST_TB_SIZE);

	/*
	 * The second TB (tb1) points to the remainder of the TX command
	 * and the 802.11 header - dword aligned size
	 * (This calculation modifies the TX command, so do it before the
	 * setup of the first TB)
	 */
	len = tx_cmd_len + sizeof(struct iwl_cmd_header) + hdr_len -
	      IWL_FIRST_TB_SIZE;

	if (pad)
		tb1_len = ALIGN(len, 4);
	else
		tb1_len = len;

	/* map the data for TB1 */
	tb1_addr = ((u8 *)&dev_cmd->hdr) + IWL_FIRST_TB_SIZE;
	tb_phys = dma_map_single(trans->dev, tb1_addr, tb1_len, DMA_TO_DEVICE);
	if (unlikely(dma_mapping_error(trans->dev, tb_phys)))
		goto out_err;
	/*
	 * No need for _with_wa(), we ensure (via alignment) that the data
	 * here can never cross or end at a page boundary.
	 */
	iwl_txq_gen2_set_tb(trans, tfd, tb_phys, tb1_len);
	trace_iwlwifi_dev_tx(trans->dev, skb, tfd, sizeof(*tfd), &dev_cmd->hdr,
			     IWL_FIRST_TB_SIZE + tb1_len, hdr_len);

	/* set up TFD's third entry to point to remainder of skb's head */
	tb2_len = skb_headlen(skb) - hdr_len;

	if (tb2_len > 0) {
		int ret;

		tb_phys = dma_map_single(trans->dev, skb->data + hdr_len,
					 tb2_len, DMA_TO_DEVICE);
		ret = iwl_txq_gen2_set_tb_with_wa(trans, skb, tfd, tb_phys,
						  skb->data + hdr_len, tb2_len,
						  NULL);
		if (ret)
			goto out_err;
	}

	if (iwl_txq_gen2_tx_add_frags(trans, skb, tfd, out_meta))
		goto out_err;

	skb_walk_frags(skb, frag) {
		int ret;

		tb_phys = dma_map_single(trans->dev, frag->data,
					 skb_headlen(frag), DMA_TO_DEVICE);
		ret = iwl_txq_gen2_set_tb_with_wa(trans, skb, tfd, tb_phys,
						  frag->data,
						  skb_headlen(frag), NULL);
		if (ret)
			goto out_err;
		if (iwl_txq_gen2_tx_add_frags(trans, frag, tfd, out_meta))
			goto out_err;
	}

	return tfd;

out_err:
	iwl_txq_gen2_tfd_unmap(trans, out_meta, tfd);
	return NULL;
}

static
struct iwl_tfh_tfd *iwl_txq_gen2_build_tfd(struct iwl_trans *trans,
					   struct iwl_txq *txq,
					   struct iwl_device_tx_cmd *dev_cmd,
					   struct sk_buff *skb,
					   struct iwl_cmd_meta *out_meta)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	int idx = iwl_txq_get_cmd_index(txq, txq->write_ptr);
	struct iwl_tfh_tfd *tfd = iwl_txq_get_tfd(trans, txq, idx);
	int len, hdr_len;
	bool amsdu;

	/* There must be data left over for TB1 or this code must be changed */
	BUILD_BUG_ON(sizeof(struct iwl_tx_cmd_gen2) < IWL_FIRST_TB_SIZE);

	memset(tfd, 0, sizeof(*tfd));

	if (trans->trans_cfg->device_family < IWL_DEVICE_FAMILY_AX210)
		len = sizeof(struct iwl_tx_cmd_gen2);
	else
		len = sizeof(struct iwl_tx_cmd_gen3);

	amsdu = ieee80211_is_data_qos(hdr->frame_control) &&
			(*ieee80211_get_qos_ctl(hdr) &
			 IEEE80211_QOS_CTL_A_MSDU_PRESENT);

	hdr_len = ieee80211_hdrlen(hdr->frame_control);

	/*
	 * Only build A-MSDUs here if doing so by GSO, otherwise it may be
	 * an A-MSDU for other reasons, e.g. NAN or an A-MSDU having been
	 * built in the higher layers already.
	 */
	if (amsdu && skb_shinfo(skb)->gso_size)
		return iwl_txq_gen2_build_tx_amsdu(trans, txq, dev_cmd, skb,
						    out_meta, hdr_len, len);
	return iwl_txq_gen2_build_tx(trans, txq, dev_cmd, skb, out_meta,
				      hdr_len, len, !amsdu);
}

int iwl_txq_space(struct iwl_trans *trans, const struct iwl_txq *q)
{
	unsigned int max;
	unsigned int used;

	/*
	 * To avoid ambiguity between empty and completely full queues, there
	 * should always be less than max_tfd_queue_size elements in the queue.
	 * If q->n_window is smaller than max_tfd_queue_size, there is no need
	 * to reserve any queue entries for this purpose.
	 */
	if (q->n_window < trans->trans_cfg->base_params->max_tfd_queue_size)
		max = q->n_window;
	else
		max = trans->trans_cfg->base_params->max_tfd_queue_size - 1;

	/*
	 * max_tfd_queue_size is a power of 2, so the following is equivalent to
	 * modulo by max_tfd_queue_size and is well defined.
	 */
	used = (q->write_ptr - q->read_ptr) &
		(trans->trans_cfg->base_params->max_tfd_queue_size - 1);

	if (WARN_ON(used > max))
		return 0;

	return max - used;
}

int iwl_txq_gen2_tx(struct iwl_trans *trans, struct sk_buff *skb,
		    struct iwl_device_tx_cmd *dev_cmd, int txq_id)
{
	struct iwl_cmd_meta *out_meta;
	struct iwl_txq *txq = trans->txqs.txq[txq_id];
	u16 cmd_len;
	int idx;
	void *tfd;

	if (WARN_ONCE(txq_id >= IWL_MAX_TVQM_QUEUES,
		      "queue %d out of range", txq_id))
		return -EINVAL;

	if (WARN_ONCE(!test_bit(txq_id, trans->txqs.queue_used),
		      "TX on unused queue %d\n", txq_id))
		return -EINVAL;

	if (skb_is_nonlinear(skb) &&
	    skb_shinfo(skb)->nr_frags > IWL_TRANS_MAX_FRAGS(trans) &&
	    __skb_linearize(skb))
		return -ENOMEM;

	spin_lock(&txq->lock);

	if (iwl_txq_space(trans, txq) < txq->high_mark) {
		iwl_txq_stop(trans, txq);

		/* don't put the packet on the ring, if there is no room */
		if (unlikely(iwl_txq_space(trans, txq) < 3)) {
			struct iwl_device_tx_cmd **dev_cmd_ptr;

			dev_cmd_ptr = (void *)((u8 *)skb->cb +
					       trans->txqs.dev_cmd_offs);

			*dev_cmd_ptr = dev_cmd;
			__skb_queue_tail(&txq->overflow_q, skb);
			spin_unlock(&txq->lock);
			return 0;
		}
	}

	idx = iwl_txq_get_cmd_index(txq, txq->write_ptr);

	/* Set up driver data for this TFD */
	txq->entries[idx].skb = skb;
	txq->entries[idx].cmd = dev_cmd;

	dev_cmd->hdr.sequence =
		cpu_to_le16((u16)(QUEUE_TO_SEQ(txq_id) |
			    INDEX_TO_SEQ(idx)));

	/* Set up first empty entry in queue's array of Tx/cmd buffers */
	out_meta = &txq->entries[idx].meta;
	out_meta->flags = 0;

	tfd = iwl_txq_gen2_build_tfd(trans, txq, dev_cmd, skb, out_meta);
	if (!tfd) {
		spin_unlock(&txq->lock);
		return -1;
	}

	if (trans->trans_cfg->device_family >= IWL_DEVICE_FAMILY_AX210) {
		struct iwl_tx_cmd_gen3 *tx_cmd_gen3 =
			(void *)dev_cmd->payload;

		cmd_len = le16_to_cpu(tx_cmd_gen3->len);
	} else {
		struct iwl_tx_cmd_gen2 *tx_cmd_gen2 =
			(void *)dev_cmd->payload;

		cmd_len = le16_to_cpu(tx_cmd_gen2->len);
	}

	/* Set up entry for this TFD in Tx byte-count array */
	iwl_pcie_gen2_update_byte_tbl(trans, txq, cmd_len,
				      iwl_txq_gen2_get_num_tbs(trans, tfd));

	/* start timer if queue currently empty */
	if (txq->read_ptr == txq->write_ptr && txq->wd_timeout)
		mod_timer(&txq->stuck_timer, jiffies + txq->wd_timeout);

	/* Tell device the write index *just past* this latest filled TFD */
	txq->write_ptr = iwl_txq_inc_wrap(trans, txq->write_ptr);
	iwl_txq_inc_wr_ptr(trans, txq);
	/*
	 * At this point the frame is "transmitted" successfully
	 * and we will get a TX status notification eventually.
	 */
	spin_unlock(&txq->lock);
	return 0;
}

/*************** HOST COMMAND QUEUE FUNCTIONS   *****/

/*
 * iwl_txq_gen2_unmap -  Unmap any remaining DMA mappings and free skb's
 */
void iwl_txq_gen2_unmap(struct iwl_trans *trans, int txq_id)
{
	struct iwl_txq *txq = trans->txqs.txq[txq_id];

	spin_lock_bh(&txq->lock);
	while (txq->write_ptr != txq->read_ptr) {
		IWL_DEBUG_TX_REPLY(trans, "Q %d Free %d\n",
				   txq_id, txq->read_ptr);

		if (txq_id != trans->txqs.cmd.q_id) {
			int idx = iwl_txq_get_cmd_index(txq, txq->read_ptr);
			struct sk_buff *skb = txq->entries[idx].skb;

			if (WARN_ON_ONCE(!skb))
				continue;

			iwl_txq_free_tso_page(trans, skb);
		}
		iwl_txq_gen2_free_tfd(trans, txq);
		txq->read_ptr = iwl_txq_inc_wrap(trans, txq->read_ptr);
	}

	while (!skb_queue_empty(&txq->overflow_q)) {
		struct sk_buff *skb = __skb_dequeue(&txq->overflow_q);

		iwl_op_mode_free_skb(trans->op_mode, skb);
	}

	spin_unlock_bh(&txq->lock);

	/* just in case - this queue may have been stopped */
	iwl_wake_queue(trans, txq);
}

static void iwl_txq_gen2_free_memory(struct iwl_trans *trans,
				     struct iwl_txq *txq)
{
	struct device *dev = trans->dev;

	/* De-alloc circular buffer of TFDs */
	if (txq->tfds) {
		dma_free_coherent(dev,
				  trans->txqs.tfd.size * txq->n_window,
				  txq->tfds, txq->dma_addr);
		dma_free_coherent(dev,
				  sizeof(*txq->first_tb_bufs) * txq->n_window,
				  txq->first_tb_bufs, txq->first_tb_dma);
	}

	kfree(txq->entries);
	if (txq->bc_tbl.addr)
		dma_pool_free(trans->txqs.bc_pool,
			      txq->bc_tbl.addr, txq->bc_tbl.dma);
	kfree(txq);
}

/*
 * iwl_pcie_txq_free - Deallocate DMA queue.
 * @txq: Transmit queue to deallocate.
 *
 * Empty queue by removing and destroying all BD's.
 * Free all buffers.
 * 0-fill, but do not free "txq" descriptor structure.
 */
static void iwl_txq_gen2_free(struct iwl_trans *trans, int txq_id)
{
	struct iwl_txq *txq;
	int i;

	if (WARN_ONCE(txq_id >= IWL_MAX_TVQM_QUEUES,
		      "queue %d out of range", txq_id))
		return;

	txq = trans->txqs.txq[txq_id];

	if (WARN_ON(!txq))
		return;

	iwl_txq_gen2_unmap(trans, txq_id);

	/* De-alloc array of command/tx buffers */
	if (txq_id == trans->txqs.cmd.q_id)
		for (i = 0; i < txq->n_window; i++) {
			kzfree(txq->entries[i].cmd);
			kzfree(txq->entries[i].free_buf);
		}
	del_timer_sync(&txq->stuck_timer);

	iwl_txq_gen2_free_memory(trans, txq);

	trans->txqs.txq[txq_id] = NULL;

	clear_bit(txq_id, trans->txqs.queue_used);
}

/*
 * iwl_queue_init - Initialize queue's high/low-water and read/write indexes
 */
static int iwl_queue_init(struct iwl_txq *q, int slots_num)
{
	q->n_window = slots_num;

	/* slots_num must be power-of-two size, otherwise
	 * iwl_txq_get_cmd_index is broken. */
	if (WARN_ON(!is_power_of_2(slots_num)))
		return -EINVAL;

	q->low_mark = q->n_window / 4;
	if (q->low_mark < 4)
		q->low_mark = 4;

	q->high_mark = q->n_window / 8;
	if (q->high_mark < 2)
		q->high_mark = 2;

	q->write_ptr = 0;
	q->read_ptr = 0;

	return 0;
}

int iwl_txq_init(struct iwl_trans *trans, struct iwl_txq *txq, int slots_num,
		 bool cmd_queue)
{
	int ret;
	u32 tfd_queue_max_size =
		trans->trans_cfg->base_params->max_tfd_queue_size;

	txq->need_update = false;

	/* max_tfd_queue_size must be power-of-two size, otherwise
	 * iwl_txq_inc_wrap and iwl_txq_dec_wrap are broken. */
	if (WARN_ONCE(tfd_queue_max_size & (tfd_queue_max_size - 1),
		      "Max tfd queue size must be a power of two, but is %d",
		      tfd_queue_max_size))
		return -EINVAL;

	/* Initialize queue's high/low-water marks, and head/tail indexes */
	ret = iwl_queue_init(txq, slots_num);
	if (ret)
		return ret;

	spin_lock_init(&txq->lock);

	if (cmd_queue) {
		static struct lock_class_key iwl_txq_cmd_queue_lock_class;

		lockdep_set_class(&txq->lock, &iwl_txq_cmd_queue_lock_class);
	}

	__skb_queue_head_init(&txq->overflow_q);

	return 0;
}

void iwl_txq_free_tso_page(struct iwl_trans *trans, struct sk_buff *skb)
{
	struct page **page_ptr;
	struct page *next;

	page_ptr = (void *)((u8 *)skb->cb + trans->txqs.page_offs);
	next = *page_ptr;
	*page_ptr = NULL;

	while (next) {
		struct page *tmp = next;

		next = *(void **)(page_address(next) + PAGE_SIZE -
				  sizeof(void *));
		__free_page(tmp);
	}
}

void iwl_txq_log_scd_error(struct iwl_trans *trans, struct iwl_txq *txq)
{
	u32 txq_id = txq->id;
	u32 status;
	bool active;
	u8 fifo;

	if (trans->trans_cfg->use_tfh) {
		IWL_ERR(trans, "Queue %d is stuck %d %d\n", txq_id,
			txq->read_ptr, txq->write_ptr);
		/* TODO: access new SCD registers and dump them */
		return;
	}

	status = iwl_read_prph(trans, SCD_QUEUE_STATUS_BITS(txq_id));
	fifo = (status >> SCD_QUEUE_STTS_REG_POS_TXF) & 0x7;
	active = !!(status & BIT(SCD_QUEUE_STTS_REG_POS_ACTIVE));

	IWL_ERR(trans,
		"Queue %d is %sactive on fifo %d and stuck for %u ms. SW [%d, %d] HW [%d, %d] FH TRB=0x0%x\n",
		txq_id, active ? "" : "in", fifo,
		jiffies_to_msecs(txq->wd_timeout),
		txq->read_ptr, txq->write_ptr,
		iwl_read_prph(trans, SCD_QUEUE_RDPTR(txq_id)) &
			(trans->trans_cfg->base_params->max_tfd_queue_size - 1),
			iwl_read_prph(trans, SCD_QUEUE_WRPTR(txq_id)) &
			(trans->trans_cfg->base_params->max_tfd_queue_size - 1),
			iwl_read_direct32(trans, FH_TX_TRB_REG(fifo)));
}

static void iwl_txq_stuck_timer(struct timer_list *t)
{
	struct iwl_txq *txq = from_timer(txq, t, stuck_timer);
	struct iwl_trans *trans = txq->trans;

	spin_lock(&txq->lock);
	/* check if triggered erroneously */
	if (txq->read_ptr == txq->write_ptr) {
		spin_unlock(&txq->lock);
		return;
	}
	spin_unlock(&txq->lock);

	iwl_txq_log_scd_error(trans, txq);

	iwl_force_nmi(trans);
}

int iwl_txq_alloc(struct iwl_trans *trans, struct iwl_txq *txq, int slots_num,
		  bool cmd_queue)
{
	size_t tfd_sz = trans->txqs.tfd.size *
		trans->trans_cfg->base_params->max_tfd_queue_size;
	size_t tb0_buf_sz;
	int i;

	if (WARN_ON(txq->entries || txq->tfds))
		return -EINVAL;

	if (trans->trans_cfg->use_tfh)
		tfd_sz = trans->txqs.tfd.size * slots_num;

	timer_setup(&txq->stuck_timer, iwl_txq_stuck_timer, 0);
	txq->trans = trans;

	txq->n_window = slots_num;

	txq->entries = kcalloc(slots_num,
			       sizeof(struct iwl_pcie_txq_entry),
			       GFP_KERNEL);

	if (!txq->entries)
		goto error;

	if (cmd_queue)
		for (i = 0; i < slots_num; i++) {
			txq->entries[i].cmd =
				kmalloc(sizeof(struct iwl_device_cmd),
					GFP_KERNEL);
			if (!txq->entries[i].cmd)
				goto error;
		}

	/* Circular buffer of transmit frame descriptors (TFDs),
	 * shared with device */
	txq->tfds = dma_alloc_coherent(trans->dev, tfd_sz,
				       &txq->dma_addr, GFP_KERNEL);
	if (!txq->tfds)
		goto error;

	BUILD_BUG_ON(sizeof(*txq->first_tb_bufs) != IWL_FIRST_TB_SIZE_ALIGN);

	tb0_buf_sz = sizeof(*txq->first_tb_bufs) * slots_num;

	txq->first_tb_bufs = dma_alloc_coherent(trans->dev, tb0_buf_sz,
						&txq->first_tb_dma,
						GFP_KERNEL);
	if (!txq->first_tb_bufs)
		goto err_free_tfds;

	return 0;
err_free_tfds:
	dma_free_coherent(trans->dev, tfd_sz, txq->tfds, txq->dma_addr);
error:
	if (txq->entries && cmd_queue)
		for (i = 0; i < slots_num; i++)
			kfree(txq->entries[i].cmd);
	kfree(txq->entries);
	txq->entries = NULL;

	return -ENOMEM;
}

static int iwl_txq_dyn_alloc_dma(struct iwl_trans *trans,
				 struct iwl_txq **intxq, int size,
				 unsigned int timeout)
{
	size_t bc_tbl_size, bc_tbl_entries;
	struct iwl_txq *txq;
	int ret;

	WARN_ON(!trans->txqs.bc_tbl_size);

	bc_tbl_size = trans->txqs.bc_tbl_size;
	bc_tbl_entries = bc_tbl_size / sizeof(u16);

	if (WARN_ON(size > bc_tbl_entries))
		return -EINVAL;

	txq = kzalloc(sizeof(*txq), GFP_KERNEL);
	if (!txq)
		return -ENOMEM;

	txq->bc_tbl.addr = dma_pool_alloc(trans->txqs.bc_pool, GFP_KERNEL,
					  &txq->bc_tbl.dma);
	if (!txq->bc_tbl.addr) {
		IWL_ERR(trans, "Scheduler BC Table allocation failed\n");
		kfree(txq);
		return -ENOMEM;
	}

	ret = iwl_txq_alloc(trans, txq, size, false);
	if (ret) {
		IWL_ERR(trans, "Tx queue alloc failed\n");
		goto error;
	}
	ret = iwl_txq_init(trans, txq, size, false);
	if (ret) {
		IWL_ERR(trans, "Tx queue init failed\n");
		goto error;
	}

	txq->wd_timeout = msecs_to_jiffies(timeout);

	*intxq = txq;
	return 0;

error:
	iwl_txq_gen2_free_memory(trans, txq);
	return ret;
}

static int iwl_txq_alloc_response(struct iwl_trans *trans, struct iwl_txq *txq,
				  struct iwl_host_cmd *hcmd)
{
	struct iwl_tx_queue_cfg_rsp *rsp;
	int ret, qid;
	u32 wr_ptr;

	if (WARN_ON(iwl_rx_packet_payload_len(hcmd->resp_pkt) !=
		    sizeof(*rsp))) {
		ret = -EINVAL;
		goto error_free_resp;
	}

	rsp = (void *)hcmd->resp_pkt->data;
	qid = le16_to_cpu(rsp->queue_number);
	wr_ptr = le16_to_cpu(rsp->write_pointer);

	if (qid >= ARRAY_SIZE(trans->txqs.txq)) {
		WARN_ONCE(1, "queue index %d unsupported", qid);
		ret = -EIO;
		goto error_free_resp;
	}

	if (test_and_set_bit(qid, trans->txqs.queue_used)) {
		WARN_ONCE(1, "queue %d already used", qid);
		ret = -EIO;
		goto error_free_resp;
	}

	txq->id = qid;
	trans->txqs.txq[qid] = txq;
	wr_ptr &= (trans->trans_cfg->base_params->max_tfd_queue_size - 1);

	/* Place first TFD at index corresponding to start sequence number */
	txq->read_ptr = wr_ptr;
	txq->write_ptr = wr_ptr;

	IWL_DEBUG_TX_QUEUES(trans, "Activate queue %d\n", qid);

	iwl_free_resp(hcmd);
	return qid;

error_free_resp:
	iwl_free_resp(hcmd);
	iwl_txq_gen2_free_memory(trans, txq);
	return ret;
}

int iwl_txq_dyn_alloc(struct iwl_trans *trans, __le16 flags, u8 sta_id, u8 tid,
		      int cmd_id, int size, unsigned int timeout)
{
	struct iwl_txq *txq = NULL;
	struct iwl_tx_queue_cfg_cmd cmd = {
		.flags = flags,
		.sta_id = sta_id,
		.tid = tid,
	};
	struct iwl_host_cmd hcmd = {
		.id = cmd_id,
		.len = { sizeof(cmd) },
		.data = { &cmd, },
		.flags = CMD_WANT_SKB,
	};
	int ret;

	ret = iwl_txq_dyn_alloc_dma(trans, &txq, size, timeout);
	if (ret)
		return ret;

	cmd.tfdq_addr = cpu_to_le64(txq->dma_addr);
	cmd.byte_cnt_addr = cpu_to_le64(txq->bc_tbl.dma);
	cmd.cb_size = cpu_to_le32(TFD_QUEUE_CB_SIZE(size));

	ret = iwl_trans_send_cmd(trans, &hcmd);
	if (ret)
		goto error;

	return iwl_txq_alloc_response(trans, txq, &hcmd);

error:
	iwl_txq_gen2_free_memory(trans, txq);
	return ret;
}

void iwl_txq_dyn_free(struct iwl_trans *trans, int queue)
{
	if (WARN(queue >= IWL_MAX_TVQM_QUEUES,
		 "queue %d out of range", queue))
		return;

	/*
	 * Upon HW Rfkill - we stop the device, and then stop the queues
	 * in the op_mode. Just for the sake of the simplicity of the op_mode,
	 * allow the op_mode to call txq_disable after it already called
	 * stop_device.
	 */
	if (!test_and_clear_bit(queue, trans->txqs.queue_used)) {
		WARN_ONCE(test_bit(STATUS_DEVICE_ENABLED, &trans->status),
			  "queue %d not used", queue);
		return;
	}

	iwl_txq_gen2_unmap(trans, queue);

	IWL_DEBUG_TX_QUEUES(trans, "Deactivate queue %d\n", queue);
}

void iwl_txq_gen2_tx_free(struct iwl_trans *trans)
{
	int i;

	memset(trans->txqs.queue_used, 0, sizeof(trans->txqs.queue_used));

	/* Free all TX queues */
	for (i = 0; i < ARRAY_SIZE(trans->txqs.txq); i++) {
		if (!trans->txqs.txq[i])
			continue;

		iwl_txq_gen2_free(trans, i);
	}
}

int iwl_txq_gen2_init(struct iwl_trans *trans, int txq_id, int queue_size)
{
	struct iwl_txq *queue;
	int ret;

	/* alloc and init the tx queue */
	if (!trans->txqs.txq[txq_id]) {
		queue = kzalloc(sizeof(*queue), GFP_KERNEL);
		if (!queue) {
			IWL_ERR(trans, "Not enough memory for tx queue\n");
			return -ENOMEM;
		}
		trans->txqs.txq[txq_id] = queue;
		ret = iwl_txq_alloc(trans, queue, queue_size, true);
		if (ret) {
			IWL_ERR(trans, "Tx %d queue init failed\n", txq_id);
			goto error;
		}
	} else {
		queue = trans->txqs.txq[txq_id];
	}

	ret = iwl_txq_init(trans, queue, queue_size,
			   (txq_id == trans->txqs.cmd.q_id));
	if (ret) {
		IWL_ERR(trans, "Tx %d queue alloc failed\n", txq_id);
		goto error;
	}
	trans->txqs.txq[txq_id]->id = txq_id;
	set_bit(txq_id, trans->txqs.queue_used);

	return 0;

error:
	iwl_txq_gen2_tx_free(trans);
	return ret;
}

