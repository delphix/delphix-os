/*
 * Copyright (C) 2007 VMware, Inc. All rights reserved.
 *
 * The contents of this file are subject to the terms of the Common
 * Development and Distribution License (the "License") version 1.0
 * and no later version.  You may not use this file except in
 * compliance with the License.
 *
 * You can obtain a copy of the License at
 *         http://www.opensource.org/licenses/cddl1.php
 *
 * See the License for the specific language governing permissions
 * and limitations under the License.
 */

/*
 * Copyright (c) 2012, 2019 by Delphix. All rights reserved.
 */

#include <vmxnet3.h>
#include <netinet/udp.h>

typedef enum vmxnet3_txstatus {
	VMXNET3_TX_OK,
	VMXNET3_TX_FAILURE,
	VMXNET3_TX_PULLUP,
	VMXNET3_TX_RINGFULL
} vmxnet3_txstatus;

typedef struct vmxnet3_offload_t {
	uint16_t om;
	uint16_t hlen;
	uint16_t msscof;
} vmxnet3_offload_t;

/*
 * Deadman timer for the tx ring.
 * If no tx completions come for vmxnet3_tx_deadman_timeout_ms milliseconds,
 * then a counter is incremented. When the counter reaches
 * vmxnet3_tx_deadman_max_count, it resets the interface. We use a counter to
 * reduce the impact of a sudden jump in the value of the system lbolt timer.
 * To disable, set vmxnet3_tx_deadman_max_count to 0,
 */
clock_t vmxnet3_tx_deadman_timeout_ms = 1000;
int vmxnet3_tx_deadman_max_count = 3;

/*
 * Initialize a TxQueue. Currently nothing needs to be done.
 */
/* ARGSUSED */
int
vmxnet3_txqueue_init(vmxnet3_softc_t *dp)
{
	return (0);
}

/*
 * Finish a TxQueue by freeing all pending Tx.
 */
void
vmxnet3_txqueue_fini(vmxnet3_softc_t *dp)
{
	ASSERT(!dp->devEnabled);

	vmxnet3_txqueue_t *txq = &dp->txQueue;
	for (unsigned int i = 0; i < txq->cmdRing.size; i++) {
		mblk_t *mp = txq->metaRing[i].mp;
		if (mp) {
			freemsg(mp);
		}
	}
}

/*
 * Build the offload context of a msg.
 *
 * Returns:
 *	0 if everything went well.
 *	+n if n bytes need to be pulled up.
 *	-1 in case of error.
 */
static int
vmxnet3_tx_prepare_offload(vmxnet3_softc_t *dp, vmxnet3_offload_t *ol,
    int *headers_len, mblk_t *mp)
{
	int ret = 0;
	uint32_t start, stuff, value, flags, lso_flag, mss;

	ol->om = VMXNET3_OM_NONE;
	ol->hlen = 0;
	ol->msscof = 0;

	hcksum_retrieve(mp, NULL, NULL, &start, &stuff, NULL, &value, &flags);

	mac_lso_get(mp, &mss, &lso_flag);

	if (flags || lso_flag) {
		struct ether_vlan_header *etvh = (void *) mp->b_rptr;
		struct ether_header *eth = (void *) mp->b_rptr;
		uint8_t ethLen, ipLen, l4Len;
		mblk_t *mblk = mp;
		uint8_t *ip, *l4;
		uint16_t l3proto;
		uint8_t l4proto;

		if (eth->ether_type == htons(ETHERTYPE_VLAN)) {
			ethLen = sizeof (struct ether_vlan_header);
			l3proto = ntohs(etvh->ether_type);
		} else {
			ethLen = sizeof (struct ether_header);
			l3proto = ntohs(eth->ether_type);
		}

		VMXNET3_DEBUG(dp, 4, "flags=0x%x, ethLen=%u, start=%u, "
		    "stuff=%u, value=%u\n", flags, ethLen, start, stuff, value);

		/*
		 * Copy e1000g's behavior:
		 * - Do not assume all the headers are in the same mblk.
		 * - Assume each header is always within one mblk.
		 * - Assume the ethernet header is in the first mblk.
		 */
		ip = mblk->b_rptr + ethLen;
		if (ip >= mblk->b_wptr) {
			mblk = mblk->b_cont;
			ip = mblk->b_rptr;
		}

		if (l3proto == ETHERTYPE_IP) {
			ipLen = IPH_HDR_LENGTH((ipha_t *)ip);
			l4proto = ((ipha_t *)ip)->ipha_protocol;
			l4 = ip + ipLen;
		} else if (l3proto == ETHERTYPE_IPV6) {
			ipLen = sizeof (ip6_t);
			l4proto = ((ip6_t *)ip)->ip6_nxt;
			l4 = ip + ipLen;
		} else {
			VMXNET3_WARN(dp, "Invalid L3 protocol (0x%x) for "
			    "hardware offload\n", l3proto);
			return (-1);
		}

		if (l4 >= mblk->b_wptr) {
			mblk = mblk->b_cont;
			l4 = mblk->b_rptr;
		}

		if (l4proto == IPPROTO_TCP) {
			l4Len = TCP_HDR_LENGTH((tcph_t *)l4);
		} else if (l4proto == IPPROTO_UDP) {
			l4Len = sizeof (struct udphdr);
		} else {
			/*
			 * Note that we should not be doing hardware offload
			 * if ipv6 has extension headers.
			 */
			VMXNET3_WARN(dp, "Invalid L4 protocol (0x%x) for "
			    "hardware offload\n", l4proto);
			return (-1);
		}

		/* Careful, '>' instead of '>=' here */
		if (l4 + l4Len > mblk->b_wptr) {
			mblk = mblk->b_cont;
		}

		*headers_len = ethLen + ipLen + l4Len;

		if (lso_flag & HW_LSO) {
			ASSERT3U(l4proto, ==, IPPROTO_TCP);
			ol->om = VMXNET3_OM_TSO;
			ol->hlen = ethLen + ipLen + l4Len;
			ol->msscof = mss;
		} else if (flags & HCK_PARTIALCKSUM) {
			ol->om = VMXNET3_OM_CSUM;
			ol->hlen = start + ethLen;
			ol->msscof = stuff + ethLen;
		}

		/*
		 * If the headers are not all in the same mblk, we must
		 * pullup the packet so that all the headers end up in the
		 * same tx descriptor. Failure to do so will either cause the
		 * hypervisor to drop the packet or to return an error
		 * requiring a reset of the driver.
		 */
		if (mblk != mp) {
			ret = *headers_len;
		}
	}

	return (ret);
}

/*
 * Map a msg into the Tx command ring of a vmxnet3 device.
 *
 * Returns:
 *	VMXNET3_TX_OK if everything went well.
 *	VMXNET3_TX_RINGFULL if the ring is nearly full.
 *	VMXNET3_TX_PULLUP if the msg is overfragmented.
 *	VMXNET3_TX_FAILURE if there was a DMA, offload error,
 *		or the device is temporarily disabled.
 *
 * Side effects:
 *	The ring is filled if VMXNET3_TX_OK is returned.
 */
static vmxnet3_txstatus
vmxnet3_tx_one(vmxnet3_softc_t *dp, vmxnet3_offload_t *ol, mblk_t *mp)
{
	int ret = VMXNET3_TX_OK;
	unsigned int frags = 0, totLen = 0;
	Vmxnet3_GenericDesc *txDesc;
	uint16_t sopIdx, eopIdx;
	uint8_t sopGen, curGen;
	mblk_t *mblk;
	boolean_t cmdRingEmpty;

	mutex_enter(&dp->txLock);

	if (!dp->devEnabled) {
		mutex_exit(&dp->txLock);
		return (VMXNET3_TX_FAILURE);
	}

	vmxnet3_txqueue_t *txq = &dp->txQueue;
	vmxnet3_cmdring_t *cmdRing = &txq->cmdRing;
	Vmxnet3_TxQueueCtrl *txqCtrl = txq->sharedCtrl;

	sopIdx = eopIdx = cmdRing->next2fill;
	sopGen = cmdRing->gen;
	curGen = !cmdRing->gen;

	cmdRingEmpty = (cmdRing->avail == cmdRing->size);

	for (mblk = mp; mblk != NULL; mblk = mblk->b_cont) {
		unsigned int len = MBLKL(mblk);
		ddi_dma_cookie_t cookie;
		uint_t cookieCount;

		if (len) {
			totLen += len;
		} else {
			continue;
		}

		if (ddi_dma_addr_bind_handle(dp->txDmaHandle, NULL,
		    (caddr_t)mblk->b_rptr, len,
		    DDI_DMA_RDWR | DDI_DMA_STREAMING, DDI_DMA_DONTWAIT, NULL,
		    &cookie, &cookieCount) != DDI_DMA_MAPPED) {
			VMXNET3_WARN(dp, "ddi_dma_addr_bind_handle() failed\n");
			ret = VMXNET3_TX_FAILURE;
			goto error;
		}

		ASSERT(cookieCount);

		do {
			uint64_t addr = cookie.dmac_laddress;
			size_t len = cookie.dmac_size;

			do {
				uint32_t dw2, dw3;
				size_t chunkLen;

				ASSERT(!txq->metaRing[eopIdx].mp);
				ASSERT(cmdRing->avail - frags);

				if (frags >= cmdRing->size - 1 ||
				    (ol->om != VMXNET3_OM_TSO &&
				    frags >= VMXNET3_MAX_TXD_PER_PKT)) {
					VMXNET3_DEBUG(dp, 2,
					    "overfragmented mp (%u)\n", frags);
					(void) ddi_dma_unbind_handle(
					    dp->txDmaHandle);
					ret = VMXNET3_TX_PULLUP;
					goto error;
				}
				if (cmdRing->avail - frags <= 1) {
					txq->mustResched = B_TRUE;
					(void) ddi_dma_unbind_handle(
					    dp->txDmaHandle);
					ret = VMXNET3_TX_RINGFULL;
					goto error;
				}

				if (len > VMXNET3_MAX_TX_BUF_SIZE) {
					chunkLen = VMXNET3_MAX_TX_BUF_SIZE;
				} else {
					chunkLen = len;
				}

				frags++;
				eopIdx = cmdRing->next2fill;

				txDesc = VMXNET3_GET_DESC(cmdRing, eopIdx);
				ASSERT(txDesc->txd.gen != cmdRing->gen);

				/* txd.addr */
				txDesc->txd.addr = addr;
				/* txd.dw2 */
				dw2 = chunkLen == VMXNET3_MAX_TX_BUF_SIZE ?
				    0 : chunkLen;
				dw2 |= curGen << VMXNET3_TXD_GEN_SHIFT;
				txDesc->dword[2] = dw2;
				ASSERT(txDesc->txd.len == len ||
				    txDesc->txd.len == 0);
				/* txd.dw3 */
				dw3 = 0;
				txDesc->dword[3] = dw3;

				VMXNET3_INC_RING_IDX(cmdRing,
				    cmdRing->next2fill);
				curGen = cmdRing->gen;

				addr += chunkLen;
				len -= chunkLen;
			} while (len);

			if (--cookieCount) {
				ddi_dma_nextcookie(dp->txDmaHandle, &cookie);
			}
		} while (cookieCount);

		(void) ddi_dma_unbind_handle(dp->txDmaHandle);
	}

	/* Update the EOP descriptor */
	txDesc = VMXNET3_GET_DESC(cmdRing, eopIdx);
	txDesc->dword[3] |= VMXNET3_TXD_CQ | VMXNET3_TXD_EOP;

	/* Update the SOP descriptor. Must be done last */
	txDesc = VMXNET3_GET_DESC(cmdRing, sopIdx);
	if (ol->om == VMXNET3_OM_TSO && txDesc->txd.len != 0 &&
	    txDesc->txd.len < ol->hlen) {
		ret = VMXNET3_TX_FAILURE;
		goto error;
	}
	txDesc->txd.om = ol->om;
	txDesc->txd.hlen = ol->hlen;
	txDesc->txd.msscof = ol->msscof;
	membar_producer();
	txDesc->txd.gen = sopGen;

	/* Update the meta ring & metadata */
	txq->metaRing[sopIdx].mp = mp;
	txq->metaRing[eopIdx].sopIdx = sopIdx;
	txq->metaRing[eopIdx].frags = frags;
	cmdRing->avail -= frags;
	if (ol->om == VMXNET3_OM_TSO) {
		txqCtrl->txNumDeferred +=
		    (totLen - ol->hlen + ol->msscof - 1) / ol->msscof;
	} else {
		txqCtrl->txNumDeferred++;
	}

	/*
	 * Start deadman timer if we have inserted descriptors into an
	 * empty ring.
	 */
	if (vmxnet3_tx_deadman_max_count != 0 && cmdRingEmpty) {
		txq->deadmanCounter = 0;
		txq->compTimestamp = ddi_get_lbolt();
	}

	VMXNET3_DEBUG(dp, 3, "tx 0x%p on [%u;%u]\n", (void *)mp, sopIdx,
	    eopIdx);

	goto done;

error:
	/* Reverse the generation bits */
	while (sopIdx != cmdRing->next2fill) {
		VMXNET3_DEC_RING_IDX(cmdRing, cmdRing->next2fill);
		txDesc = VMXNET3_GET_DESC(cmdRing, cmdRing->next2fill);
		txDesc->txd.gen = !cmdRing->gen;
	}

done:
	mutex_exit(&dp->txLock);

	return (ret);
}

/*
 * Send packets on a vmxnet3 device.
 *
 * Returns:
 *	NULL in case of success or failure.
 *	The mps to be retransmitted later if the ring is full.
 */
mblk_t *
vmxnet3_tx(void *data, mblk_t *mps)
{
	vmxnet3_softc_t *dp = data;
	vmxnet3_txstatus status = VMXNET3_TX_OK;
	mblk_t *mp;

	ASSERT(mps != NULL);

	do {
		vmxnet3_offload_t ol;
		int pullup_bytes;
		int headers_len = 0;
		boolean_t retried = B_FALSE;

		mp = mps;
		mps = mp->b_next;
		mp->b_next = NULL;

		if (DB_TYPE(mp) != M_DATA) {
			/*
			 * PR #315560: M_PROTO mblks could be passed for
			 * some reason. Drop them because we don't understand
			 * them and because their contents are not Ethernet
			 * frames anyway.
			 */
			ASSERT(B_FALSE);
			freemsg(mp);
			continue;
		}

		pullup_bytes =
		    vmxnet3_tx_prepare_offload(dp, &ol, &headers_len, mp);
		/*
		 * Packet headers need to be in the same mblk when hardware
		 * offload is enabled. If they are not, we pull those headers
		 * into a new mblk.
		 */
		if (pullup_bytes > 0) {
			atomic_inc_32(&dp->tx_pullup_needed);
			/*
			 * Note, we use the older pullupmsg() instead of
			 * msgpullup() as the latter always flattens the whole
			 * message instead of just the requested bytes.
			 * This can be expensive with large LSO packets.
			 */
			if (pullupmsg(mp, pullup_bytes) == 0) {
				atomic_inc_32(&dp->tx_pullup_failed);
				continue;
			}
			ASSERT3U(MBLKL(mp), >=, headers_len);
		} else if (pullup_bytes < 0) {
			freemsg(mp);
			atomic_inc_32(&dp->tx_error);
			continue;
		}

		/*
		 * Not only do the headers have to be in the same mblk, but
		 * that mblk must not cross a page boundary, else it will be
		 * split into multiple tx descriptors. We reallocate the
		 * packet into a new flat mblk if that's the case.
		 */
		if (headers_len > 0 && ((uintptr_t)mp->b_rptr & PAGEOFFSET) +
		    headers_len > PAGESIZE) {
			mblk_t *new_mp = msgpullup(mp, -1);
			atomic_inc_32(&dp->tx_pullup_needed);
			freemsg(mp);
			mp = new_mp;
			if (mp == NULL) {
				atomic_inc_32(&dp->tx_pullup_failed);
				continue;
			}
		}

retry:
		/*
		 * If the headers still cross a page boundary, we give up.
		 */
		if (headers_len > 0 && ((uintptr_t)mp->b_rptr & PAGEOFFSET) +
		    headers_len > PAGESIZE) {
			VMXNET3_WARN(dp, "packet 0x%p dropped as headers cross "
			    "page boundary (hlen %d)\n", (void *)mp,
			    headers_len);
			freemsg(mp);
			atomic_inc_32(&dp->tx_headers_cross_boundary);
			continue;
		}

		/*
		 * Try to map the message in the Tx ring.
		 * This call might fail for non-fatal reasons.
		 */
		status = vmxnet3_tx_one(dp, &ol, mp);
		if (status == VMXNET3_TX_PULLUP && !retried) {
			/*
			 * Try one more time after flattening
			 * the message with msgpullup().
			 */
			if (mp->b_cont != NULL) {
				mblk_t *new_mp = msgpullup(mp, -1);
				atomic_inc_32(&dp->tx_pullup_needed);
				freemsg(mp);
				mp = new_mp;
				if (mp == NULL) {
					atomic_inc_32(&dp->tx_pullup_failed);
					continue;
				}
				retried = B_TRUE;
				goto retry;
			}
		}

		if (status != VMXNET3_TX_OK && status != VMXNET3_TX_RINGFULL) {
			/* Fatal failure, drop it */
			atomic_inc_32(&dp->tx_error);
			freemsg(mp);
		}
	} while (mps && status != VMXNET3_TX_RINGFULL);

	if (status == VMXNET3_TX_RINGFULL) {
		atomic_inc_32(&dp->tx_ring_full);
		mp->b_next = mps;
		mps = mp;
	} else {
		ASSERT(!mps);
	}

	/* Notify the device */
	mutex_enter(&dp->txLock);
	if (dp->devEnabled) {
		vmxnet3_txqueue_t *txq = &dp->txQueue;
		vmxnet3_cmdring_t *cmdRing = &txq->cmdRing;
		Vmxnet3_TxQueueCtrl *txqCtrl = txq->sharedCtrl;

		if (txqCtrl->txNumDeferred >= txqCtrl->txThreshold) {
			txqCtrl->txNumDeferred = 0;
			VMXNET3_BAR0_PUT32(dp,
			    VMXNET3_REG_TXPROD, cmdRing->next2fill);
		}
	}
	mutex_exit(&dp->txLock);

	return (mps);
}

/*
 * If we have detected that the tx ring is not being processed, trigger a
 * reset.
 */
static void
vmxnet3_tx_deadman(vmxnet3_softc_t *dp)
{
	/*
	 * Stop deadman to prevent it from firing again before the reset
	 * is processed.
	 */
	dp->txQueue.compTimestamp = 0;
	dp->txQueue.deadmanCounter = 0;
	VMXNET3_WARN(dp, "tx ring stuck: deadman timeout triggered");
	if (ddi_taskq_dispatch(dp->resetTask, vmxnet3_reset,
	    dp, DDI_NOSLEEP) == DDI_SUCCESS) {
		VMXNET3_WARN(dp, "reset scheduled\n");
	} else {
		VMXNET3_WARN(dp,
		    "reset dispatch failed\n");
	}
}

/*
 * Parse a transmit queue and complete packets.
 *
 * Returns:
 *	B_TRUE if Tx must be updated or B_FALSE if no action is required.
 */
boolean_t
vmxnet3_tx_complete(vmxnet3_softc_t *dp)
{
	ASSERT(mutex_owned(&dp->intrLock));
	ASSERT(dp->devEnabled);

	mutex_enter(&dp->txLock);

	vmxnet3_txqueue_t *txq = &dp->txQueue;
	vmxnet3_cmdring_t *cmdRing = &txq->cmdRing;
	vmxnet3_compring_t *compRing = &txq->compRing;
	Vmxnet3_GenericDesc *compDesc;
	boolean_t completedTx = B_FALSE;
	boolean_t ret = B_FALSE;
	clock_t timestamp;

	compDesc = VMXNET3_GET_DESC(compRing, compRing->next2comp);
	while (compDesc->tcd.gen == compRing->gen) {
		vmxnet3_metatx_t *sopMetaDesc, *eopMetaDesc;
		uint16_t sopIdx, eopIdx;
		mblk_t *mp;

		eopIdx = compDesc->tcd.txdIdx;
		eopMetaDesc = &txq->metaRing[eopIdx];
		sopIdx = eopMetaDesc->sopIdx;
		sopMetaDesc = &txq->metaRing[sopIdx];

		ASSERT(eopMetaDesc->frags);
		cmdRing->avail += eopMetaDesc->frags;

		ASSERT(sopMetaDesc->mp);
		mp = sopMetaDesc->mp;
		freemsg(mp);

		eopMetaDesc->sopIdx = 0;
		eopMetaDesc->frags = 0;
		sopMetaDesc->mp = NULL;

		completedTx = B_TRUE;

		VMXNET3_DEBUG(dp, 3, "cp 0x%p on [%u;%u]\n", (void *)mp, sopIdx,
		    eopIdx);

		VMXNET3_INC_RING_IDX(compRing, compRing->next2comp);
		compDesc = VMXNET3_GET_DESC(compRing, compRing->next2comp);
	}

	if (txq->mustResched && completedTx) {
		txq->mustResched = B_FALSE;
		ret = B_TRUE;
	}

	/*
	 * Update deadman timer. We do this in vmxnet3_tx_complete() instead
	 * of within a timer callback since we are still receiving rx
	 * interrupts even when the tx ring is hung and adding it here makes
	 * the logic simpler and have less impact on performance.
	 */
	if (txq->compTimestamp != 0) {
		if (vmxnet3_tx_deadman_max_count == 0 ||
		    cmdRing->avail == cmdRing->size) {
			/*
			 * If deadman was disabled manually or there are
			 * no more completions pending, stop timer.
			 */
			txq->compTimestamp = 0;
			txq->deadmanCounter = 0;
		} else if (completedTx) {
			/*
			 * Received completions, refresh timer.
			 */
			txq->compTimestamp = ddi_get_lbolt();
			txq->deadmanCounter = 0;
		} else {
			timestamp = ddi_get_lbolt();
			if (txq->compTimestamp +
			    MSEC_TO_TICK(vmxnet3_tx_deadman_timeout_ms) <
			    timestamp) {
				txq->compTimestamp = timestamp;
				txq->deadmanCounter++;
				if (txq->deadmanCounter >=
				    vmxnet3_tx_deadman_max_count) {
					vmxnet3_tx_deadman(dp);
				}
			}
		}
	}

	mutex_exit(&dp->txLock);

	return (ret);
}
