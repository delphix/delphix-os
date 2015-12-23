/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright 2011 Nexenta Systems, Inc. All rights reserved.
 * Copyright (c) 2011, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2014, Joyent, Inc. All rights reserved.
 * Copyright 2014 HybridCluster. All rights reserved.
 */

#include <sys/dmu.h>
#include <sys/dmu_impl.h>
#include <sys/dmu_tx.h>
#include <sys/dbuf.h>
#include <sys/dnode.h>
#include <sys/zfs_context.h>
#include <sys/dmu_objset.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_pool.h>
#include <sys/dsl_synctask.h>
#include <sys/zfs_ioctl.h>
#include <sys/zap.h>
#include <sys/zio_checksum.h>
#include <sys/zfs_znode.h>
#include <zfs_fletcher.h>
#include <sys/avl.h>
#include <sys/ddt.h>
#include <sys/zfs_onexit.h>
#include <sys/dmu_send.h>
#include <sys/dsl_destroy.h>
#include <sys/mooch_byteswap.h>
#include <sys/blkptr.h>
#include <sys/dsl_bookmark.h>
#include <sys/zfeature.h>
#include <sys/bqueue.h>

/* Set this tunable to TRUE to replace corrupt data with 0x2f5baddb10c */
int zfs_send_corrupt_data = B_FALSE;
/*
 * This tunable controls the amount of data (measured in bytes) that will be
 * prefetched by zfs send.  If the main thread is blocking on reads that haven't
 * completed, this variable might need to be increased.  If instead the main
 * thread is issuing new reads because the prefetches have fallen out of the
 * cache, this may need to be decreased.
 */
int zfs_send_queue_length = 16 * 1024 * 1024;
/*
 * This tunable controls the length of the queues that zfs send worker threads
 * use to communicate.  If the send_main_thread is blocking on these queues,
 * this variable may need to be increased.  If there is a significant slowdown
 * at the start of a send as these threads consume all the available IO
 * resources, this variable may need to be decreased.
 */
int zfs_send_no_prefetch_queue_length = 1024 * 1024;
/*
 * These tunables control the fill fraction of the queues by zfs send.  The fill
 * fraction controls the frequency with which threads have to be cv_signaled.
 * If a lot of cpu time is being spent on cv_signal, then these should be tuned
 * down.  If the queues empty before the signalled thread can catch up, then
 * these should be tuned up.
 */
uint64_t zfs_send_queue_ff = 20;
uint64_t zfs_send_no_prefetch_queue_ff = 20;

int zfs_recv_queue_length = 16 * 1024 * 1024;
uint64_t zfs_recv_queue_ff = 20;

/*
 * Controls how often to update the redaction list when performing a redacted
 * zfs send.
 */
uint64_t redaction_list_update_interval_ns = 1000 * 1000 * 1000ULL; /* NS */

/*
 * This controls the number of entries in the buffer the redaction_list_update
 * synctask uses to buffer writes to the redaction list.
 */
int redact_sync_bufsize = 1024;

static char *dmu_recv_tag = "dmu_recv_tag";
const char *recv_clone_name = "%recv";

#define	BEGINNV_REDACT_SNAPS		"redact_snaps"
#define	BEGINNV_REDACT_FROM_SNAPS	"redact_from_snaps"
#define	BEGINNV_RESUME_OBJECT		"resume_object"
#define	BEGINNV_RESUME_OFFSET		"resume_offset"

static inline uint64_t
bp_span_in_blocks(uint8_t indblkshift, uint64_t level)
{
	return (((uint64_t)1) << (level * (indblkshift - SPA_BLKPTRSHIFT)));
}

static inline uint64_t
bp_span(uint32_t datablksz, uint8_t indblkshift, uint64_t level)
{
	return (bp_span_in_blocks(indblkshift, level) * datablksz);
}


static void byteswap_record(dmu_replay_record_t *drr);

struct send_thread_arg {
	bqueue_t	q;
	dsl_dataset_t	*ds;		/* Dataset to traverse */
	redaction_list_t *redaction_list;
	struct send_redact_record *current_record;
	uint64_t	fromtxg;	/* Traverse from this txg */
	objset_t	*to_os;		/* The "to" objset (from thread only) */
	uint64_t	ignore_object;	/* ignore further callbacks on this */
	int		flags;		/* flags to pass to traverse_dataset */
	int		error_code;
	boolean_t	cancel;
	zbookmark_phys_t resume;
};

struct redact_merge_thread_arg {
	struct send_thread_arg	*thread_args;
	uint32_t		num_threads;
	boolean_t		cancel;
	bqueue_t		q;
	uint64_t		send_objset;
};

/*
 * A wrapper around struct redact_block so it can be stored in a list_t.
 */
struct redact_block_list_node {
	redact_block_phys_t	block;
	list_node_t		node;
};

struct redact_bookmark_info {
	redact_block_phys_t	rbi_furthest[TXG_SIZE];
	/* Lists of struct redact_block_list_node. */
	list_t			rbi_blocks[TXG_SIZE];
	boolean_t		rbi_synctasc_txg[TXG_SIZE];
	uint64_t		rbi_latest_synctask_txg;
	redaction_list_t	*rbi_redaction_list;
};

struct send_merge_thread_arg {
	bqueue_t			q;
	objset_t			*os;
	struct send_thread_arg		*from_arg;
	struct send_thread_arg		*to_arg;
	struct redact_merge_thread_arg	*redact_arg;
	int				error;
	boolean_t			cancel;
	struct redact_bookmark_info	rbi;
	/*
	 * If we're resuming a redacted send, then the object/offset from the
	 * resume token may be different from the object/offset that we have
	 * updated the bookmark to.  resume_redact_zb will store the earlier of
	 * the two object/offset pairs, and bookmark_before will be B_TRUE if
	 * resume_redact_zb has the object/offset for resuming the redaction
	 * bookmark, and B_FALSE if resume_redact_zb is storing the
	 * object/offset from the resume token.
	 */
	zbookmark_phys_t		resume_redact_zb;
	boolean_t			bookmark_before;
};

struct send_block_record {
	boolean_t		eos_marker; /* Marks the end of the stream */
	/* Marks that this record should be redacted. */
	boolean_t		redact_marker;
	blkptr_t		bp;
	zbookmark_phys_t	zb;
	uint8_t			indblkshift;
	uint32_t		datablksz;
	dmu_object_type_t	obj_type;
	bqueue_node_t		ln;
};

struct send_redact_record {
	bqueue_node_t		ln;
	boolean_t		eos_marker; /* Marks the end of the stream */
	uint64_t		start_object;
	uint64_t		start_blkid;
	uint64_t		end_object;
	uint64_t		end_blkid;
	uint8_t			indblkshift;
	uint32_t		datablksz;
};

/*
 * The list of data whose inclusion in a send stream can be pending from
 * one call to backup_cb to another.  Multiple calls to dump_free(),
 * dump_freeobjects(), and dump_redact() can be aggregated into a single
 * DRR_FREE, DRR_FREEOBJECTS, or DRR_REDACT replay record.
 */
typedef enum {
	PENDING_NONE,
	PENDING_FREE,
	PENDING_FREEOBJECTS,
	PENDING_REDACT
} dmu_pendop_t;

typedef struct dmu_send_cookie {
	dmu_replay_record_t *dsc_drr;
	vnode_t *dsc_vp;
	offset_t *dsc_off;
	objset_t *dsc_os;
	zio_cksum_t dsc_zc;
	uint64_t dsc_toguid;
	int dsc_err;
	dmu_pendop_t dsc_pending_op;
	uint64_t dsc_featureflags;
	uint64_t dsc_last_data_object;
	uint64_t dsc_last_data_offset;
	uint64_t dsc_resume_object;
	uint64_t dsc_resume_offset;
	boolean_t dsc_sent_begin;
	boolean_t dsc_sent_end;
} dmu_send_cookie_t;

/*
 * The redaction node is a wrapper around the redaction record that is used
 * by the redaction merging thread to sort the records and determine overlaps.
 *
 * It contains two nodes; one sorts the records by their start_zb, and the other
 * sorts the records by their end_zb.
 */
struct redact_node {
	avl_node_t			avl_node_start;
	avl_node_t			avl_node_end;
	struct send_redact_record	*record;
	struct send_thread_arg		*st_arg;
	uint32_t			thread_num;
};

static int
dump_bytes(dmu_send_cookie_t *dscp, void *buf, int len)
{
	dsl_dataset_t *ds = dmu_objset_ds(dscp->dsc_os);
	ssize_t resid; /* have to get resid to get detailed errno */

	/*
	 * The code does not rely on this (len being a multiple of 8).  We keep
	 * this assertion because of the corresponding assertion in
	 * receive_read().  Keeping this assertion ensures that we do not
	 * inadvertently break backwards compatibility (causing the assertion
	 * in receive_read() to trigger on old software).
	 *
	 * Removing the assertions could be rolled into a new feature that uses
	 * data that isn't 8-byte aligned; if the assertions were removed, a
	 * feature flag would have to be added.
	 */

	ASSERT0(len % 8);

	dscp->dsc_err = vn_rdwr(UIO_WRITE, dscp->dsc_vp,
	    (caddr_t)buf, len,
	    0, UIO_SYSSPACE, FAPPEND, RLIM64_INFINITY, CRED(), &resid);

	mutex_enter(&ds->ds_sendstream_lock);
	*dscp->dsc_off += len;
	mutex_exit(&ds->ds_sendstream_lock);

	return (dscp->dsc_err);
}

/*
 * For all record types except BEGIN, fill in the checksum (overlaid in
 * drr_u.drr_checksum.drr_checksum).  The checksum verifies everything
 * up to the start of the checksum itself.
 */
static int
dump_record(dmu_send_cookie_t *dscp, void *payload, int payload_len)
{
	ASSERT3U(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    ==, sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	fletcher_4_incremental_native(dscp->dsc_drr,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    &dscp->dsc_zc);
	if (dscp->dsc_drr->drr_type == DRR_BEGIN) {
		dscp->dsc_sent_begin = B_TRUE;
	} else {
		ASSERT(ZIO_CHECKSUM_IS_ZERO(&dscp->dsc_drr->drr_u.
		    drr_checksum.drr_checksum));
		dscp->dsc_drr->drr_u.drr_checksum.drr_checksum = dscp->dsc_zc;
	}
	if (dscp->dsc_drr->drr_type == DRR_END) {
		dscp->dsc_sent_end = B_TRUE;
	}
	fletcher_4_incremental_native(&dscp->dsc_drr->
	    drr_u.drr_checksum.drr_checksum,
	    sizeof (zio_cksum_t), &dscp->dsc_zc);
	if (dump_bytes(dscp, dscp->dsc_drr, sizeof (dmu_replay_record_t)) != 0)
		return (SET_ERROR(EINTR));
	if (payload_len != 0) {
		fletcher_4_incremental_native(payload, payload_len,
		    &dscp->dsc_zc);
		if (dump_bytes(dscp, payload, payload_len) != 0)
			return (SET_ERROR(EINTR));
	}
	return (0);
}

/*
 * Fill in the drr_free struct, or perform aggregation if the previous record is
 * also a free record, and the two are adjacent.
 *
 * Note that we send free records even for a full send, because we want to be
 * able to receive a full send as a clone, which requires a list of all the free
 * and freeobject records that were generated on the source.
 */
static int
dump_free(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    uint64_t length)
{
	struct drr_free *drrf = &(dscp->dsc_drr->drr_u.drr_free);

	/*
	 * When we receive a free record, dbuf_free_range() assumes
	 * that the receiving system doesn't have any dbufs in the range
	 * being freed.  This is always true because there is a one-record
	 * constraint: we only send one WRITE record for any given
	 * object,offset.  We know that the one-record constraint is
	 * true because we always send data in increasing order by
	 * object,offset.
	 *
	 * If the increasing-order constraint ever changes, we should find
	 * another way to assert that the one-record constraint is still
	 * satisfied.
	 */
	ASSERT(object > dscp->dsc_last_data_object ||
	    (object == dscp->dsc_last_data_object &&
	    offset > dscp->dsc_last_data_offset));

	if (length != -1ULL && offset + length < offset)
		length = -1ULL;

	/*
	 * If there is a pending op, but it's not PENDING_FREE, push it out,
	 * since free block aggregation can only be done for blocks of the
	 * same type (i.e., DRR_FREE records can only be aggregated with
	 * other DRR_FREE records.  DRR_FREEOBJECTS records can only be
	 * aggregated with other DRR_FREEOBJECTS records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_FREE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	if (dscp->dsc_pending_op == PENDING_FREE) {
		/*
		 * There should never be a PENDING_FREE if length is -1
		 * (because dump_dnode is the only place where this
		 * function is called with a -1, and only after flushing
		 * any pending record).
		 */
		ASSERT(length != -1ULL);
		/*
		 * Check to see whether this free block can be aggregated
		 * with pending one.
		 */
		if (drrf->drr_object == object && drrf->drr_offset +
		    drrf->drr_length == offset) {
			drrf->drr_length += length;
			return (0);
		} else {
			/* not a continuation.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}
	/* create a FREE record and make it pending */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_FREE;
	drrf->drr_object = object;
	drrf->drr_offset = offset;
	drrf->drr_length = length;
	drrf->drr_toguid = dscp->dsc_toguid;
	if (length == -1ULL) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
	} else {
		dscp->dsc_pending_op = PENDING_FREE;
	}

	return (0);
}

/*
 * Fill in the drr_redact struct, or perform aggregation if the previous record
 * is also a redaction record, and the two are adjacent.
 */
static int
dump_redact(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    uint64_t length)
{
	struct drr_redact *drrr = &dscp->dsc_drr->drr_u.drr_redact;

	/*
	 * If there is a pending op, but it's not PENDING_REDACT, push it out,
	 * since free block aggregation can only be done for blocks of the
	 * same type (i.e., DRR_REDACT records can only be aggregated with
	 * other DRR_REDACT records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_REDACT) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	if (dscp->dsc_pending_op == PENDING_REDACT) {
		/*
		 * Check to see whether this redacted block can be aggregated
		 * with pending one.
		 */
		if (drrr->drr_object == object && drrr->drr_offset +
		    drrr->drr_length == offset) {
			drrr->drr_length += length;
			return (0);
		} else {
			/* not a continuation.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}
	/* create a REDACT record and make it pending */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_REDACT;
	drrr->drr_object = object;
	drrr->drr_offset = offset;
	drrr->drr_length = length;
	drrr->drr_toguid = dscp->dsc_toguid;
	dscp->dsc_pending_op = PENDING_REDACT;

	return (0);
}

static int
dump_write(dmu_send_cookie_t *dscp, dmu_object_type_t type,
    uint64_t object, uint64_t offset, int lsize, int psize, const blkptr_t *bp,
    void *data)
{
	uint64_t payload_size;
	struct drr_write *drrw = &(dscp->dsc_drr->drr_u.drr_write);

	/*
	 * We send data in increasing object, offset order.
	 * See comment in dump_free() for details.
	 */
	ASSERT(object > dscp->dsc_last_data_object ||
	    (object == dscp->dsc_last_data_object &&
	    offset > dscp->dsc_last_data_offset));
	dscp->dsc_last_data_object = object;
	dscp->dsc_last_data_offset = offset + lsize - 1;

	/*
	 * If there is any kind of pending aggregation (currently either
	 * a grouping of free objects or free blocks), push it out to
	 * the stream, since aggregation can't be done across operations
	 * of different types.
	 */
	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}
	/* write a WRITE record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_WRITE;
	drrw->drr_object = object;
	drrw->drr_type = type;
	drrw->drr_offset = offset;
	drrw->drr_toguid = dscp->dsc_toguid;
	drrw->drr_logical_size = lsize;

	/* only set the compression fields if the buf is compressed */
	if (lsize != psize) {
		ASSERT(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_COMPRESSED);
		ASSERT(!BP_IS_EMBEDDED(bp));
		ASSERT(!BP_SHOULD_BYTESWAP(bp));
		ASSERT(!DMU_OT_IS_METADATA(BP_GET_TYPE(bp)));
		ASSERT3U(BP_GET_COMPRESS(bp), !=, ZIO_COMPRESS_OFF);
		ASSERT3S(psize, >, 0);
		ASSERT3S(lsize, >=, psize);

		drrw->drr_compressiontype = BP_GET_COMPRESS(bp);
		drrw->drr_compressed_size = psize;
		payload_size = drrw->drr_compressed_size;
	} else {
		payload_size = drrw->drr_logical_size;
	}

	if (bp == NULL || BP_IS_EMBEDDED(bp)) {
		/*
		 * There's no pre-computed checksum for partial-block
		 * writes or embedded BP's, so (like
		 * fletcher4-checkummed blocks) userland will have to
		 * compute a dedup-capable checksum itself.
		 */
		drrw->drr_checksumtype = ZIO_CHECKSUM_OFF;
	} else {
		drrw->drr_checksumtype = BP_GET_CHECKSUM(bp);
		if (zio_checksum_table[drrw->drr_checksumtype].ci_flags &
		    ZCHECKSUM_FLAG_DEDUP)
			drrw->drr_checksumflags |= DRR_CHECKSUM_DEDUP;
		DDK_SET_LSIZE(&drrw->drr_key, BP_GET_LSIZE(bp));
		DDK_SET_PSIZE(&drrw->drr_key, BP_GET_PSIZE(bp));
		DDK_SET_COMPRESS(&drrw->drr_key, BP_GET_COMPRESS(bp));
		drrw->drr_key.ddk_cksum = bp->blk_cksum;
	}

	if (dump_record(dscp, data, payload_size) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_write_embedded(dmu_send_cookie_t *dscp, uint64_t object, uint64_t offset,
    int blksz, const blkptr_t *bp)
{
	char buf[BPE_PAYLOAD_SIZE];
	struct drr_write_embedded *drrw =
	    &(dscp->dsc_drr->drr_u.drr_write_embedded);

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (EINTR);
		dscp->dsc_pending_op = PENDING_NONE;
	}

	ASSERT(BP_IS_EMBEDDED(bp));

	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_WRITE_EMBEDDED;
	drrw->drr_object = object;
	drrw->drr_offset = offset;
	drrw->drr_length = blksz;
	drrw->drr_toguid = dscp->dsc_toguid;
	drrw->drr_compression = BP_GET_COMPRESS(bp);
	drrw->drr_etype = BPE_GET_ETYPE(bp);
	drrw->drr_lsize = BPE_GET_LSIZE(bp);
	drrw->drr_psize = BPE_GET_PSIZE(bp);

	decode_embedded_bp_compressed(bp, buf);

	if (dump_record(dscp, buf, P2ROUNDUP(drrw->drr_psize, 8)) != 0)
		return (EINTR);
	return (0);
}

static int
dump_spill(dmu_send_cookie_t *dscp, uint64_t object, int blksz, void *data)
{
	struct drr_spill *drrs = &(dscp->dsc_drr->drr_u.drr_spill);

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	/* write a SPILL record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_SPILL;
	drrs->drr_object = object;
	drrs->drr_length = blksz;
	drrs->drr_toguid = dscp->dsc_toguid;

	if (dump_record(dscp, data, blksz) != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static int
dump_freeobjects(dmu_send_cookie_t *dscp, uint64_t firstobj, uint64_t numobjs)
{
	struct drr_freeobjects *drrfo = &(dscp->dsc_drr->drr_u.drr_freeobjects);

	/*
	 * If there is a pending op, but it's not PENDING_FREEOBJECTS,
	 * push it out, since free block aggregation can only be done for
	 * blocks of the same type (i.e., DRR_FREE records can only be
	 * aggregated with other DRR_FREE records.  DRR_FREEOBJECTS records
	 * can only be aggregated with other DRR_FREEOBJECTS records).
	 */
	if (dscp->dsc_pending_op != PENDING_NONE &&
	    dscp->dsc_pending_op != PENDING_FREEOBJECTS) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}
	if (dscp->dsc_pending_op == PENDING_FREEOBJECTS) {
		/*
		 * See whether this free object array can be aggregated
		 * with pending one
		 */
		if (drrfo->drr_firstobj + drrfo->drr_numobjs == firstobj) {
			drrfo->drr_numobjs += numobjs;
			return (0);
		} else {
			/* can't be aggregated.  Push out pending record */
			if (dump_record(dscp, NULL, 0) != 0)
				return (SET_ERROR(EINTR));
			dscp->dsc_pending_op = PENDING_NONE;
		}
	}

	/* write a FREEOBJECTS record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_FREEOBJECTS;
	drrfo->drr_firstobj = firstobj;
	drrfo->drr_numobjs = numobjs;
	drrfo->drr_toguid = dscp->dsc_toguid;

	dscp->dsc_pending_op = PENDING_FREEOBJECTS;

	return (0);
}

static int
dump_dnode(dmu_send_cookie_t *dscp, uint64_t object, dnode_phys_t *dnp)
{
	struct drr_object *drro = &(dscp->dsc_drr->drr_u.drr_object);

	if (object < dscp->dsc_resume_object) {
		/*
		 * Note: when resuming, we will visit all the dnodes in
		 * the block of dnodes that we are resuming from.  In
		 * this case it's unnecessary to send the dnodes prior to
		 * the one we are resuming from.  We should be at most one
		 * block's worth of dnodes behind the resume point.
		 */
		ASSERT3U(dscp->dsc_resume_object - object, <,
		    1 << (DNODE_BLOCK_SHIFT - DNODE_SHIFT));
		return (0);
	}

	if (dnp == NULL || dnp->dn_type == DMU_OT_NONE)
		return (dump_freeobjects(dscp, object, 1));

	if (dscp->dsc_pending_op != PENDING_NONE) {
		if (dump_record(dscp, NULL, 0) != 0)
			return (SET_ERROR(EINTR));
		dscp->dsc_pending_op = PENDING_NONE;
	}

	/* write an OBJECT record */
	bzero(dscp->dsc_drr, sizeof (dmu_replay_record_t));
	dscp->dsc_drr->drr_type = DRR_OBJECT;
	drro->drr_object = object;
	drro->drr_type = dnp->dn_type;
	drro->drr_bonustype = dnp->dn_bonustype;
	drro->drr_blksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	drro->drr_bonuslen = dnp->dn_bonuslen;
	drro->drr_checksumtype = dnp->dn_checksum;
	drro->drr_compress = dnp->dn_compress;
	drro->drr_toguid = dscp->dsc_toguid;

	if (!(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS) &&
	    drro->drr_blksz > SPA_OLD_MAXBLOCKSIZE)
		drro->drr_blksz = SPA_OLD_MAXBLOCKSIZE;

	if (dump_record(dscp, DN_BONUS(dnp),
	    P2ROUNDUP(dnp->dn_bonuslen, 8)) != 0) {
		return (SET_ERROR(EINTR));
	}

	/* Free anything past the end of the file. */
	if (dump_free(dscp, object, (dnp->dn_maxblkid + 1) *
	    (dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT), -1ULL) != 0)
		return (SET_ERROR(EINTR));
	if (dscp->dsc_err != 0)
		return (SET_ERROR(EINTR));
	return (0);
}

static boolean_t
backup_do_embed(dmu_send_cookie_t *dscp, const blkptr_t *bp)
{
	if (!BP_IS_EMBEDDED(bp))
		return (B_FALSE);

	/*
	 * Compression function must be legacy, or explicitly enabled.
	 */
	if ((BP_GET_COMPRESS(bp) >= ZIO_COMPRESS_LEGACY_FUNCTIONS &&
	    !(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LZ4)))
		return (B_FALSE);

	/*
	 * Embed type must be explicitly enabled.
	 */
	switch (BPE_GET_ETYPE(bp)) {
	case BP_EMBEDDED_TYPE_DATA:
		if (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_EMBED_DATA)
			return (B_TRUE);
		break;
	case BP_EMBEDDED_TYPE_MOOCH_BYTESWAP:
		if (dscp->dsc_featureflags &
		    DMU_BACKUP_FEATURE_EMBED_MOOCH_BYTESWAP)
			return (B_TRUE);
		break;
	default:
		return (B_FALSE);
	}
	return (B_FALSE);
}

/*
 * This function actually handles figuring out what kind of record needs to be
 * dumped, reading the data (which has hopefully been prefetched), and calling
 * the appropriate helper function.
 */
static int
do_dump(dmu_send_cookie_t *dscp, struct send_block_record *data)
{
	dsl_dataset_t *ds = dmu_objset_ds(dscp->dsc_os);
	const blkptr_t *bp = &data->bp;
	const zbookmark_phys_t *zb = &data->zb;
	uint8_t indblkshift = data->indblkshift;
	spa_t *spa = ds->ds_dir->dd_pool->dp_spa;
	dmu_object_type_t type = BP_GET_TYPE(bp);
	int err = 0;

	ASSERT3U(zb->zb_level, >=, 0);

	ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
	    zb->zb_object >= dscp->dsc_resume_object);

	if (zb->zb_object != DMU_META_DNODE_OBJECT &&
	    DMU_OBJECT_IS_SPECIAL(zb->zb_object)) {
		ASSERT(!data->redact_marker);
		return (0);
	} else if (BP_IS_HOLE(bp) &&
	    zb->zb_object == DMU_META_DNODE_OBJECT) {
		ASSERT(!data->redact_marker);
		uint64_t span = bp_span(data->datablksz, indblkshift,
		    zb->zb_level);
		uint64_t dnobj = (zb->zb_blkid * span) >> DNODE_SHIFT;
		err = dump_freeobjects(dscp, dnobj, span >> DNODE_SHIFT);
	} else if (BP_IS_HOLE(bp)) {
		uint64_t span = bp_span(data->datablksz, indblkshift,
		    zb->zb_level);
		uint64_t offset = zb->zb_blkid * span;
		if (data->redact_marker)
			err = dump_redact(dscp, zb->zb_object, offset, span);
		else
			err = dump_free(dscp, zb->zb_object, offset, span);
	} else if (BP_IS_REDACTED(bp)) {
		ASSERT0(zb->zb_level);
		uint64_t span = bp_span(data->datablksz, indblkshift,
		    zb->zb_level);
		uint64_t offset = zb->zb_blkid * span;
		err = dump_redact(dscp, zb->zb_object, offset, span);
	} else if (zb->zb_level > 0 || type == DMU_OT_OBJSET) {
		return (0);
	} else if (type == DMU_OT_DNODE) {
		int blksz = BP_GET_LSIZE(bp);
		arc_flags_t aflags = ARC_FLAG_WAIT;
		uint64_t dnobj = zb->zb_blkid * (blksz >> DNODE_SHIFT);
		arc_buf_t *abuf;

		ASSERT0(zb->zb_level);
		ASSERT(!data->redact_marker);

		if (arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL,
		    &aflags, zb) != 0)
			return (SET_ERROR(EIO));

		dnode_phys_t *blk = abuf->b_data;

		for (int i = 0; i < blksz >> DNODE_SHIFT; i++) {
			err = dump_dnode(dscp, dnobj + i, blk + i);
			if (err != 0)
				break;
		}
		arc_buf_destroy(abuf, &abuf);
	} else if (type == DMU_OT_SA) {
		arc_flags_t aflags = ARC_FLAG_WAIT;
		arc_buf_t *abuf;
		int blksz = BP_GET_LSIZE(bp);
		ASSERT(!data->redact_marker);

		if (arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
		    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL,
		    &aflags, zb) != 0)
			return (SET_ERROR(EIO));

		err = dump_spill(dscp, zb->zb_object, blksz,
		    abuf->b_data);
		arc_buf_destroy(abuf, &abuf);
	} else if (data->redact_marker) {
		ASSERT0(zb->zb_level);
		ASSERT(zb->zb_object > dscp->dsc_resume_object ||
		    (zb->zb_object == dscp->dsc_resume_object &&
		    zb->zb_blkid * data->datablksz >= dscp->dsc_resume_offset));
		err = dump_redact(dscp, zb->zb_object,  zb->zb_blkid *
		    data->datablksz, data->datablksz);
	} else if (backup_do_embed(dscp, bp)) {
		/* it's an embedded level-0 block of a regular object */
		ASSERT0(zb->zb_level);
		err = dump_write_embedded(dscp, zb->zb_object,
		    zb->zb_blkid * data->datablksz, data->datablksz, bp);
	} else {
		ASSERT0(zb->zb_level);
		ASSERT(zb->zb_object > dscp->dsc_resume_object ||
		    (zb->zb_object == dscp->dsc_resume_object &&
		    zb->zb_blkid * data->datablksz >= dscp->dsc_resume_offset));
		/* it's a level-0 block of a regular object */
		arc_flags_t aflags = ARC_FLAG_WAIT;
		arc_buf_t *abuf;
		uint64_t offset;

		/*
		 * If we have large blocks stored on disk but the send flags
		 * don't allow us to send large blocks, we split the data from
		 * the arc buf into chunks.
		 */
		boolean_t split_large_blocks =
		    data->datablksz > SPA_OLD_MAXBLOCKSIZE &&
		    !(dscp->dsc_featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS);
		/*
		 * We should only request compressed data from the ARC if all
		 * the following are true:
		 *  - stream compression was requested
		 *  - we aren't splitting large blocks into smaller chunks
		 *  - the data won't need to be byteswapped before sending
		 *  - this isn't an embedded block
		 *  - this isn't metadata (if receiving on a different endian
		 *    system it can be byteswapped more easily)
		 */
		boolean_t request_compressed =
		    (dscp->dsc_featureflags & DMU_BACKUP_FEATURE_COMPRESSED) &&
		    !split_large_blocks && !BP_SHOULD_BYTESWAP(bp) &&
		    !BP_IS_EMBEDDED(bp) && !DMU_OT_IS_METADATA(BP_GET_TYPE(bp));

		ASSERT0(zb->zb_level);
		ASSERT(zb->zb_object > dscp->dsc_resume_object ||
		    (zb->zb_object == dscp->dsc_resume_object &&
		    zb->zb_blkid * data->datablksz >= dscp->dsc_resume_offset));

		ASSERT0(zb->zb_level);
		ASSERT(zb->zb_object > dscp->dsc_resume_object ||
		    (zb->zb_object == dscp->dsc_resume_object &&
		    zb->zb_blkid * data->datablksz >= dscp->dsc_resume_offset));

		if (BP_IS_EMBEDDED(bp) &&
		    BPE_GET_ETYPE(bp) == BP_EMBEDDED_TYPE_MOOCH_BYTESWAP) {
			objset_t *origin_objset;
			dmu_buf_t *origin_db;
			uint64_t origin_obj;

			VERIFY0(dmu_objset_mooch_origin(dscp->dsc_os,
			    &origin_objset));
			VERIFY0(dmu_objset_mooch_obj_refd(dscp->dsc_os,
			    zb->zb_object, &origin_obj));
			err = dmu_buf_hold(origin_objset, origin_obj,
			    zb->zb_blkid * data->datablksz, FTAG, &origin_db,
			    0);
			ASSERT3U(data->datablksz, ==, origin_db->db_size);
			if (err == 0) {
				abuf = arc_alloc_buf(spa, &abuf, ARC_BUFC_DATA,
				    origin_db->db_size);
				mooch_byteswap_reconstruct(origin_db,
				    abuf->b_data, bp);
				dmu_buf_rele(origin_db, FTAG);
			}
		} else {
			enum zio_flag zioflags = ZIO_FLAG_CANFAIL;

			ASSERT3U(data->datablksz, ==, BP_GET_LSIZE(bp));

			if (request_compressed)
				zioflags |= ZIO_FLAG_RAW;

			err = arc_read(NULL, spa, bp, arc_getbuf_func, &abuf,
			    ZIO_PRIORITY_ASYNC_READ, zioflags, &aflags, zb);
		}
		if (err != 0) {
			if (zfs_send_corrupt_data) {
				/* Send a block filled with 0x"zfs badd bloc" */
				abuf = arc_alloc_buf(spa, &abuf, ARC_BUFC_DATA,
				    data->datablksz);
				uint64_t *ptr;
				for (ptr = abuf->b_data;
				    (char *)ptr < (char *)abuf->b_data +
				    data->datablksz; ptr++)
					*ptr = 0x2f5baddb10cULL;
			} else {
				return (SET_ERROR(EIO));
			}
		}

		offset = zb->zb_blkid * data->datablksz;

		if (split_large_blocks) {
			ASSERT3U(arc_get_compression(abuf), ==,
			    ZIO_COMPRESS_OFF);
			char *buf = abuf->b_data;
			while (data->datablksz > 0 && err == 0) {
				int n = MIN(data->datablksz,
				    SPA_OLD_MAXBLOCKSIZE);
				err = dump_write(dscp, type, zb->zb_object,
				    offset, n, n, NULL, buf);
				offset += n;
				buf += n;
				data->datablksz -= n;
			}
		} else {
			err = dump_write(dscp, type, zb->zb_object, offset,
			    data->datablksz, arc_buf_size(abuf), bp,
			    abuf->b_data);
		}
		arc_buf_destroy(abuf, &abuf);
	}

	ASSERT(err == 0 || err == EINTR);
	return (err);
}

/*
 * This thread finds any blocks in the given object between start and start +
 * len (or the end of the file, if len is 0), and creates artificial records for
 * them.  This will force the main thread to use the to_ds's version of the
 * data.  It does this via dmu_offset_next, which intelligently traverses the
 * tree using the blkfill field in the blkptrs.
 */
static int
enqueue_block_range(struct send_thread_arg *sta,
    uint64_t object, uint64_t offset, uint64_t len)
{
	objset_t *to_os = sta->to_os;
	uint64_t end;
	int err = 0;
	dmu_object_info_t doi;

	ASSERT(object != 0);

	if (len == 0)
		end = UINT64_MAX;
	else
		end = offset + len;

	err = dmu_object_info(to_os, object, &doi);
	if (err != 0)
		return (err);

	/*
	 * When we are traversing the tosnap, we will always resume from
	 * the exact resume point.  However, when we are traversing the
	 * fromsnap, we may resume from a bookmark that is earlier than
	 * we intended, if there is not a block pointer that starts
	 * exactly at the resume point.  This is possible because the
	 * resume point corresponds to a bookmark in the tosnap, and the
	 * fromsnap can be arbitrarily different from the tosnap.
	 *
	 * To deal with this, we must adjust the records that we enqueue,
	 * so that do_dump() does not see any bookmarks that are before the
	 * resume point.
	 */

	if (object < sta->resume.zb_object)
		return (0);

	if (object == sta->resume.zb_object &&
	    offset / doi.doi_data_block_size < sta->resume.zb_blkid) {
		/*
		 * We are trying to enqueue a range that is before the
		 * resume point.  If the range is entirely before the resume
		 * point, ignore it.  Otherwise, start enqueing at the
		 * resume point.
		 */
		uint64_t resume_off =
		    sta->resume.zb_blkid * doi.doi_data_block_size;
		if (end <= resume_off)
			return (0);
		offset = resume_off;
		/*
		 * Note: Because we've advanced "offset", "len" no longer
		 * reflects the range to enqueue, and should not be used
		 * past this point.  "end" should be used instead.
		 */
	}

	err = dmu_offset_next(to_os, object, B_FALSE, &offset);
	while (offset < end && err == 0) {
		struct send_block_record *record;

		record = kmem_zalloc(sizeof (*record), KM_SLEEP);
		record->eos_marker = B_FALSE;
		record->zb.zb_objset = 0;
		record->zb.zb_object = object;
		record->zb.zb_level = 0;
		record->zb.zb_blkid = offset / doi.doi_data_block_size;
		record->indblkshift =
		    highbit64(doi.doi_metadata_block_size) - 1;
		record->datablksz = doi.doi_data_block_size;

		bqueue_enqueue(&sta->q, record, sizeof (*record));
		offset += doi.doi_data_block_size;
		err = dmu_offset_next(to_os, object, B_FALSE, &offset);
	}
	if (err == ESRCH)
		err = 0;
	return (err);
}

static int
enqueue_whole_object(struct send_thread_arg *sta, uint64_t object)
{
	int err = enqueue_block_range(sta, object, 0, 0);
	if (err == ENOENT)
		err = 0;
	return (err);
}

/*
 * This function handles some of the special cases described in send_cb.
 * If a hole is created in the meta-dnode, this thread calls
 * enqueue_whole_object on every object that is allocated in the
 * corresponding range in the to_ds.  It finds these objects by using
 * dmu_object_next, which uses the blkfill field of the blkptrs to
 * efficiently traverse the tree.
 *
 * If a hole is created inside an object, we calculate the range it
 * covers, and use equiv_find to fabricate records for any data blocks
 * that might exist in to_ds.
 */
static int
enqueue_holes(struct send_thread_arg *sta, const zbookmark_phys_t *zb,
    const blkptr_t *bp, uint8_t indblkshift, uint32_t datablksz)
{
	objset_t *to_os = sta->to_os;
	uint64_t span = bp_span(datablksz, indblkshift, zb->zb_level);
	uint64_t blkid = zb->zb_blkid;
	int err = 0;
	if (zb->zb_object == DMU_META_DNODE_OBJECT && BP_IS_HOLE(bp)) {
		int epb = span >> DNODE_SHIFT; /* entries per block */

		for (uint64_t obj = blkid * epb;
		    err == 0 && obj < (blkid + 1) * epb;
		    err = dmu_object_next(to_os, &obj, B_FALSE, 0)) {
			/*
			 * Object 0 is invalid (used to specify
			 * the META_DNODE object).
			 */
			if (obj == 0)
				obj = 1;
			err = enqueue_whole_object(sta, obj);
			/*
			 * Note: we must explicitly "break" so that
			 * dmu_object_next() does not overwrite "err".
			 */
			if (err != 0)
				break;
		}

		if (err == ESRCH)
			err = 0;
	} else if (BP_IS_HOLE(bp) && zb->zb_level > 0) {
		err = enqueue_block_range(sta, zb->zb_object, span * blkid,
		    span);
		if (err == ENOENT)
			err = 0;
	}

	return (err);
}

/*
 * This is the callback function to traverse_dataset that acts as the worker
 * thread for dmu_send_impl.  This thread manages some of the special cases that
 * come up so dmu_send_impl doesn't have to worry about them.  These special
 * cases only apply to the from_ds.
 *
 * The first case is if a hole is created in the meta-dnode.  This means that
 * some block of dnodes was unallocated in the from_ds.  We need to go through
 * each object in that range that is present in the to_ds and manually traverse
 * it, because the to_ds may not do so if those objects have not been modified
 * since the common ancestor.  This case is handled in enqueue_holes_prefetch.
 *
 * The second case is when one object is freed in the from_ds.  We need to
 * manually traverse the version of the object in the to_ds, because to_ds
 * thread won't necessarily do so.  We call hole_object to handle
 * this if we find an unallocated dnode that is allocated in the to_ds.
 *
 * The third case is if a hole is created inside an object.  Again, we need to
 * manually traverse that area.  This is also handled in enqueue_holes_prefetch.
 */
/*ARGSUSED*/
static int
send_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const struct dnode_phys *dnp, void *arg)
{
	struct send_thread_arg *sta = arg;
	objset_t *to_os = sta->to_os;
	struct send_block_record *record;
	int err = 0;

	ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
	    zb->zb_object >= sta->resume.zb_object);

	if (sta->cancel)
		return (SET_ERROR(EINTR));

	if (sta->ignore_object != 0 && zb->zb_object == sta->ignore_object)
		return (0);

	if (bp == NULL) {
		ASSERT3U(zb->zb_level, ==, ZB_DNODE_LEVEL);

		/* Ignore if we are traversing the tosnap. */
		if (to_os == NULL || zb->zb_object == 0)
			return (0);

		/*
		 * If this object is fundamentally different
		 * from the corresponding object in the tosnap,
		 * then we must enqueue the whole object, and ignore
		 * any further callbacks for this object.  Any callbacks
		 * we receive later that we do not ignore, then, must be
		 * for a compatible object.
		 */
		if (dnp->dn_type == DMU_OT_NONE) {
			sta->ignore_object = zb->zb_object;
			return (enqueue_whole_object(sta, zb->zb_object));
		}

		dmu_object_info_t tosnap_obj_info;
		err = dmu_object_info(to_os, zb->zb_object, &tosnap_obj_info);
		if (err == ENOENT)
			return (0);
		if (err != 0)
			return (err);
		if (tosnap_obj_info.doi_data_block_size !=
		    dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT ||
		    tosnap_obj_info.doi_metadata_block_size !=
		    1 << dnp->dn_indblkshift) {
			sta->ignore_object = zb->zb_object;
			return (enqueue_whole_object(sta, zb->zb_object));
		}
		return (0);
	} else if (zb->zb_level < 0) {
		return (0);
	}

	record = kmem_zalloc(sizeof (struct send_block_record), KM_SLEEP);
	record->eos_marker = B_FALSE;
	record->bp = *bp;
	record->zb = *zb;
	record->indblkshift = dnp->dn_indblkshift;
	record->datablksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	record->obj_type = dnp->dn_type;
	bqueue_enqueue(&sta->q, record, sizeof (*record));

	/*
	 * We also need to handle a special case: If there is a new hole in the
	 * from_ds that was not modified since the common ancestor in the to_ds,
	 * we have to iterate over the data in the to_ds to get the blocks to
	 * send so that we can recreate the to_ds.  This function,
	 * enqueue_holes, handles both that case.  We only call it when to_os is
	 * passed in because that is how we know we're the thread handling the
	 * from_ds.
	 */
	if (to_os != NULL) {
		err = enqueue_holes(sta, zb, bp, dnp->dn_indblkshift,
		    dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT);
	}
	return (err);
}

static inline unsigned int
redact_block_buf_num_entries(unsigned int size)
{
	return (size / sizeof (redact_block_phys_t));
}

/*
 * This function calculates the offset of the last entry in the array of
 * redact_block_phys_t.  If we're reading the redaction list into buffers of
 * size bufsize, then for all but the last buffer, the last valid entry in the
 * array will be the last entry in the array.  However, for the last buffer, any
 * amount of it may be filled.  Thus, we check to see if we're looking at the
 * last buffer in the redaction list, and if so, we return the total number of
 * entries modulo the number of entries per buffer.  Otherwise, we return the
 * number of entries per buffer minus one.
 */
static inline unsigned int
last_entry(redaction_list_t *rl, unsigned int bufsize, uint64_t bufid)
{
	if (bufid == (rl->rl_phys->rlp_num_entries - 1) /
	    redact_block_buf_num_entries(bufsize)) {
		return ((rl->rl_phys->rlp_num_entries - 1) %
		    redact_block_buf_num_entries(bufsize));
	}
	return (redact_block_buf_num_entries(bufsize) - 1);
}

/*
 * Compare the redact_block_phys_t to the bookmark. If the last block in the
 * redact_block_phys_t is before the bookmark, return -1.  If the first block in
 * the redact_block_phys_t is after the bookmark, return 1.  Otherwise, the
 * bookmark is inside the range of the redact_block_phys_t, and we return 0.
 */
int
redact_block_zb_compare(redact_block_phys_t *first,
    zbookmark_phys_t *second)
{
	/*
	 * If the block_phys is for a previous object, or the last block in the
	 * block_phys is strictly before the block in the bookmark, the
	 * block_phys is earlier.
	 */
	if (first->rbp_object < second->zb_object ||
	    (first->rbp_object == second->zb_object &&
	    first->rbp_blkid + (redact_block_get_count(first) - 1) <
	    second->zb_blkid))
		return (-1);

	/*
	 * If the bookmark is for a previous object, or the block in the
	 * bookmark is strictly before the first block in the block_phys, the
	 * bookmark is earlier.
	 */
	if (first->rbp_object > second->zb_object ||
	    (first->rbp_object == second->zb_object &&
	    first->rbp_blkid > second->zb_blkid))
		return (1);

	return (0);
}

/*
 * Traverse the redaction list in the provided object, and create
 * send_block_records for each entry we find. Don't send any records before
 * resume.
 */
static int
redaction_list_traverse(bqueue_t *q, objset_t *os, redaction_list_t *rl,
    zbookmark_phys_t *resume, boolean_t *cancel)
{
	objset_t *mos = spa_meta_objset(dmu_objset_spa(os));
	redact_block_phys_t *buf;
	unsigned int bufsize = SPA_OLD_MAXBLOCKSIZE;
	int err = 0;

	ASSERT3P(os, !=, NULL);

	/*
	 * The redaction list is incomplete; when we finish a send, we update
	 * the last object and offset to UINT64_MAX.  This happens when a send
	 * fails; we leave the partial redaction list around so the send can be
	 * resumed.
	 */
	if (rl->rl_phys->rlp_last_object != UINT64_MAX ||
	    rl->rl_phys->rlp_last_blkid != UINT64_MAX)
		return (EINVAL);

	/*
	 * Binary search for the point to resume from.  The goal is to minimize
	 * the number of disk reads we have to perform.
	 */
	buf = kmem_alloc(bufsize, KM_SLEEP);
	uint64_t maxbufid = (rl->rl_phys->rlp_num_entries - 1) /
	    redact_block_buf_num_entries(bufsize);
	uint64_t minbufid = 0;
	while (resume != NULL && maxbufid - minbufid >= 1) {
		ASSERT3U(maxbufid, >, minbufid);
		uint64_t midbufid = minbufid + ((maxbufid - minbufid) / 2);
		err = dmu_read(mos, rl->rl_object, midbufid * bufsize, bufsize,
		    buf, DMU_READ_NO_PREFETCH);
		if (err != 0)
			break;

		int cmp0 = redact_block_zb_compare(&buf[0], resume);
		int cmpn = redact_block_zb_compare(
		    &buf[last_entry(rl, bufsize, maxbufid)], resume);

		/*
		 * If the first block is before or equal to the resume point,
		 * and the last one is equal or after, then the resume point is
		 * in this buf, and we should start here.
		 */
		if (cmp0 <= 0 && cmpn >= 0)
			break;

		if (cmp0 > 0)
			maxbufid = midbufid - 1;
		else if (cmpn < 0)
			minbufid = midbufid + 1;
		else
			panic("No progress in binary search for resume point");
	}

	for (uint64_t curidx = minbufid * redact_block_buf_num_entries(bufsize);
	    err == 0 && !*cancel && curidx < rl->rl_phys->rlp_num_entries;
	    curidx++) {
		/*
		 * We read in the redaction list one block at a time.  Once we
		 * finish with all the entries in a given block, we read in a
		 * new one.  The predictive prefetcher will take care of any
		 * prefetching, and this code shouldn't be the bottleneck, so we
		 * don't need to do manual prefetching.
		 */
		if (curidx % redact_block_buf_num_entries(bufsize) == 0) {
			err = dmu_read(mos, rl->rl_object, curidx *
			    sizeof (*buf), bufsize, buf,
			    DMU_READ_PREFETCH);
			if (err != 0)
				break;
		}
		redact_block_phys_t *rb = &buf[curidx %
		    redact_block_buf_num_entries(bufsize)];
		/*
		 * If resume is non-null, we should either not send the data, or
		 * null out resume so we don't have to keep doing these
		 * comparisons.
		 */
		if (resume != NULL) {
			if (redact_block_zb_compare(rb, resume) < 0) {
				continue;
			} else {
				/*
				 * If the place to resume is in the middle of
				 * the range described by this
				 * redact_block_phys, then modify the
				 * redact_block_phys in memory so we generate
				 * the right records.
				 */
				if (resume->zb_object == rb->rbp_object &&
				    resume->zb_blkid > rb->rbp_blkid) {
					uint64_t diff = resume->zb_blkid -
					    rb->rbp_blkid;
					rb->rbp_blkid = resume->zb_blkid;
					redact_block_set_count(rb,
					    redact_block_get_count(rb) - diff);
				}
				resume = NULL;
			}
		}

		for (uint64_t i = 0; i < redact_block_get_count(rb); i++)  {
			struct send_block_record *data;
			data = kmem_zalloc(sizeof (*data), KM_SLEEP);
			SET_BOOKMARK(&data->zb, 0,
			    rb->rbp_object, 0, rb->rbp_blkid + i);
			data->datablksz = redact_block_get_size(rb);
			/*
			 * We only redact user data, so we know that this object
			 * contained plain file contents.
			 */
			data->obj_type = DMU_OT_PLAIN_FILE_CONTENTS;
			bqueue_enqueue(q, data, sizeof (*data));
		}
	}
	kmem_free(buf, bufsize);

	return (err);
}

/*
 * This function kicks off the traverse_dataset.  It also handles setting the
 * error code of the thread in case something goes wrong, and pushes the End of
 * Stream record when the traverse_dataset call has finished.  If there is no
 * dataset to traverse, the thread immediately pushes End of Stream marker.
 */
static void
send_traverse_thread(void *arg)
{
	struct send_thread_arg *st_arg = arg;
	int err = 0;
	struct send_block_record *data;

	if (st_arg->ds != NULL) {
		ASSERT3P(st_arg->redaction_list, ==, NULL);
		err = traverse_dataset_resume(st_arg->ds,
		    st_arg->fromtxg, &st_arg->resume,
		    st_arg->flags, send_cb, st_arg);
	} else if (st_arg->redaction_list != NULL) {
		err = redaction_list_traverse(&st_arg->q, st_arg->to_os,
		    st_arg->redaction_list, &st_arg->resume, &st_arg->cancel);
	}

	if (err != EINTR)
		st_arg->error_code = err;
	data = kmem_zalloc(sizeof (*data), KM_SLEEP);
	data->eos_marker = B_TRUE;
	bqueue_enqueue(&st_arg->q, data, sizeof (*data));
	bqueue_flush(&st_arg->q);
}

/*
 * We've found a new redaction candidate.  In order to improve performance, we
 * coalesce these blocks when they're adjacent to each other.  This function
 * handles that.  If the new candidate block range is immediately after the
 * range we're building, coalesce it into the range we're building.  Otherwise,
 * put the record we're building on the queue, and update the build pointer to
 * point to the new record.
 */
static void
redact_record_merge_enqueue(bqueue_t *q, struct send_redact_record **build,
    struct send_redact_record *new)
{
	if (new->eos_marker) {
		if (*build != NULL)
			bqueue_enqueue(q, *build, sizeof (*build));
		bqueue_enqueue(q, new, sizeof (*new));
		return;
	}
	if (*build == NULL) {
		*build = new;
		return;
	}
	struct send_redact_record *curbuild = *build;
	if ((curbuild->end_object == new->start_object &&
	    curbuild->end_blkid + 1 == new->start_blkid) ||
	    (curbuild->end_object + 1 == new->start_object &&
	    curbuild->end_blkid == UINT64_MAX && new->start_blkid == 0)) {
		curbuild->end_object = new->end_object;
		curbuild->end_blkid = new->end_blkid;
		kmem_free(new, sizeof (*new));
	} else {
		bqueue_enqueue(q, curbuild, sizeof (*curbuild));
		*build = new;
	}
}

/*
 * This is the callback function to traverse_dataset for the redaction threads
 * for dmu_send_impl.  This thread is responsible for creating redaction records
 * for all the data that is modified by the snapshots we're redacting with
 * respect to.  Redaction records represent ranges of data that have been
 * modified by one of our redaction snapshots, and are stored in the
 * send_redact_record struct. We need to create redaction records for three
 * cases:
 *
 * First, if there's a normal write, we need to create a redaction record for
 * that block.
 *
 * Second, if there's a hole, we need to create a redaction record that covers
 * the whole range of the hole.  If the hole is in the meta-dnode, it must cover
 * every block in all of the objects in the hole.
 *
 * Third, if there is a deleted object, we need to create a redaction record for
 * all of the blocks in that object.
 *
 * While redaction is best understood as "only send blocks referenced by one of
 * the redaction snapshots", our existing infrastructure only allows us to
 * easily detect blocks that the redaction snapshots no longer reference.  As a
 * result, we have to find the intersection of the sets of no-longer-referenced
 * blocks across all the redaction snapshots; the complement of that set is the
 * set of blocks we can send.
 */
/*ARGSUSED*/
static int
redact_cb(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const struct dnode_phys *dnp, void *arg)
{
	struct send_thread_arg *sta = arg;
	struct send_redact_record *record;

	ASSERT(zb->zb_object == DMU_META_DNODE_OBJECT ||
	    zb->zb_object >= sta->resume.zb_object);

	if (sta->cancel)
		return (SET_ERROR(EINTR));

	/*
	 * If we're visiting a dnode, we need to handle the case where the
	 * object has been deleted.
	 */
	if (bp == NULL) {
		ASSERT3U(zb->zb_level, ==, ZB_DNODE_LEVEL);

		if (zb->zb_object == 0)
			return (0);

		/*
		 * If the object has been deleted, redact all of the blocks in
		 * it.
		 */
		if (dnp->dn_type == DMU_OT_NONE) {
			sta->ignore_object = zb->zb_object;
			record = kmem_zalloc(sizeof (struct send_redact_record),
			    KM_SLEEP);

			record->eos_marker = B_FALSE;
			record->start_object = record->end_object =
			    zb->zb_object;
			record->start_blkid = 0;
			record->end_blkid = UINT64_MAX;
			redact_record_merge_enqueue(&sta->q,
			    &sta->current_record, record);
		}
		return (0);
	} else if (zb->zb_level < 0) {
		return (0);
	} else if (zb->zb_level > 0 && !BP_IS_HOLE(bp)) {
		/*
		 * If this is an indirect block, but not a hole, it doesn't
		 * provide any useful information for redaction, so ignore it.
		 */
		return (0);
	}

	/*
	 * At this point, there are two options left for the type of block we're
	 * looking at.  Either this is a hole (which could be in the dnode or
	 * the meta-dnode), or it's a level 0 block of some sort.  If it's a
	 * hole, we create a redaction record that covers the whole range.  If
	 * the hole is in a dnode, we need to redact all the blocks in that
	 * hole.  If the hole is in the meta-dnode, we instead need to redact
	 * all blocks in every object covered by that hole.  If it's a level 0
	 * block, we only need to redact that single block.
	 */
	record = kmem_zalloc(sizeof (struct send_redact_record), KM_SLEEP);
	record->eos_marker = B_FALSE;

	record->start_object = record->end_object = zb->zb_object;
	if (BP_IS_HOLE(bp)) {
		record->start_blkid = zb->zb_blkid *
		    bp_span_in_blocks(dnp->dn_indblkshift, zb->zb_level);

		record->end_blkid = ((zb->zb_blkid + 1) *
		    bp_span_in_blocks(dnp->dn_indblkshift, zb->zb_level)) - 1;

		if (zb->zb_object == DMU_META_DNODE_OBJECT) {
			record->start_object = record->start_blkid *
			    ((SPA_MINBLOCKSIZE * dnp->dn_datablkszsec) /
			    DNODE_SIZE);
			record->start_blkid = 0;
			record->end_object = ((record->end_blkid +
			    1) * ((SPA_MINBLOCKSIZE * dnp->dn_datablkszsec) /
			    DNODE_SIZE)) - 1;
			record->end_blkid = UINT64_MAX;
		}
	} else if (zb->zb_level != 0) {
		kmem_free(record, sizeof (*record));
		return (0);
	} else {
		record->start_blkid = record->end_blkid = zb->zb_blkid;
	}
	record->indblkshift = dnp->dn_indblkshift;
	record->datablksz = dnp->dn_datablkszsec << SPA_MINBLOCKSHIFT;
	redact_record_merge_enqueue(&sta->q, &sta->current_record, record);

	return (0);
}


/*
 * This function kicks off the traverse_dataset call for one of the snapshots
 * we're redacting with respect to.  It also handles setting error codes when
 * something goes wrong, and pushing an End of Stream record when the traversal
 * has finished.
 */
static void
redact_traverse_thread(void *arg)
{
	struct send_thread_arg *st_arg = arg;
	int err;
	struct send_redact_record *data;

	err = traverse_dataset_resume(st_arg->ds, st_arg->fromtxg,
	    &st_arg->resume, st_arg->flags, redact_cb, st_arg);

	if (err != EINTR)
		st_arg->error_code = err;

	data = kmem_zalloc(sizeof (*data), KM_SLEEP);
	data->eos_marker = B_TRUE;
	redact_record_merge_enqueue(&st_arg->q, &st_arg->current_record, data);
	bqueue_flush(&st_arg->q);
}

static inline void
create_zbookmark_from_obj_off(zbookmark_phys_t *zb, uint64_t object,
    uint64_t blkid)
{
	zb->zb_object = object;
	zb->zb_level = 0;
	zb->zb_blkid = blkid;
}

/*
 * This is a utility function that can do the comparison for the start or ends
 * of the ranges in a send_redact_record.
 */
static int
redact_range_compare(uint64_t obj1, uint64_t off1, uint32_t dbss1,
    uint64_t obj2, uint64_t off2, uint32_t dbss2)
{
	zbookmark_phys_t z1, z2;
	create_zbookmark_from_obj_off(&z1, obj1, off1);
	create_zbookmark_from_obj_off(&z2, obj2, off2);

	return (zbookmark_compare(dbss1 >> SPA_MINBLOCKSHIFT, 0,
	    dbss2 >> SPA_MINBLOCKSHIFT, 0, &z1, &z2));
}

/*
 * Utility function that compares two redaction records to determine if any part
 * of the "from" record is before any part of the "to" record. Also causes End
 * of Stream redaction records to compare after all others, so that the
 * redaction merging logic can stay simple.
 */
static boolean_t
redact_record_before(const struct send_redact_record *from,
    const struct send_redact_record *to)
{
	if (from->eos_marker == B_TRUE)
		return (B_FALSE);
	else if (to->eos_marker == B_TRUE)
		return (B_TRUE);
	return (redact_range_compare(from->start_object, from->start_blkid,
	    from->datablksz, to->end_object, to->end_blkid,
	    to->datablksz) <= 0);
}


/*
 * Compare two redaction records by their range's start location.  Also makes
 * eos records always compare last.  We use the thread number in the redact_node
 * to ensure that records do not compare equal (which is not allowed in our avl
 * trees).
 */
static int
redact_node_compare_start(const void *arg1, const void *arg2)
{
	const struct redact_node *rn1 = arg1;
	const struct redact_node *rn2 = arg2;
	const struct send_redact_record *srr1 = rn1->record;
	const struct send_redact_record *srr2 = rn2->record;
	if (srr1->eos_marker)
		return (1);
	if (srr2->eos_marker)
		return (-1);

	int cmp = redact_range_compare(srr1->start_object, srr1->start_blkid,
	    srr1->datablksz, srr2->start_object, srr2->start_blkid,
	    srr2->datablksz);
	if (cmp == 0)
		cmp = (rn1->thread_num < rn2->thread_num ? -1 : 1);
	return (cmp);
}

/*
 * Compare two redaction records by their range's end location.  Also makes
 * eos records always compare last.  We use the thread number in the redact_node
 * to ensure that records do not compare equal (which is not allowed in our avl
 * trees).
 */
static int
redact_node_compare_end(const void *arg1, const void *arg2)
{
	const struct redact_node *rn1 = arg1;
	const struct redact_node *rn2 = arg2;
	const struct send_redact_record *srr1 = rn1->record;
	const struct send_redact_record *srr2 = rn2->record;
	if (srr1->eos_marker)
		return (1);
	if (srr2->eos_marker)
		return (-1);

	int cmp = redact_range_compare(srr1->end_object, srr1->end_blkid,
	    srr1->datablksz, srr2->end_object, srr2->end_blkid,
	    srr2->datablksz);
	if (cmp == 0)
		cmp = (rn1->thread_num < rn2->thread_num ? -1 : 1);
	return (cmp);
}

/*
 * Pop a new redaction record off the queue, check that the records are in the
 * right order, and free the old data.
 */
static struct send_redact_record *
get_next_redact_record(bqueue_t *bq, struct send_redact_record *prev)
{
	struct send_redact_record *next = bqueue_dequeue(bq);
	ASSERT(redact_record_before(prev, next));
	kmem_free(prev, sizeof (*prev));
	return (next);
}

/*
 * Remove the given redaction node from both trees, pull a new redaction record
 * off the queue, free the old redaction record, update the redaction node, and
 * reinsert the node into the trees.
 */
static void
update_avl_trees(avl_tree_t *start_tree, avl_tree_t *end_tree,
    struct redact_node *redact_node)
{
	avl_remove(start_tree, redact_node);
	avl_remove(end_tree, redact_node);
	redact_node->record = get_next_redact_record(&redact_node->st_arg->q,
	    redact_node->record);
	avl_add(end_tree, redact_node);
	avl_add(start_tree, redact_node);
}

/*
 * This thread merges all the redaction records provided by the worker threads,
 * and determines which blocks are redacted by all the snapshots.  The algorithm
 * for doing so is similar to performing a merge in mergesort with n sub-lists
 * instead of 2, with some added complexity due to the fact that the entries are
 * ranges, not just single blocks.  This algorithm relies on the fact that the
 * queues are sorted, which is ensured by the fact that traverse_dataset
 * traverses the dataset in a consistent order.  We pull one entry off the front
 * of the queues of each secure dataset traversal thread.  Then we repeat the
 * following: each record represents a range of blocks modified by one of the
 * redaction snapshots, and each block in that range may need to be redacted in
 * the send stream.  Find the record with the latest start of its range, and the
 * record with the earliest end of its range. If the last start is before the
 * first end, then we know that the blocks in the range [last_start, first_end]
 * are covered by all of the ranges at the front of the queues, which means
 * every thread redacts that whole range.  For example, let's say the ranges on
 * each queue look like this:
 *
 * Block Id   1  2  3  4  5  6  7  8  9 10 11
 * Thread 1 |    [====================]
 * Thread 2 |       [========]
 * Thread 3 |             [=================]
 *
 * Thread 3 has the last start (5), and the thread 2 has the last end (6).  All
 * three threads modified the range [5,6], so that data should not be sent over
 * the wire.  After we've determined whether or not to redact anything, we take
 * the record with the first end.  We discard that record, and pull a new one
 * off the front of the queue it came from.  In the above example, we would
 * discard Thread 2's record, and pull a new one.  Let's say the next record we
 * pulled from Thread 2 covered range [10,11].  The new layout would look like
 * this:
 *
 * Block Id   1  2  3  4  5  6  7  8  9 10 11
 * Thread 1 |    [====================]
 * Thread 2 |                            [==]
 * Thread 3 |             [=================]
 *
 * When we compare the last start (10, from Thread 2) and the first end (9, from
 * Thread 1), we see that the last start is greater than the first end.
 * Therefore, we do not redact anything from these records.  We'll iterate by
 * replacing the record from Thread 1.
 *
 * We iterate by replacing the record with the lowest end because we know
 * that the record with the lowest end has helped us as much as it can.  All the
 * ranges before it that we will ever redact have been redacted.  In addition,
 * by replacing the one with the lowest end, we guarantee we catch all ranges
 * that need to be redacted.  For example, if in the case above we had replaced
 * the record from Thread 1 instead, we might have ended up with the following:
 *
 * Block Id   1  2  3  4  5  6  7  8  9 10 11 12
 * Thread 1 |                               [==]
 * Thread 2 |       [========]
 * Thread 3 |             [=================]
 *
 * If the next record from Thread 2 had been [8,10], for example, we should have
 * redacted part of that range, but because we updated Thread 1's record, we
 * missed it.
 *
 * We implement this algorithm by using two trees.  The first sorts the
 * redaction records by their start_zb, and the second sorts them by their
 * end_zb.  We use these to find the record with the last start and the record
 * with the first end.  We create a record with that start and end, and send it
 * on.  The overall runtime of this implementation is O(n log m), where n is the
 * total number of redaction records from all the different redaction snapshots,
 * and m is the number of redaction snapshots.
 *
 * If we redact with respect to zero snapshots, we create a redaction
 * record with the start object and blkid to 0, and the end object and blkid to
 * UINT64_MAX.  This will result in us redacting every block.
 */
static void
redact_merge_thread(void *arg)
{
	struct redact_merge_thread_arg *mt_arg = arg;
	struct redact_node *redact_nodes = NULL;
	avl_tree_t start_tree, end_tree;
	struct send_redact_record *record, *current_record;

	/*
	 * If we're redacting with respect to zero snapshots, then no data is
	 * permitted to be sent.  We enqueue a record that redacts all blocks,
	 * and an eos marker.
	 */
	if (mt_arg->num_threads == 0) {
		record = kmem_zalloc(sizeof (struct send_redact_record),
		    KM_SLEEP);
		record->start_object = record->start_blkid = 0;
		record->end_object = record->end_blkid = UINT64_MAX;
		bqueue_enqueue(&mt_arg->q, record, sizeof (*record));

		record = kmem_zalloc(sizeof (struct send_redact_record),
		    KM_SLEEP);
		record->eos_marker = B_TRUE;
		bqueue_enqueue(&mt_arg->q, record, sizeof (*record));
		bqueue_flush(&mt_arg->q);
		return;
	}
	if (mt_arg->num_threads > 0) {
		redact_nodes = kmem_zalloc(mt_arg->num_threads *
		    sizeof (*redact_nodes), KM_SLEEP);
	}

	avl_create(&start_tree, redact_node_compare_start,
	    sizeof (struct redact_node),
	    offsetof(struct redact_node, avl_node_start));
	avl_create(&end_tree, redact_node_compare_end,
	    sizeof (struct redact_node),
	    offsetof(struct redact_node, avl_node_end));

	for (int i = 0; i < mt_arg->num_threads; i++) {
		struct redact_node *node = &redact_nodes[i];
		struct send_thread_arg *targ = &mt_arg->thread_args[i];
		node->record = bqueue_dequeue(&targ->q);
		node->st_arg = targ;
		node->thread_num = i;
		avl_add(&start_tree, node);
		avl_add(&end_tree, node);
	}

	/*
	 * Once the first record in the end tree has returned EOS, every record
	 * must be an EOS record, so we should stop.
	 */
	while (!((struct redact_node *)avl_first(&end_tree))->
	    record->eos_marker) {
		struct redact_node *last_start = avl_last(&start_tree);
		struct redact_node *first_end = avl_first(&end_tree);
		if (mt_arg->cancel)
			break;

		/*
		 * If the last start record is before the first end record,
		 * then we have blocks that are redacted by all threads.
		 * Therefore, we should redact them.  Copy the record, and send
		 * it to the main thread.
		 */
		if (redact_record_before(last_start->record,
		    first_end->record)) {
			record = kmem_zalloc(sizeof (struct send_redact_record),
			    KM_SLEEP);
			*record = *first_end->record;
			record->start_object = last_start->record->start_object;
			record->start_blkid = last_start->record->start_blkid;
			redact_record_merge_enqueue(&mt_arg->q, &current_record,
			    record);
		}
		update_avl_trees(&start_tree, &end_tree, first_end);
	}

	/*
	 * We're done; if we were cancelled, we need to cancel our workers and
	 * clear out their queues.  Either way, we need to remove every thread's
	 * redact_node struct from the avl trees.
	 */
	for (int i = 0; i < mt_arg->num_threads; i++) {
		if (mt_arg->cancel) {
			mt_arg->thread_args[i].cancel = B_TRUE;
			while (!redact_nodes[i].record->eos_marker) {
				update_avl_trees(&start_tree, &end_tree,
				    &redact_nodes[i]);
			}
		}
		avl_remove(&start_tree, &redact_nodes[i]);
		avl_remove(&end_tree, &redact_nodes[i]);
		kmem_free(redact_nodes[i].record,
		    sizeof (struct send_redact_record));
	}

	avl_destroy(&start_tree);
	avl_destroy(&end_tree);
	kmem_free(redact_nodes, mt_arg->num_threads * sizeof (*redact_nodes));
	record = kmem_zalloc(sizeof (struct send_redact_record), KM_SLEEP);
	record->eos_marker = B_TRUE;
	redact_record_merge_enqueue(&mt_arg->q, &current_record,
	    record);
	bqueue_flush(&mt_arg->q);
}

/*
 * Utility function that causes End of Stream records to compare after of all
 * others, so that other threads' comparison logic can stay simple.
 */
static int
send_record_compare(const struct send_block_record *from,
    const struct send_block_record *to)
{
	if (from->eos_marker == B_TRUE)
		return (1);
	else if (to->eos_marker == B_TRUE)
		return (-1);
	return (zbookmark_compare(from->datablksz >> SPA_MINBLOCKSHIFT,
	    from->indblkshift, to->datablksz >> SPA_MINBLOCKSHIFT,
	    to->indblkshift, &from->zb, &to->zb));
}

/*
 * Pop the new data off the queue, check that the records we receive are in
 * the right order, but do not free the old data.  This is used so that the
 * records can be sent on to the main thread without copying the data.
 */
static struct send_block_record *
get_next_record_nofree(bqueue_t *bq, struct send_block_record *prev)
{
	struct send_block_record *next = bqueue_dequeue(bq);
	ASSERT3S(send_record_compare(prev, next), ==, -1);
	return (next);
}

/*
 * Pop the new data off the queue, check that the records we receive are in
 * the right order, and free the old data.
 */
static struct send_block_record *
get_next_record(bqueue_t *bq, struct send_block_record *prev)
{
	struct send_block_record *next = get_next_record_nofree(bq, prev);
	kmem_free(prev, sizeof (*prev));
	return (next);
}

/*
 * Returns -1 if redact_data is above data, 1 if data is above redact_data, 0 if
 * they're equal, and -2 if neither is above the other.  In this context,
 * record a being "above" record b means that record a has a higher level and
 * that record b is entirely in the tree of blocks that record a points to.  We
 * detect this by comparing the start and end of the range each record refers
 * to.
 *
 * There are two special cases; if the redact record says to redact all the
 * data, we always return -1, indicating that the data should be redacted.  If
 * the redact record is marked EOS, and isn't marked redact_all, then we return
 * -2 because the data should not be redacted.
 */
static int
is_above(struct send_redact_record *redact_data, struct send_block_record *data)
{
	zbookmark_phys_t data_start = data->zb;
	zbookmark_phys_t data_end = data->zb;
	zbookmark_phys_t redact_start;
	zbookmark_phys_t redact_end;
	int start_cmp, end_cmp;
	uint16_t ddbss = data->datablksz >> SPA_MINBLOCKSHIFT;
	uint16_t dind = data->indblkshift;
	uint16_t rdbss = redact_data->datablksz >> SPA_MINBLOCKSHIFT;
	uint16_t rind = redact_data->indblkshift;

	if (redact_data->eos_marker)
		return (-2);
	ASSERT(!data->eos_marker);
	create_zbookmark_from_obj_off(&redact_start, redact_data->start_object,
	    redact_data->start_blkid);
	create_zbookmark_from_obj_off(&redact_end, redact_data->end_object,
	    redact_data->end_blkid);

	/*
	 * Create bookmarks that point to the start and end of the block
	 * record's range.
	 */
	data_start.zb_blkid = data->zb.zb_blkid * bp_span_in_blocks(dind,
	    data->zb.zb_level);
	data_end.zb_blkid = ((data->zb.zb_blkid + 1) *
	    bp_span_in_blocks(dind, data->zb.zb_level)) - 1;
	data_start.zb_level = data_end.zb_level = 0;

	start_cmp = zbookmark_compare(rdbss, rind, ddbss, dind, &data_start,
	    &redact_start);
	end_cmp = zbookmark_compare(rdbss, rind, ddbss, dind, &data_end,
	    &redact_end);

	if (start_cmp == 0 && end_cmp == 0) {
		return (0);
	}

	if (start_cmp <= 0 && end_cmp >= 0)
		return (1);
	if (start_cmp >= 0 && end_cmp <= 0)
		return (-1);
	return (-2);
}

/*
 * Compare a redaction record to a block record.  Return -1 if the redact record
 * is strictly before the block record, and 1 if the opposite is true.  If there
 * is any overlap at all, we return 0.  There are two special cases.  If the
 * redact_all marker is set in the redact record, then they overlap and we
 * return 0.  If the eos marker is set in the redact record, then they don't
 * overlap, and we return 1, since the block record is before the redact record.
 */
static int
redact_block_compare(struct send_redact_record *redact_data,
    struct send_block_record *data)
{
	zbookmark_phys_t redact_start;
	zbookmark_phys_t redact_end;
	zbookmark_phys_t data_start = data->zb;
	zbookmark_phys_t data_end = data->zb;

	if (redact_data->eos_marker)
		return (1);
	ASSERT(!data->eos_marker);
	create_zbookmark_from_obj_off(&redact_start, redact_data->start_object,
	    redact_data->start_blkid);
	create_zbookmark_from_obj_off(&redact_end, redact_data->end_object,
	    redact_data->end_blkid);

	data_start.zb_blkid = data->zb.zb_blkid *
	    bp_span_in_blocks(data->indblkshift, data->zb.zb_level);
	data_start.zb_level = 0;
	data_end.zb_blkid = ((data->zb.zb_blkid + 1) *
	    bp_span_in_blocks(data->indblkshift, data->zb.zb_level)) - 1;
	data_end.zb_level = 0;

	if (zbookmark_compare(redact_data->datablksz >> SPA_MINBLOCKSHIFT,
	    redact_data->indblkshift, data->datablksz >> SPA_MINBLOCKSHIFT,
	    data->indblkshift, &redact_end, &data_start) < 0) {
		return (-1);
	}
	if (zbookmark_compare(redact_data->datablksz >> SPA_MINBLOCKSHIFT,
	    redact_data->indblkshift, data->datablksz >> SPA_MINBLOCKSHIFT,
	    data->indblkshift, &redact_start, &data_end) > 0) {
		return (1);
	}
	return (0);
}

/*
 * We pull the data out of the embedded bp, whether it's compressed or not;
 * either way, byte-by-byte equality will test for data equality, since there's
 * only one compression algorithm for embedded block pointers.
 */
static int
embedded_bp_eq(blkptr_t *from_bp, blkptr_t *to_bp)
{
	if (BP_GET_LSIZE(from_bp) == BP_GET_LSIZE(to_bp)) {
		uint64_t from_buf[BPE_PAYLOAD_SIZE];
		uint64_t to_buf[BPE_PAYLOAD_SIZE];
		bzero(from_buf, sizeof (from_buf));
		bzero(to_buf, sizeof (to_buf));
		decode_embedded_bp_compressed(to_bp, to_buf);
		decode_embedded_bp_compressed(from_bp, from_buf);
		return (bcmp(to_buf, from_buf, sizeof (from_buf)) == 0);
	}
	return (0);
}

/*
 * Synctask for updating redaction lists.  We first take this txg's list of
 * redacted blocks and append those to the redaction list.  We then update the
 * redaction list's bonus buffer.  We store the furthest blocks we visited and
 * the list of snapshots that we're redacting with respect to.  We need these so
 * that redacted sends and receives can be correctly resumed.
 */
static void
redaction_list_update_sync(void *arg, dmu_tx_t *tx)
{
	struct redact_bookmark_info *rbi = arg;
	uint64_t txg = dmu_tx_get_txg(tx);
	list_t *list = &rbi->rbi_blocks[txg & TXG_MASK];
	redact_block_phys_t *furthest_visited =
	    &rbi->rbi_furthest[txg & TXG_MASK];
	objset_t *mos = tx->tx_pool->dp_meta_objset;
	redaction_list_t *rl = rbi->rbi_redaction_list;
	int bufsize = redact_sync_bufsize;
	redact_block_phys_t *buf = kmem_alloc(bufsize * sizeof (*buf),
	    KM_SLEEP);
	int index = 0;

	dmu_buf_will_dirty(rl->rl_dbuf, tx);

	for (struct redact_block_list_node *rbln = list_remove_head(list);
	    rbln != NULL; rbln = list_remove_head(list)) {
		ASSERT3U(rbln->block.rbp_object, <=,
		    furthest_visited->rbp_object);
		ASSERT(rbln->block.rbp_object < furthest_visited->rbp_object ||
		    rbln->block.rbp_blkid <= furthest_visited->rbp_blkid);
		buf[index] = rbln->block;
		index++;
		if (index == bufsize) {
			dmu_write(mos, rl->rl_object,
			    rl->rl_phys->rlp_num_entries * sizeof (*buf),
			    bufsize * sizeof (*buf), buf, tx);
			rl->rl_phys->rlp_num_entries += bufsize;
			index = 0;
		}
		kmem_free(rbln, sizeof (*rbln));
	}
	if (index > 0) {
		dmu_write(mos, rl->rl_object, rl->rl_phys->rlp_num_entries *
		    sizeof (*buf), index * sizeof (*buf), buf, tx);
		rl->rl_phys->rlp_num_entries += index;
	}
	kmem_free(buf, bufsize * sizeof (*buf));

	rbi->rbi_synctasc_txg[txg & TXG_MASK] = B_FALSE;
	rl->rl_phys->rlp_last_object = furthest_visited->rbp_object;
	rl->rl_phys->rlp_last_blkid = furthest_visited->rbp_blkid;
}

struct send_merge_data {
	list_t				redact_block_pending;
	redact_block_phys_t		coalesce_block;
	uint64_t			last_time;
};

/*
 * We want to store the list of blocks that we're redacting in the bookmark's
 * redaction list.  However, this list is stored in the MOS, which means it can
 * only be written to in syncing context.  To get around this, we create a
 * synctask that will write to the mos for us.  We tell it what to write by
 * a linked list for each current transaction group; every time we decide to
 * redact a block, we append it to the transaction group that is currently in
 * open context.  We also update some progress information that the synctask
 * will store to enable resumable redacted sends.
 */
static void
update_redaction_list(struct send_merge_data *smd, objset_t *os,
    struct redact_bookmark_info *rbi, uint64_t object, uint64_t blkid,
    uint32_t blksz, boolean_t redacted, boolean_t force)
{
	boolean_t enqueue = B_FALSE;
	redact_block_phys_t cur = {0};
	if (rbi->rbi_redaction_list == NULL)
		return;

	if (redacted) {
		ASSERT(!force);
		redact_block_phys_t *coalesce = &smd->coalesce_block;
		boolean_t new;
		if (coalesce->rbp_size_count == 0) {
			new = B_TRUE;
			enqueue = B_FALSE;
		} else  {
			uint64_t count = redact_block_get_count(coalesce);
			if (coalesce->rbp_object == object &&
			    coalesce->rbp_blkid + count == blkid &&
			    count < REDACT_BLOCK_MAX_COUNT) {
				ASSERT3U(redact_block_get_size(coalesce), ==,
				    blksz);
				redact_block_set_count(coalesce, count + 1);
				new = B_FALSE;
				enqueue = B_FALSE;
			} else {
				new = B_TRUE;
				enqueue = B_TRUE;
			}
		}

		if (new) {
			cur = *coalesce;
			coalesce->rbp_blkid = blkid;
			coalesce->rbp_object = object;

			redact_block_set_count(coalesce, 1);
			redact_block_set_size(coalesce, blksz);
		}
	} else {
		cur = smd->coalesce_block;
	}

	if ((enqueue || force) && !(redact_block_get_size(&cur) == 0)) {
		struct redact_block_list_node *rbln =
		    kmem_alloc(sizeof (struct redact_block_list_node),
		    KM_SLEEP);
		rbln->block = cur;
		list_insert_tail(&smd->redact_block_pending, rbln);
	}

	hrtime_t now = gethrtime();
	if (force || now > smd->last_time + redaction_list_update_interval_ns) {
		dmu_tx_t *tx =
		    dmu_tx_create_dd(spa_get_dsl(os->os_spa)->dp_mos_dir);
		dmu_tx_hold_space(tx, sizeof (struct redact_block_list_node));
		VERIFY0(dmu_tx_assign(tx, TXG_WAIT));
		uint64_t txg = dmu_tx_get_txg(tx);
		if (!rbi->rbi_synctasc_txg[txg & TXG_MASK]) {
			dsl_sync_task_nowait(dmu_tx_pool(tx),
			    redaction_list_update_sync, rbi, 5,
			    ZFS_SPACE_CHECK_NONE, tx);
			rbi->rbi_synctasc_txg[txg & TXG_MASK] = B_TRUE;
			rbi->rbi_latest_synctask_txg = txg;
		}

		rbi->rbi_furthest[txg & TXG_MASK].rbp_object = object;
		rbi->rbi_furthest[txg & TXG_MASK].rbp_blkid = blkid;
		list_move_tail(&rbi->rbi_blocks[txg & TXG_MASK],
		    &smd->redact_block_pending);

		dmu_tx_commit(tx);
		smd->last_time = now;
	}
}

/*
 * Merges redaction records from the redaction snapshots and block records from
 * the tosnap to figure out which blocks should be redacted.  If a block should
 * be redacted, we mark it so.  Either way, we then send the blocks to the main
 * thread.
 *
 * First, we pull redaction records until we find one that overlaps with or is
 * ahead of the block record.  Then, we check to see if either one fully
 * contains other.  If they're equal, or the redact record fully contains the
 * block record, then we know that all the data in the block record should be
 * redacted, so we mark the record as redact it and send it along.  If they
 * don't overlap at all, then we prefetch the data to improve performance, and
 * send the block record on to the main thread.  Finally, if the send record is
 * above the redaction record (perhaps because it's a hole and the redaction
 * record redacts a single block), then we ignore it.  If it's a hole, we don't
 * redact any subsets of it, because we don't need to redact a lack of data.  If
 * it's not a hole, it's an indirect block, which contains no data in and of
 * itself, so it need not be redacted.
 *
 * When we're done, we return the last redaction record we pulled off the queue.
 */
static struct send_redact_record *
redact_block_merge(struct send_merge_thread_arg *smta,
    struct send_merge_data *smd, bqueue_t *outq,
    struct send_redact_record *redact_data, struct send_block_record *data)
{
	zbookmark_phys_t *zb = &data->zb;
	int compare;
	bqueue_t *inq = &smta->redact_arg->q;
	boolean_t update_redaction = B_TRUE;
	boolean_t do_send = B_TRUE;
	objset_t *os = smta->os;

	while (redact_block_compare(redact_data, data) < 0) {
		redact_data = get_next_redact_record(inq, redact_data);
	}

	/*
	 * If the objset in the resume_redact_zb is non-zero, we are resuming a
	 * redacted send, and the redaction bookmark and the send stream did
	 * not end in the same place. Further, we have not caught up to
	 * whichever of the two was farther ahead, so we need to take special
	 * action in this case.
	 */
	if (smta->resume_redact_zb.zb_objset != 0) {
		if (zbookmark_compare(data->datablksz >> SPA_MINBLOCKSHIFT, 0,
		    data->datablksz >> SPA_MINBLOCKSHIFT, 0, &data->zb,
		    &smta->resume_redact_zb) < 0) {
			if (smta->bookmark_before) {
				/*
				 * The bookmark's last update was before the
				 * end of the original send stream, and the
				 * current data is before the point where the
				 * original send stream ended, then we should
				 * update the redaction list and not send the
				 * data to the main thread or prefetch it.
				 */
				do_send = B_FALSE;
			} else {
				/*
				 * The bookmark's last update was after the
				 * end of the original send stream, and the
				 * current data is before the last update of the
				 * bookmark, then we should update prefetch the
				 * data and send the record to the main thread
				 * as normal, but we should not update the
				 * bookmark.
				 */
				update_redaction = B_FALSE;
			}
		} else {
			/*
			 * If the current data is after both the end of the
			 * original send stream and the end of the redaction
			 * bookmark, then the further behind of the two has
			 * caught up, and we should clear the resume_redact_zb
			 * and return to normal operation.
			 */
			bzero(&smta->resume_redact_zb,
			    sizeof (zbookmark_phys_t));
		}
	}

	if (DMU_OT_IS_METADATA(data->obj_type)) {
		compare = -2;
	} else {
		compare = is_above(redact_data, data);
	}
	switch (compare) {
	case 0:
		/*
		 * The two ranges are equal. We should redact the data.
		 */
		/* FALLTHROUGH */
	case -1:
		/*
		 * The redaction range is above the send record. Redact the data
		 * by marking it and sending it to the main thread.
		 */
		data->redact_marker = B_TRUE;
		break;
	case 1:
		/*
		 * In this case, the send record is above the redaction record.
		 * This happens if the send record points to a hole.  We don't
		 * create redaction records for holes, because doing so requires
		 * some complicated, finnicky code.  Since holes don't contain
		 * meaningful data, fabricating holes and redaction records
		 * doesn't gain us anything.
		 */
		break;
	case -2: {
		/*
		 * In this case, neither record is above the other, so we want
		 * to actually send the data on to the main thread.  For
		 * performance reasons, prefetching is handled by another
		 * thread, so we just pass the record on to them.
		 */
		break;
	}
	default:
		panic("Invalid return value from is_above: %d", compare);
		return (NULL);
	}

	if (update_redaction && zb->zb_level == 0 && zb->zb_object != 0) {
		update_redaction_list(smd, os, &smta->rbi, zb->zb_object,
		    zb->zb_blkid, data->datablksz, data->redact_marker,
		    B_FALSE);
	}
	if (do_send)
		bqueue_enqueue(outq, data, sizeof (*data));

	return (redact_data);
}

/*
 * Merge the results from the from_ds and the to_ds, and then hand the record
 * off to send_main_thread to see if it should be redacted.  If this is not a
 * rebase send, the from thread will push an end of stream record and stop.
 * We'll just send everything that was changed in the to_ds since the ancestor's
 * creation txg.  Otherwise, we merge the records from the fromsnap and the
 * tosnap as follows:
 *
 * Since traverse_dataset has a canonical order, we can compare each change as
 * they're pulled off the queues.  We need to send all the differences between
 * from_ds and to_ds.  Anything that hasn't been modified since the common
 * ancestor can't be different between them.  Thus, we send:
 *
 * 1) Everything that's changed in to_ds since the common ancestor (just like in
 * the non-rebase case).
 * 2) Everything that's changed in from_ds since the common ancestor, but we
 * send the the data in to_ds.  For example, from_ds changed object 6 block
 * 10, so we send a record for object 6 block 10, but the data is the data from
 * to_ds.
 * 3) As an exception to the above, if the data has the same checksum (and the
 * checksums are cryptographically secure), then we don't need to send it.
 */
static void
send_merge_thread(void *arg)
{
	struct send_merge_thread_arg *smt_arg = arg;
	objset_t *os = smt_arg->os;
	struct send_thread_arg *from_arg = smt_arg->from_arg;
	struct send_thread_arg *to_arg = smt_arg->to_arg;
	struct redact_merge_thread_arg *rmt = smt_arg->redact_arg;
	bqueue_t *q = &smt_arg->q;
	struct send_block_record *from_data, *to_data;
	struct send_redact_record *redact_data;
	int err = 0;
	struct send_merge_data smd = { 0 };

	if (rmt == NULL) {
		redact_data = kmem_zalloc(sizeof (*redact_data), KM_SLEEP);
		redact_data->eos_marker = B_TRUE;
	} else {
		redact_data = bqueue_dequeue(&rmt->q);
		list_create(&smd.redact_block_pending,
		    sizeof (struct redact_block_list_node),
		    offsetof(struct redact_block_list_node, node));
	}
	from_data = bqueue_dequeue(&from_arg->q);
	to_data = bqueue_dequeue(&to_arg->q);

	while (!(from_data->eos_marker && to_data->eos_marker) && err == 0 &&
	    !smt_arg->cancel && from_arg->error_code == 0 &&
	    to_arg->error_code == 0) {
		int cmp = send_record_compare(from_data, to_data);
		if (cmp == 0) {
			/*
			 * Bookmarks are the same.
			 * Send data unless it's verifiably identical.
			 */

			/*
			 * We do this here because redact_block_merge consumes
			 * the to_data by passing it to another thread; if we
			 * get unlucky, this record will be freed before we
			 * return from redact_block_merge.
			 */
			struct send_block_record *next =
			    get_next_record_nofree(&to_arg->q, to_data);
			boolean_t strong = zio_checksum_table[
			    BP_GET_CHECKSUM(&to_data->bp)].ci_flags &
			    ZCHECKSUM_FLAG_NOPWRITE;
			if (BP_IS_EMBEDDED(&to_data->bp) &&
			    BP_IS_EMBEDDED(&from_data->bp)) {
				if (!embedded_bp_eq(&from_data->bp,
				    &to_data->bp)) {
					redact_data =
					    redact_block_merge(smt_arg, &smd, q,
					    redact_data, to_data);
				}
			} else if (!(strong && BP_GET_CHECKSUM(&to_data->bp) ==
			    BP_GET_CHECKSUM(&from_data->bp) &&
			    ZIO_CHECKSUM_EQUAL(to_data->bp.blk_cksum,
			    from_data->bp.blk_cksum))) {
				redact_data = redact_block_merge(smt_arg, &smd,
				    q, redact_data, to_data);
			}
			from_data = get_next_record(&from_arg->q, from_data);
			to_data = next;
		} else if (cmp < 0) {
			/*
			 * The "from" bookmark is ahead.  Send the record to the
			 * prefetch thread, which will retrieve and send to's
			 * version of the data.
			 */
			struct send_block_record *next =
			    get_next_record_nofree(&from_arg->q, from_data);
			ASSERT3U(from_data->zb.zb_objset, !=,
			    os->os_dsl_dataset->ds_object);
			redact_data = redact_block_merge(smt_arg, &smd,
			    q, redact_data, from_data);
			from_data = next;
		} else {
			/*
			 * The "to" bookmark is ahead.  Send the data.
			 */
			struct send_block_record *next =
			    get_next_record_nofree(&to_arg->q, to_data);
			redact_data = redact_block_merge(smt_arg, &smd, q,
			    redact_data, to_data);
			to_data = next;
		}
	}

	if (err == 0 && from_arg->error_code != 0)
		err = from_arg->error_code;
	if (err == 0 && to_arg->error_code != 0)
		err = to_arg->error_code;

	if (err != 0 || smt_arg->cancel) {
		to_arg->cancel = B_TRUE;
		while (!to_data->eos_marker) {
			to_data = get_next_record(&to_arg->q, to_data);
		}
		from_arg->cancel = B_TRUE;
		while (!from_data->eos_marker) {
			from_data = get_next_record(&from_arg->q, from_data);
		}
		if (rmt != NULL)
			rmt->cancel = B_TRUE;
	}
	/*
	 * If there is no redaction thread, we created a dummy redaction record,
	 * so we don't need to special case that logic.  We always need to pull
	 * off any extra redaction records, in case the redaction thread was
	 * behind the from thread and the to thread.
	 */
	while (!redact_data->eos_marker) {
		redact_data = get_next_redact_record(&rmt->q, redact_data);
	}
	kmem_free(redact_data, sizeof (*redact_data));
	kmem_free(from_data, sizeof (*from_data));
	kmem_free(to_data, sizeof (*to_data));

	if (!smt_arg->cancel && err == 0) {
		update_redaction_list(&smd, os, &smt_arg->rbi, UINT64_MAX,
		    UINT64_MAX, 0, B_FALSE, B_TRUE);
	}
	/*
	 * Wait for all the redaction info to sync out before we return, so that
	 * anyone who attempts to resume this send will have all the data they
	 * need.
	 */
	if (rmt != NULL) {
		dsl_pool_t *dp = spa_get_dsl(os->os_spa);
		struct redact_bookmark_info *rbip = &smt_arg->rbi;
		if (rbip->rbi_latest_synctask_txg != 0)
			txg_wait_synced(dp, rbip->rbi_latest_synctask_txg);
		for (int i = 0; i < TXG_SIZE; i++)
			list_destroy(&rbip->rbi_blocks[i]);
	}

	smt_arg->error = err;
	from_data = kmem_zalloc(sizeof (struct send_block_record), KM_SLEEP);
	from_data->eos_marker = B_TRUE;
	bqueue_enqueue(&smt_arg->q, from_data, sizeof (*from_data));
	bqueue_flush(&smt_arg->q);
}

struct send_prefetch_thread_arg {
	struct send_merge_thread_arg *smta;
	bqueue_t q;
	boolean_t cancel;
	int error;
};

/*
 * This thread is responsible for two things: First, it retrieves the correct
 * blkptr in the to ds if we need to send the data because of something from
 * the from thread.  As a result of this, we're the first ones to discover that
 * some indirect blocks can be discarded because they're not holes. Second,
 * it issues prefetches for the data we need to send.
 */
static void
send_prefetch_thread(void *arg)
{
	struct send_prefetch_thread_arg *spta = arg;
	struct send_merge_thread_arg *smta = spta->smta;
	bqueue_t *inq = &smta->q;
	bqueue_t *outq = &spta->q;
	objset_t *os = smta->os;
	struct send_block_record *data = bqueue_dequeue(inq);
	uint64_t data_size;
	int err = 0;
	while (!data->eos_marker && !spta->cancel && smta->error == 0) {
		if (!data->redact_marker && data->zb.zb_objset !=
		    dmu_objset_id(os)) {
			blkptr_t bp;
			uint16_t datablkszsec;
			err = dbuf_bookmark_findbp(os, &data->zb, &bp,
			    &datablkszsec, &data->indblkshift);
			if (err == ENOENT) {
				/*
				 * The block was modified in the from dataset,
				 * but doesn't exist in the to dataset; if it
				 * was deleted in the to dataset, then we'll
				 * visit the hole bp for it at some point.
				 */
				kmem_free(data, sizeof (*data));
				data = bqueue_dequeue(inq);
				err = 0;
				continue;
			} else if (err != 0) {
				break;
			} else {
				data->datablksz = datablkszsec <<
				    SPA_MINBLOCKSHIFT;
				data->bp = bp;
				data->zb.zb_objset = dmu_objset_id(os);
			}
		}

		if (data->zb.zb_level > 0 && !BP_IS_HOLE(&data->bp)) {
			kmem_free(data, sizeof (*data));
			data = bqueue_dequeue(inq);
			continue;
		}

		if (!data->redact_marker && !BP_IS_HOLE(&data->bp) &&
		    !BP_IS_REDACTED(&data->bp)) {
			arc_flags_t aflags = ARC_FLAG_NOWAIT |
			    ARC_FLAG_PREFETCH;
			(void) arc_read(NULL, os->os_spa, &data->bp, NULL, NULL,
			    ZIO_PRIORITY_ASYNC_READ, ZIO_FLAG_CANFAIL |
			    ZIO_FLAG_SPECULATIVE, &aflags, &data->zb);
		}
		data_size = data->datablksz == 0 ? sizeof (*data) :
		    data->datablksz;
		bqueue_enqueue(outq, data, data_size);
		data = bqueue_dequeue(inq);
	}
	if (spta->cancel || err != 0) {
		smta->cancel = B_TRUE;
		spta->error = err;
	} else if (smta->error != 0) {
		spta->error = smta->error;
	}
	while (!data->eos_marker) {
		kmem_free(data, sizeof (*data));
		data = bqueue_dequeue(inq);
	}

	bqueue_enqueue(outq, data, 1);
	bqueue_flush(outq);
}

static boolean_t
redact_snaps_contains(uint64_t *snaps, uint64_t num_snaps, uint64_t guid)
{
	for (int i = 0; i < num_snaps; i++) {
		if (snaps[i] == guid)
			return (B_TRUE);
	}
	return (B_FALSE);
}

struct dmu_send_params {
	/* Pool args */
	void *tag; // Tag that dp was held with, will be used to release dp.
	dsl_pool_t *dp;
	/* To snapshot args */
	const char *tosnap;
	dsl_dataset_t *to_ds;
	/* From snapshot args */
	dsl_dataset_t *from_ds; // Only set if this is a rebase send
	zfs_bookmark_phys_t ancestor_zb; // Always set
	uint64_t *fromredactsnaps;
	uint64_t numfromredactsnaps; // UINT64_MAX if not sending from redacted
	/* Stream params */
	boolean_t is_clone;
	boolean_t embedok;
	boolean_t large_block_ok;
	boolean_t compressok;
	uint64_t resumeobj;
	uint64_t resumeoff;
	dsl_dataset_t **redactsnaparr;
	uint32_t numredactsnaps;
	const char *redactbook;
	/* Stream output params */
	vnode_t *vp;
	offset_t *off;
	int outfd;
};

/*
 * Actually do the bulk of the work in a zfs send.
 *
 * The idea is that we want to do a send from from_ds to to_ds, and their common
 * ancestor's information is in ancestor_zb.  We also want to not send any data
 * that has been modified by all the datasets in redactsnaparr, and store the
 * list of blocks that are redacted in this way in a bookmark named redactbook,
 * created on the to_ds.  We do this by creating several worker threads, whose
 * function is described below.
 *
 * There are four cases.
 * The first case is a redacted zfs send.  In this case there are a variable
 * number of threads.  There are 5 threads plus one more for each dataset
 * redaction is occuring with respect to.  The first thread is the to_ds
 * traversal thread: it calls dataset_traverse on the to_ds and finds all the
 * blocks that have changed since ancestor_zb (if it's a full send, that's all
 * blocks in the dataset).  It then sends those blocks on to the send merge
 * thread.  The variable threads are the secure dataset traversal threads.  They
 * each call traverse_dataset on one of the redaction snapshots (typically the
 * first snapshot in a clone of the to_ds), finding all blocks that have changed
 * in that dataset since it diverged from the to_ds.  The redact merge thread
 * takes the data from the secure dataset traversal thread and merges them. Any
 * blocks modified by all the redaction snapshots should be redacted, so it
 * sends those blocks on to the send merge thread.  The send merge thread takes
 * the data from the to_ds traversal thread, and combines it with the redaction
 * records from the redact merge thread.  If a block appears in both the to_ds's
 * data and the redaction data, the send merge thread will mark it as redacted
 * and send it on to the prefetch thread.  It will also append that block to the
 * redaction list that is being created as part of the send.  Otherwise, the
 * send merge thread will send the block on to the prefetch thread unchanged.
 * The prefetch thread will issue prefetch reads for any data that isn't
 * redacted, and then send the data on to the main thread.  The main thread
 * behaves the same as in a normal send case.
 *
 * The graphic below diagrams the flow of data in the case of a redacted zfs
 * send.  Each box represents a thread, and each line represents the flow of
 * data.
 *
 * +--------------------+
 * | Secure Dataset     |
 * | Traversal Thread 1 +-------+
 * |                    |       |
 * +--------------------+       | Ranges modified by secure snap 1
 *                              | (redact_send_record)
 * +--------------------+  +----v----------------+
 * | Secure Dataset     |  |                     |
 * | Traversal Thread 2 +--> Redact Merge Thread |
 * |                    |  |                     |
 * +--------------------+  +----^-------+--------+
 *                              |       |
 *          ... Ranges modified |       | Ranges modified by every secure snap
 *              by secure snap N|       | (redact_send_record)
 * +--------------------+       |  +----v----------------------+
 * | Secure Dataset     |       |  | Send Merge Thread         |
 * | Traversal Thread N +-------+  | Apply redaction marks to  |
 * |                    |          | records as specified by   |
 * +--------------------+          | redaction ranges          |
 *                                 +----^---------------+------+
 *                                      |               | Merged data
 *                                      |               | (send_block_record)
 *                                      |  +------------v--------+
 *                                      |  | Prefetch Thread     |
 * +--------------------+               |  | Issues prefetch     |
 * | to_ds Traversal    |               |  | reads of data blocks|
 * | Thread (finds      +---------------+  +------------+--------+
 * | candidate blocks)  |  Blocks modified              | Prefetched data
 * +--------------------+  by to_ds since               | (send_block_record)
 *                         ancestor_zb     +------------v----+
 *                  (send_block_record)    | Main Thread     |  File Descriptor
 *                                         | Sends data over +->(to zfs receive)
 *                                         | wire            |
 *                                         +-----------------+
 * The second case is a rebase send.  In this case, there are six threads.  The
 * to_ds traversal thread and the main thread behave the same as in the redacted
 * send case.  The redact merge thread notices there are no redact traversal
 * threads, and so returns immediately.  The new thread is the from_ds traversal
 * thread.  It performs basically the same function as the to_ds traversal
 * thread, but for the from_ds.  The send merge thread now has to merge the data
 * from the two threads.  For details about that process, see the header comment
 * of send_merge_thread().  Any data it decides to send on will be prefetched by
 * the prefetch thread.  Note that it is not possible to perform a redacted
 * rebase send.
 *
 * The graphic below diagrams the flow of data in the case of a rebase zfs
 * send.
 *
 * +---------------------+
 * |                     |
 * | Redact Merge Thread +--------------+
 * |                     |              |
 * +---------------------+              |
 *        Blocks modified by            |
 *        from_ds since ancestor_zb     | End of Stream record
 *        (send_block_record)           | (send_redact_record)
 * +---------------------+   |     +----v----------------------+
 * | from_ds Traversal   |   v     | Send Merge Thread         |
 * | Thread (finds       +---------> Merges from_ds and to_ds  |
 * | candidate blocks)   |         | send records              |
 * +---------------------+         +----^---------------+------+
 *                                      |               | Merged data
 *                                      |  +------------v--------+
 *                                      |  | Prefetch Thread     |
 * +--------------------+               |  | Issues prefetch     |
 * | to_ds Traversal    |               |  | reads of data blocks|
 * | Thread (finds      +---------------+  +------------+--------+
 * | candidate blocks)  |  Blocks modified              | Prefetched data
 * +--------------------+  by to_ds since  +------------v----+
 *                         ancestor_zb     | Main Thread     |  File Descriptor
 *                  (send_block_record)    | Sends data over +->(to zfs receive)
 *                                         | wire            |
 *                                         +-----------------+
 *
 * The third case is an incremental send from a redaction bookmark.  This case
 * is very similar to the rebase send case; there are six threads, and they all
 * fulfill the same basic role.  The only difference is the from_ds traversal
 * thread.  Instead of iterating over the blocks in the from_ds, it iterates
 * over the redaction list in the redaction bookmark, and enqueues records for
 * each block that was redacted in the original send.  Note that you can perform
 * a redacted send from an incremental bookmark; in that case, the data flow
 * behaves very similarly to the flow in the redacted send case, except with the
 * addition of the from_ds traversal thread iterating over the redaction
 * bookmark.  The send_merge_thread also has to take on the responsibility of
 * merging the redaction list's records and the to_ds records.
 *
 * The final case is a simple zfs full or incremental send.  In this case, there
 * are only 5 threads. The to_ds traversal thread behaves the same as always. As
 * in the rebase case, the redact merge thread is started, realizes there's no
 * redaction going on, and promptly returns. The send merge thread takes all the
 * blocks that the to_ds traveral thread sends it, prefetches the data, and
 * sends the blocks on to the main thread.  The main thread sends the data over
 * the wire.
 *
 * To keep performance acceptable, we want to prefetch the data in the worker
 * threads.  While the to_ds thread could simply use the TRAVERSE_PREFETCH
 * feature built into traverse_dataset, the combining and deletion of records
 * due to redaction and rebase sends means that we could issue many unnecessary
 * prefetches.  As a result, we only prefetch data after we've determined that
 * the record is not going to be redacted.  To prevent the prefetching from
 * getting too far ahead of the main thread, the blocking queues that are used
 * for communication are capped not by the number of entries in the queue, but
 * by the sum of the size of the prefetches associated with them.  The limit on
 * the amount of data that the thread can prefetch beyond what the main thread
 * has reached is controlled by the global variable zfs_send_queue_length.  In
 * addition, to prevent poor performance in the beginning of a send, we also
 * limit the distance ahead that the traversal threads can be.  That distance is
 * controlled by the zfs_send_no_prefetch_queue_length tunable.
 *
 * Note: Releases dp using the specified tag.
 */
static int
dmu_send_impl(struct dmu_send_params *dspp)
{
	objset_t *os;
	dmu_replay_record_t *drr;
	dmu_sendstatus_t *dssp;
	dmu_send_cookie_t dsc = {0};
	int err;
	uint64_t fromtxg = 0;
	uint64_t featureflags = 0;
	struct send_thread_arg from_arg = { 0 };
	struct send_thread_arg to_arg = { 0 };
	struct redact_merge_thread_arg rmt_arg = { 0 };
	struct send_merge_thread_arg smt_arg = { 0 };
	struct send_prefetch_thread_arg spt_arg = { 0 };
	struct send_thread_arg *redact_args = NULL;
	struct send_block_record *rec;
	redaction_list_t *new_rl = NULL;
	redaction_list_t *from_rl = NULL;
	char newredactbook[ZFS_MAX_DATASET_NAME_LEN];
	boolean_t resuming = (dspp->resumeobj != 0 || dspp->resumeoff != 0);

	dsl_dataset_t *to_ds = dspp->to_ds;
	dsl_dataset_t *from_ds = dspp->from_ds;
	zfs_bookmark_phys_t *ancestor_zb = &dspp->ancestor_zb;
	dsl_pool_t *dp = dspp->dp;
	void *tag = dspp->tag;

	err = dmu_objset_from_ds(to_ds, &os);
	if (err != 0) {
		dsl_pool_rele(dp, tag);
		return (err);
	}

	drr = kmem_zalloc(sizeof (dmu_replay_record_t), KM_SLEEP);
	drr->drr_type = DRR_BEGIN;
	drr->drr_u.drr_begin.drr_magic = DMU_BACKUP_MAGIC;
	DMU_SET_STREAM_HDRTYPE(drr->drr_u.drr_begin.drr_versioninfo,
	    DMU_SUBSTREAM);

#ifdef _KERNEL
	if (dmu_objset_type(os) == DMU_OST_ZFS) {
		uint64_t version;
		if (zfs_get_zplprop(os, ZFS_PROP_VERSION, &version) != 0) {
			kmem_free(drr, sizeof (dmu_replay_record_t));
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(EINVAL));
		}
		if (version >= ZPL_VERSION_SA) {
			featureflags |= DMU_BACKUP_FEATURE_SA_SPILL;
		}
	}
#endif

	if (dspp->large_block_ok && dsl_dataset_feature_is_active(to_ds,
	    SPA_FEATURE_LARGE_BLOCKS)) {
		featureflags |= DMU_BACKUP_FEATURE_LARGE_BLOCKS;
	}
	if (dspp->embedok &&
	    spa_feature_is_active(dp->dp_spa, SPA_FEATURE_EMBEDDED_DATA)) {
		featureflags |= DMU_BACKUP_FEATURE_EMBED_DATA;
	}
	if (dspp->compressok) {
		featureflags |= DMU_BACKUP_FEATURE_COMPRESSED;
	}
	if ((featureflags &
	    (DMU_BACKUP_FEATURE_EMBED_DATA | DMU_BACKUP_FEATURE_COMPRESSED)) !=
	    0 && spa_feature_is_active(dp->dp_spa, SPA_FEATURE_LZ4_COMPRESS)) {
		featureflags |= DMU_BACKUP_FEATURE_LZ4;
	}

	/*
	 * Note: If we are sending a full stream (non-incremental), then
	 * we can not send mooch records, because the receiver won't have
	 * the origin to mooch from.
	 */
	if (dspp->embedok && dsl_dataset_feature_is_active(to_ds,
	    SPA_FEATURE_MOOCH_BYTESWAP) && ancestor_zb != NULL) {
		featureflags |= DMU_BACKUP_FEATURE_EMBED_MOOCH_BYTESWAP;
	}

	if (resuming) {
		featureflags |= DMU_BACKUP_FEATURE_RESUMING;
	}

	if (dspp->redactbook != NULL || dsl_dataset_feature_is_active(to_ds,
	    SPA_FEATURE_REDACTED_DATASETS)) {
		if (dspp->redactbook != NULL &&
		    dsl_dataset_feature_is_active(to_ds,
		    SPA_FEATURE_REDACTED_DATASETS)) {
			kmem_free(drr, sizeof (dmu_replay_record_t));
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(EALREADY));
		}
		featureflags |= DMU_BACKUP_FEATURE_REDACTED;
		for (int i = 0; i < dspp->numredactsnaps; i++) {
			if (dsl_dataset_feature_is_active(
			    dspp->redactsnaparr[i],
			    SPA_FEATURE_REDACTED_DATASETS)) {
				kmem_free(drr, sizeof (dmu_replay_record_t));
				dsl_pool_rele(dp, tag);
				return (SET_ERROR(EALREADY));
			}
		}
	}

	DMU_SET_FEATUREFLAGS(drr->drr_u.drr_begin.drr_versioninfo,
	    featureflags);

	drr->drr_u.drr_begin.drr_creation_time =
	    dsl_dataset_phys(to_ds)->ds_creation_time;
	drr->drr_u.drr_begin.drr_type = dmu_objset_type(os);
	if (dspp->is_clone)
		drr->drr_u.drr_begin.drr_flags |= DRR_FLAG_CLONE;
	drr->drr_u.drr_begin.drr_toguid = dsl_dataset_phys(to_ds)->ds_guid;
	if (dsl_dataset_phys(to_ds)->ds_flags & DS_FLAG_CI_DATASET)
		drr->drr_u.drr_begin.drr_flags |= DRR_FLAG_CI_DATA;
	drr->drr_u.drr_begin.drr_flags |= DRR_FLAG_FREERECORDS;

	if (ancestor_zb != NULL) {
		/*
		 * We're doing an incremental send; if from_ds is non-null, then
		 * this is a rebase send, and we have to specify the guid of the
		 * snapshot we're rebasing from.  If it is null, then this is a
		 * normal incremental send, and we should specify the guid of
		 * the ancestor_zb.
		 */
		if (from_ds != NULL) {
			drr->drr_u.drr_begin.drr_fromguid =
			    dsl_dataset_phys(from_ds)->ds_guid;
		} else {
			drr->drr_u.drr_begin.drr_fromguid =
			    ancestor_zb->zbm_guid;
		}
		fromtxg = ancestor_zb->zbm_creation_txg;
	}
	dsl_dataset_name(to_ds, drr->drr_u.drr_begin.drr_toname);
	if (!to_ds->ds_is_snapshot) {
		(void) strlcat(drr->drr_u.drr_begin.drr_toname, "@--head--",
		    sizeof (drr->drr_u.drr_begin.drr_toname));
	}

	if (dspp->numredactsnaps > 0) {
		redact_args = kmem_zalloc(dspp->numredactsnaps *
		    sizeof (to_arg), KM_SLEEP);
	}

	dsl_dataset_long_hold(to_ds, FTAG);
	if (from_ds != NULL)
		dsl_dataset_long_hold(from_ds, FTAG);
	for (int i = 0; i < dspp->numredactsnaps; i++)
		dsl_dataset_long_hold(dspp->redactsnaparr[i], FTAG);

	if (dspp->redactbook != NULL) {
		char *c;
		int n;
		(void) strncpy(newredactbook, dspp->tosnap,
		    ZFS_MAX_DATASET_NAME_LEN);
		c = strchr(newredactbook, '@');
		ASSERT3P(c, !=, NULL);
		n = snprintf(c, ZFS_MAX_DATASET_NAME_LEN - (c - newredactbook),
		    "#%s", dspp->redactbook);
		if (n >= ZFS_MAX_DATASET_NAME_LEN - (c - newredactbook)) {
			kmem_free(drr, sizeof (dmu_replay_record_t));
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(ENAMETOOLONG));
		}
		if (resuming) {
			zfs_bookmark_phys_t bookmark;
			err = dsl_bookmark_lookup(dp, newredactbook, NULL,
			    &bookmark);
			if (err != 0) {
				kmem_free(drr, sizeof (dmu_replay_record_t));
				dsl_pool_rele(dp, tag);
				return (SET_ERROR(ENOENT));
			}
			if (bookmark.zbm_redaction_obj == 0) {
				kmem_free(drr, sizeof (dmu_replay_record_t));
				dsl_pool_rele(dp, tag);
				return (SET_ERROR(EINVAL));
			}
			err = dsl_redaction_list_hold_obj(dp,
			    bookmark.zbm_redaction_obj, FTAG, &new_rl);
			if (err != 0) {
				kmem_free(drr, sizeof (dmu_replay_record_t));
				dsl_pool_rele(dp, tag);
				return (SET_ERROR(EINVAL));
			}
			dsl_redaction_list_long_hold(dp, new_rl, FTAG);
		}
	}

	if (ancestor_zb != NULL && ancestor_zb->zbm_redaction_obj != 0) {
		err = dsl_redaction_list_hold_obj(dp,
		    ancestor_zb->zbm_redaction_obj, FTAG, &from_rl);
		if (err != 0) {
			kmem_free(drr, sizeof (dmu_replay_record_t));
			dsl_pool_rele(dp, tag);
			return (SET_ERROR(EINVAL));
		}
		dsl_redaction_list_long_hold(dp, from_rl, FTAG);
	}

	dssp = kmem_zalloc(sizeof (dmu_sendstatus_t), KM_SLEEP);
	dssp->dss_outfd = dspp->outfd;
	dssp->dss_off = dspp->off;
	dssp->dss_proc = curproc;

	mutex_enter(&to_ds->ds_sendstream_lock);
	list_insert_head(&to_ds->ds_sendstreams, dssp);
	mutex_exit(&to_ds->ds_sendstream_lock);

	dsc.dsc_drr = drr;
	dsc.dsc_vp = dspp->vp;
	dsc.dsc_os = os;
	dsc.dsc_off = dspp->off;
	dsc.dsc_toguid = dsl_dataset_phys(to_ds)->ds_guid;
	dsc.dsc_pending_op = PENDING_NONE;
	dsc.dsc_featureflags = featureflags;
	dsc.dsc_resume_object = dspp->resumeobj;
	dsc.dsc_resume_offset = dspp->resumeoff;

	dsl_pool_rele(dp, tag);

	void *payload = NULL;
	size_t payload_len = 0;
	nvlist_t *nvl = fnvlist_alloc();

	/*
	 * If we're doing a redacted send, we include the snapshots we're
	 * redacted with respect to so that the target system knows what send
	 * streams can be correctly received on top of this dataset. If we're
	 * instead sending a redacted dataset, we include the snapshots that the
	 * dataset was created with respect to.
	 */
	if (dspp->redactbook != NULL) {
		uint64_t *guids = NULL;
		if (dspp->numredactsnaps > 0) {
			guids = kmem_zalloc(dspp->numredactsnaps *
			    sizeof (uint64_t), KM_SLEEP);
		}
		for (int i = 0; i < dspp->numredactsnaps; i++) {
			guids[i] =
			    dsl_dataset_phys(dspp->redactsnaparr[i])->ds_guid;
		}

		if (!resuming) {
			err = dsl_bookmark_create_redacted(newredactbook,
			    dspp->tosnap, dspp->numredactsnaps, guids, FTAG,
			    &new_rl);
			if (err != 0) {
				kmem_free(guids, dspp->numredactsnaps *
				    sizeof (uint64_t));
				fnvlist_free(nvl);
				goto out;
			}
		}

		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_SNAPS, guids,
		    dspp->numredactsnaps);
		kmem_free(guids, dspp->numredactsnaps * sizeof (uint64_t));
	} else if (dsl_dataset_feature_is_active(to_ds,
	    SPA_FEATURE_REDACTED_DATASETS)) {
		uint64_t *tods_guids;
		uint64_t length;
		VERIFY(dsl_dataset_get_uint64_array_feature(to_ds,
		    SPA_FEATURE_REDACTED_DATASETS, &length, &tods_guids));
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_SNAPS, tods_guids,
		    length);
	}

	/*
	 * If we're sending from a redaction bookmark, then we should retrieve
	 * the guids of that bookmark so we can send them over the wire.
	 */
	if (from_rl != NULL) {
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_FROM_SNAPS,
		    from_rl->rl_phys->rlp_snaps,
		    from_rl->rl_phys->rlp_num_snaps);
	}

	/*
	 * If the snapshot we're sending from is redacted, include the redaction
	 * list in the stream.
	 */
	if (dspp->numfromredactsnaps != UINT64_MAX) {
		ASSERT3P(from_rl, ==, NULL);
		fnvlist_add_uint64_array(nvl, BEGINNV_REDACT_FROM_SNAPS,
		    dspp->fromredactsnaps, dspp->numfromredactsnaps);
		if (dspp->numfromredactsnaps > 0) {
			kmem_free(dspp->fromredactsnaps,
			    dspp->numfromredactsnaps * sizeof (uint64_t));
			dspp->fromredactsnaps = NULL;
		}
	}

	if (resuming) {
		uint64_t obj = dspp->resumeobj;
		dmu_object_info_t to_doi;

		err = dmu_object_info(os, obj, &to_doi);
		if (err != 0)
			goto out;

		uint64_t blkid = dspp->resumeoff / to_doi.doi_data_block_size;

		/*
		 * If we're resuming a redacted send, we have to modify where we
		 * start traversing our various datasets; if the previous send
		 * failed because this system crashed, the redaction bookmark
		 * may be missing some redaction_records.  In that case, we need
		 * to start the traversal where the redaction bookmark leaves
		 * off.  On the other hand, the redaction bookmark may be ahead
		 * of the resume location, in which case we need to make sure
		 * not to insert the same record into the redaction bookmark
		 * twice.
		 */
		smt_arg.bookmark_before = B_FALSE;
		if (new_rl != NULL) {
			uint64_t furthest_object =
			    new_rl->rl_phys->rlp_last_object;
			uint64_t furthest_blkid =
			    new_rl->rl_phys->rlp_last_blkid;
			if (furthest_object < dspp->resumeobj ||
			    (furthest_object == dspp->resumeobj &&
			    furthest_blkid < blkid)) {
				obj = furthest_object;
				blkid = furthest_blkid;
				SET_BOOKMARK(&smt_arg.resume_redact_zb,
				    to_ds->ds_object, obj, 0, blkid);
				smt_arg.bookmark_before = B_TRUE;
			} else if (furthest_object > dspp->resumeobj ||
			    (furthest_object == dspp->resumeobj &&
			    furthest_blkid > blkid)) {
				SET_BOOKMARK(&smt_arg.resume_redact_zb,
				    to_ds->ds_object, furthest_object, 0,
				    furthest_blkid);
			}
		}

		SET_BOOKMARK(&to_arg.resume, to_ds->ds_object, obj, 0,
		    blkid);
		if (from_ds != NULL || nvlist_exists(nvl,
		    BEGINNV_REDACT_FROM_SNAPS)) {
			uint64_t objset = (from_ds == NULL ?
			    ancestor_zb->zbm_redaction_obj :
			    from_ds->ds_object);
			/*
			 * Note: If the resume point is in an object whose
			 * blocksize is different in the from vs to snapshots,
			 * we will have divided by the "wrong" blocksize.
			 * However, in this case fromsnap's send_cb() will
			 * detect that the blocksize has changed and therefore
			 * ignore this object.
			 *
			 * If we're resuming a send from a redaction bookmark,
			 * we still cannot accidentally suggest blocks behind
			 * the to_ds.  In addition, we know that any blocks in
			 * the object in the to_ds will have to be sent, since
			 * the size changed.  Therefore, we can't cause any harm
			 * this way either.
			 */
			SET_BOOKMARK(&from_arg.resume, objset, obj, 0, blkid);
		}
		for (int i = 0; i < dspp->numredactsnaps; i++) {
			SET_BOOKMARK(&redact_args[i].resume,
			    dspp->redactsnaparr[i]->ds_object, obj, 0,
			    blkid);
		}

		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OBJECT, dspp->resumeobj);
		fnvlist_add_uint64(nvl, BEGINNV_RESUME_OFFSET, dspp->resumeoff);
	}

	if (fnvlist_num_pairs(nvl) > 0) {
		payload = fnvlist_pack(nvl, &payload_len);
		drr->drr_payloadlen = payload_len;
	}

	fnvlist_free(nvl);
	err = dump_record(&dsc, payload, payload_len);
	fnvlist_pack_free(payload, payload_len);
	if (err != 0) {
		err = dsc.dsc_err;
		goto out;
	}

	VERIFY0(bqueue_init(&to_arg.q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_block_record, ln)));
	to_arg.error_code = 0;
	to_arg.cancel = B_FALSE;
	to_arg.ds = to_ds;
	to_arg.fromtxg = fromtxg;
	to_arg.flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA;
	to_arg.to_os = NULL;
	to_arg.redaction_list = NULL;
	(void) thread_create(NULL, 0, send_traverse_thread, &to_arg, 0,
	    curproc, TS_RUN, minclsyspri);

	VERIFY0(bqueue_init(&from_arg.q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_block_record, ln)));

	from_arg.error_code = 0;
	from_arg.cancel = B_FALSE;
	from_arg.ds = from_ds;
	from_arg.fromtxg = fromtxg;
	from_arg.flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA;
	from_arg.to_os = os;
	from_arg.redaction_list = from_rl;

	/*
	 * If from_ds is null, send_traverse_thread just returns success and
	 * enqueues an eos marker.
	 */
	(void) thread_create(NULL, 0, send_traverse_thread, &from_arg, 0,
	    curproc, TS_RUN, minclsyspri);

	for (int i = 0; i < dspp->numredactsnaps; i++) {
		struct send_thread_arg *arg = redact_args + i;
		VERIFY0(bqueue_init(&arg->q, zfs_send_no_prefetch_queue_ff,
		    zfs_send_no_prefetch_queue_length,
		    offsetof(struct send_redact_record, ln)));
		arg->error_code = 0;
		arg->cancel = B_FALSE;
		arg->ds = dspp->redactsnaparr[i];
		arg->fromtxg = dsl_dataset_phys(to_ds)->ds_creation_txg;
		arg->flags = TRAVERSE_PRE | TRAVERSE_PREFETCH_METADATA;
		arg->to_os = os;

		(void) thread_create(NULL, 0, redact_traverse_thread, arg, 0,
		    curproc, TS_RUN, minclsyspri);
	}
	if (new_rl != NULL) {
		rmt_arg.cancel = B_FALSE;
		rmt_arg.num_threads = dspp->numredactsnaps;
		rmt_arg.send_objset = os->os_dsl_dataset->ds_object;
		VERIFY0(bqueue_init(&rmt_arg.q, zfs_send_no_prefetch_queue_ff,
		    zfs_send_no_prefetch_queue_length,
		    offsetof(struct send_redact_record, ln)));
		rmt_arg.thread_args = redact_args;
		(void) thread_create(NULL, 0, redact_merge_thread, &rmt_arg,
		    0, curproc, TS_RUN, minclsyspri);
	}

	VERIFY0(bqueue_init(&smt_arg.q, zfs_send_no_prefetch_queue_ff,
	    zfs_send_no_prefetch_queue_length,
	    offsetof(struct send_block_record, ln)));
	smt_arg.cancel = B_FALSE;
	smt_arg.error = 0;
	smt_arg.from_arg = &from_arg;
	smt_arg.to_arg = &to_arg;
	if (new_rl != NULL) {
		smt_arg.redact_arg = &rmt_arg;
		smt_arg.rbi.rbi_redaction_list = new_rl;
		smt_arg.rbi.rbi_latest_synctask_txg = 0;
		for (int i = 0; i < TXG_SIZE; i++) {
			list_create(&smt_arg.rbi.rbi_blocks[i],
			    sizeof (struct redact_block_list_node),
			    offsetof(struct redact_block_list_node, node));

		}
	}
	smt_arg.os = os;
	(void) thread_create(NULL, 0, send_merge_thread, &smt_arg, 0, curproc,
	    TS_RUN, minclsyspri);

	VERIFY0(bqueue_init(&spt_arg.q, zfs_send_queue_ff,
	    zfs_send_queue_length, offsetof(struct send_block_record, ln)));
	spt_arg.smta = &smt_arg;
	(void) thread_create(NULL, 0, send_prefetch_thread, &spt_arg, 0,
	    curproc, TS_RUN, minclsyspri);

	rec = bqueue_dequeue(&spt_arg.q);
	while (err == 0 && !rec->eos_marker) {
		err = do_dump(&dsc, rec);
		rec = get_next_record(&spt_arg.q, rec);
		if (issig(JUSTLOOKING) && issig(FORREAL))
			err = EINTR;
	}

	/*
	 * If we hit an error or are interrupted, cancel our worker threads and
	 * clear the queue of any pending records.  The threads will pass the
	 * cancel up the tree of worker threads, and each one will clean up any
	 * pending records before exiting.
	 */
	if (err != 0) {
		spt_arg.cancel = B_TRUE;
		while (!rec->eos_marker) {
			rec = get_next_record(&spt_arg.q, rec);
		}
	}
	kmem_free(rec, sizeof (*rec));

	bqueue_destroy(&spt_arg.q);
	bqueue_destroy(&smt_arg.q);
	if (new_rl != NULL)
		bqueue_destroy(&rmt_arg.q);
	for (int i = 0; i < dspp->numredactsnaps; i++) {
		bqueue_destroy(&redact_args[i].q);
	}
	bqueue_destroy(&to_arg.q);
	bqueue_destroy(&from_arg.q);

	if (err == 0 && spt_arg.error != 0)
		err = spt_arg.error;

	if (err != 0)
		goto out;

	if (dsc.dsc_pending_op != PENDING_NONE)
		if (dump_record(&dsc, NULL, 0) != 0)
			err = SET_ERROR(EINTR);

	if (err != 0) {
		if (err == EINTR && dsc.dsc_err != 0)
			err = dsc.dsc_err;
		goto out;
	}

	bzero(drr, sizeof (dmu_replay_record_t));
	drr->drr_type = DRR_END;
	drr->drr_u.drr_end.drr_checksum = dsc.dsc_zc;
	drr->drr_u.drr_end.drr_toguid = dsc.dsc_toguid;

	if (dump_record(&dsc, NULL, 0) != 0)
		err = dsc.dsc_err;

out:
	mutex_enter(&to_ds->ds_sendstream_lock);
	list_remove(&to_ds->ds_sendstreams, dssp);
	mutex_exit(&to_ds->ds_sendstream_lock);

	VERIFY(err != 0 || (dsc.dsc_sent_begin && dsc.dsc_sent_end));

	kmem_free(drr, sizeof (dmu_replay_record_t));
	kmem_free(dssp, sizeof (dmu_sendstatus_t));
	if (dspp->numredactsnaps > 0)
		kmem_free(redact_args, dspp->numredactsnaps * sizeof (to_arg));

	for (int i = 0; i < dspp->numredactsnaps; i++)
		dsl_dataset_long_rele(dspp->redactsnaparr[i], FTAG);
	if (from_ds != NULL)
		dsl_dataset_long_rele(from_ds, FTAG);
	dsl_dataset_long_rele(to_ds, FTAG);
	if (from_rl != NULL) {
		dsl_redaction_list_long_rele(from_rl, FTAG);
		dsl_redaction_list_rele(from_rl, FTAG);
	}
	if (new_rl != NULL) {
		dsl_redaction_list_long_rele(new_rl, FTAG);
		dsl_redaction_list_rele(new_rl, FTAG);
	}

	return (err);
}

static int
dsl_dataset_walk_origin(dsl_pool_t *dp, dsl_dataset_t **ds, void *tag)
{
	uint64_t origin_obj = dsl_dir_phys((*ds)->ds_dir)->dd_origin_obj;
	dsl_dataset_t *prev;
	int err = dsl_dataset_hold_obj(dp, origin_obj, tag, &prev);
	if (err != 0)
		return (err);
	dsl_dataset_rele(*ds, tag);
	*ds = prev;
	prev = NULL;
	return (err);
}

/*
 * Find the common ancestor of two datasets.
 *
 * We first measure how far each dataset is from the ORIGIN$ORIGIN by stepping
 * back through each object's origin snapshot.  We then walk the one that is
 * further up it's origin snapshots until each dataset is the same distance from
 * ORIGIN$ORIGIN.  Now, at each step we compare to see whether the two datasets
 * are in the same ds_dir.  Once they are, we compare the two snapshots; the
 * older of the two is the common ancestor of the two datasets.
 */
static int
find_common_ancestor(dsl_pool_t *dp, dsl_dataset_t *ds1, dsl_dataset_t *ds2,
    zfs_bookmark_phys_t *zb)
{
	uint32_t steps1, steps2, diff;
	dsl_dataset_t *walker1, *walker2;
	int err = 0;

	if (ds1->ds_dir == ds2->ds_dir) {
		err = dsl_dataset_hold_obj(dp, ds1->ds_object, FTAG, &walker1);
		if (err != 0)
			return (err);
		err = dsl_dataset_hold_obj(dp, ds2->ds_object, FTAG, &walker2);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			return (err);
		}
		goto fini;
	}

	/*
	 * Count how far ds1 is from $ORIGIN.
	 */
	err = dsl_dataset_hold_obj(dp, ds1->ds_object, FTAG, &walker1);
	if (err != 0)
		return (err);

	steps1 = 0;
	while (dsl_dataset_is_clone(walker1, dp->dp_origin_snap)) {
		err = dsl_dataset_walk_origin(dp, &walker1, FTAG);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			return (err);
		}
		steps1++;
	}
	dsl_dataset_rele(walker1, FTAG);

	/*
	 * Count how far ds2 is from $ORIGIN
	 */
	err = dsl_dataset_hold_obj(dp, ds2->ds_object, FTAG, &walker1);
	if (err != 0)
		return (err);

	steps2 = 0;
	while (dsl_dataset_is_clone(walker1, dp->dp_origin_snap)) {
		err = dsl_dataset_walk_origin(dp, &walker1, FTAG);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			return (err);
		}
		steps2++;
	}
	dsl_dataset_rele(walker1, FTAG);

	/*
	 * Calculate which ds is farther from $ORIGIN, assign that to walker1,
	 * and assign the other to walker2.  Assign the difference in distances
	 * to diff.
	 */
	if (steps1 > steps2) {
		diff = steps1 - steps2;
		err = dsl_dataset_hold_obj(dp, ds1->ds_object, FTAG, &walker1);
		if (err != 0)
			return (err);
		err = dsl_dataset_hold_obj(dp, ds2->ds_object, FTAG, &walker2);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			return (err);
		}
	} else {
		diff = steps2 - steps1;
		err = dsl_dataset_hold_obj(dp, ds2->ds_object, FTAG, &walker1);
		if (err != 0)
			return (err);
		err = dsl_dataset_hold_obj(dp, ds1->ds_object, FTAG, &walker2);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			return (err);
		}
	}

	/*
	 * Walk walker1 back diff steps so both systems are the same distance
	 * from $ORIGIN.
	 */
	for (int i = 0; i < diff; i++) {
		err = dsl_dataset_walk_origin(dp, &walker1, FTAG);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			dsl_dataset_rele(walker2, FTAG);
			return (err);
		}
	}

	/*
	 * Walk back in step, and stop when the two walkers are snapshots in the
	 * same dataset dir.
	 */
	while ((walker1->ds_dir != walker2->ds_dir) &&
	    !(dsl_dir_phys(walker1->ds_dir)->dd_origin_obj == 0 &&
	    dsl_dir_phys(walker2->ds_dir)->dd_origin_obj == 0)) {
		err = dsl_dataset_walk_origin(dp, &walker1, FTAG);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			dsl_dataset_rele(walker2, FTAG);
			return (err);
		}


		err = dsl_dataset_walk_origin(dp, &walker2, FTAG);
		if (err != 0) {
			dsl_dataset_rele(walker1, FTAG);
			dsl_dataset_rele(walker2, FTAG);
			return (err);
		}
	}

fini:
	/*
	 * Load the zb with the data from the older snapshot.
	 */
	if (walker1->ds_dir != walker2->ds_dir) {
		zb->zbm_creation_txg = 0;
		zb->zbm_creation_time = 0;
		zb->zbm_guid = 0;
	} else if (dsl_dataset_phys(walker1)->ds_creation_txg >
	    dsl_dataset_phys(walker2)->ds_creation_txg) {
		zb->zbm_creation_txg =
		    dsl_dataset_phys(walker2)->ds_creation_txg;
		zb->zbm_creation_time =
		    dsl_dataset_phys(walker2)->ds_creation_time;
		zb->zbm_guid = dsl_dataset_phys(walker2)->ds_guid;
	} else {
		zb->zbm_creation_txg =
		    dsl_dataset_phys(walker1)->ds_creation_txg;
		zb->zbm_creation_time =
		    dsl_dataset_phys(walker1)->ds_creation_time;
		zb->zbm_guid = dsl_dataset_phys(walker1)->ds_guid;
	}
	zb->zbm_redaction_obj = 0;
	dsl_dataset_rele(walker1, FTAG);
	dsl_dataset_rele(walker2, FTAG);
	return (err);
}

int
dmu_send_obj(const char *pool, uint64_t tosnap, uint64_t fromsnap,
    boolean_t embedok, boolean_t large_block_ok, boolean_t compressok,
    int outfd, vnode_t *vp, offset_t *off)
{
	int err;
	struct dmu_send_params dspp = {0};
	dspp.embedok = embedok;
	dspp.large_block_ok = large_block_ok;
	dspp.compressok = compressok;
	dspp.outfd = outfd;
	dspp.vp = vp;
	dspp.off = off;
	dspp.tag = FTAG;

	err = dsl_pool_hold(pool, FTAG, &dspp.dp);
	if (err != 0)
		return (err);

	err = dsl_dataset_hold_obj(dspp.dp, tosnap, FTAG, &dspp.to_ds);
	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (fromsnap != 0) {

		err = dsl_dataset_hold_obj(dspp.dp, fromsnap, FTAG,
		    &dspp.from_ds);
		if (err != 0) {
			dsl_dataset_rele(dspp.to_ds, FTAG);
			dsl_pool_rele(dspp.dp, FTAG);
			return (err);
		}

		err = find_common_ancestor(dspp.dp, dspp.from_ds, dspp.to_ds,
		    &dspp.ancestor_zb);
		if (err != 0) {
			dsl_dataset_rele(dspp.to_ds, FTAG);
			dsl_dataset_rele(dspp.from_ds, FTAG);
			dsl_pool_rele(dspp.dp, FTAG);
			return (err);
		}
		/* See dmu_send for the reasons behind this. */
		uint64_t *fromredact;

		if (!dsl_dataset_get_uint64_array_feature(dspp.from_ds,
		    SPA_FEATURE_REDACTED_DATASETS,
		    &dspp.numfromredactsnaps,
		    &fromredact)) {
			dspp.numfromredactsnaps = UINT64_MAX;
		} else if (dspp.numfromredactsnaps > 0) {
			uint64_t size = dspp.numfromredactsnaps *
			    sizeof (uint64_t);
			dspp.fromredactsnaps = kmem_zalloc(size, KM_SLEEP);
			bcopy(fromredact, dspp.fromredactsnaps, size);
		}

		if (dsl_dataset_is_before(dspp.to_ds, dspp.from_ds, 0)) {
			dspp.is_clone = (dspp.to_ds->ds_dir !=
			    dspp.from_ds->ds_dir);
			dsl_dataset_rele(dspp.from_ds, FTAG);
			dspp.from_ds = NULL;
		}

		err = dmu_send_impl(&dspp);

		if (dspp.from_ds != NULL)
			dsl_dataset_rele(dspp.from_ds, FTAG);
	} else {
		dspp.numfromredactsnaps = UINT64_MAX;
		err = dmu_send_impl(&dspp);
	}
	dsl_dataset_rele(dspp.to_ds, FTAG);
	return (err);
}

int
dmu_send(const char *tosnap, const char *fromsnap, boolean_t embedok,
    boolean_t large_block_ok, boolean_t compressok, int outfd,
    uint64_t resumeobj, uint64_t resumeoff,
    nvlist_t *redactsnaps, const char *redactbook, vnode_t *vp, offset_t *off)
{
	int err = 0;
	boolean_t owned = B_FALSE;
	struct dmu_send_params dspp = {0};
	dspp.tosnap = tosnap;
	dspp.embedok = embedok;
	dspp.large_block_ok = large_block_ok;
	dspp.compressok = compressok;
	dspp.outfd = outfd;
	dspp.vp = vp;
	dspp.off = off;
	dspp.tag = FTAG;
	dspp.redactbook = redactbook;
	dspp.resumeobj = resumeobj;
	dspp.resumeoff = resumeoff;

	if (fromsnap != NULL && strpbrk(fromsnap, "@#") == NULL)
		return (SET_ERROR(EINVAL));

	if ((redactbook != NULL && redactsnaps == NULL) ||
	    (redactsnaps != NULL && redactbook == NULL))
		return (SET_ERROR(EINVAL));

	err = dsl_pool_hold(tosnap, FTAG, &dspp.dp);
	if (err != 0) {
		return (err);
	}

	if (strchr(tosnap, '@') == NULL && spa_writeable(dspp.dp->dp_spa)) {
		/*
		 * We are sending a filesystem or volume.  Ensure
		 * that it doesn't change by owning the dataset.
		 */
		err = dsl_dataset_own(dspp.dp, tosnap, FTAG, &dspp.to_ds);
		owned = B_TRUE;
	} else {
		err = dsl_dataset_hold(dspp.dp, tosnap, FTAG, &dspp.to_ds);
	}

	if (err != 0) {
		dsl_pool_rele(dspp.dp, FTAG);
		return (err);
	}

	if (redactsnaps != NULL) {
		nvpair_t *pair;
		if (redactbook == NULL) {
			err = EINVAL;
		}

		if (fnvlist_num_pairs(redactsnaps) > 0 && err == 0) {
			dspp.redactsnaparr =
			    kmem_zalloc(fnvlist_num_pairs(redactsnaps) *
			    sizeof (dsl_dataset_t *), KM_SLEEP);
		}

		for (pair = nvlist_next_nvpair(redactsnaps, NULL); err == 0 &&
		    pair != NULL; pair =
		    nvlist_next_nvpair(redactsnaps, pair)) {
			const char *name = nvpair_name(pair);
			err = dsl_dataset_hold(dspp.dp, name, FTAG,
			    dspp.redactsnaparr + dspp.numredactsnaps);
			if (err != 0)
				break;
			if (!dsl_dataset_is_before(
			    dspp.redactsnaparr[dspp.numredactsnaps], dspp.to_ds,
			    0)) {
				err = EINVAL;
				dspp.numredactsnaps++;
				break;
			}
			dspp.numredactsnaps++;
		}
	}

	if (err != 0) {
		for (int i = 0; i < dspp.numredactsnaps; i++)
			dsl_dataset_rele(dspp.redactsnaparr[i], FTAG);

		if (dspp.redactsnaparr != NULL) {
			kmem_free(dspp.redactsnaparr,
			    fnvlist_num_pairs(redactsnaps) *
			    sizeof (dsl_dataset_t *));
		}

		dsl_pool_rele(dspp.dp, FTAG);
		if (owned)
			dsl_dataset_disown(dspp.to_ds, FTAG);
		else
			dsl_dataset_rele(dspp.to_ds, FTAG);
		return (SET_ERROR(err));
	}

	if (fromsnap != NULL) {
		zfs_bookmark_phys_t *zb = &dspp.ancestor_zb;
		int fsnamelen = strpbrk(tosnap, "@#") - tosnap;
		/*
		 * If the fromsnap is in a different filesystem, then
		 * mark the send stream as a clone.
		 */
		if (strncmp(tosnap, fromsnap, fsnamelen) != 0 ||
		    (fromsnap[fsnamelen] != '@' &&
		    fromsnap[fsnamelen] != '#')) {
			dspp.is_clone = B_TRUE;
		}

		if (strchr(fromsnap, '@')) {
			err = dsl_dataset_hold(dspp.dp, fromsnap, FTAG,
			    &dspp.from_ds);

			if (err != 0) {
				ASSERT3P(dspp.from_ds, ==, NULL);
			} else {
				/*
				 * We need to make a deep copy of the redact
				 * snapshots of the from snapshot, because the
				 * array will be freed when we evict from_ds.
				 */
				uint64_t *fromredact;
				if (!dsl_dataset_get_uint64_array_feature(
				    dspp.from_ds, SPA_FEATURE_REDACTED_DATASETS,
				    &dspp.numfromredactsnaps,
				    &fromredact)) {
					dspp.numfromredactsnaps = UINT64_MAX;
				} else if (dspp.numfromredactsnaps > 0) {
					uint64_t size =
					    dspp.numfromredactsnaps *
					    sizeof (uint64_t);
					dspp.fromredactsnaps = kmem_zalloc(size,
					    KM_SLEEP);
					bcopy(fromredact, dspp.fromredactsnaps,
					    size);
				}
				if (dsl_dataset_is_before(dspp.to_ds,
				    dspp.from_ds, 0)) {
					ASSERT3U(dspp.is_clone, ==,
					    (dspp.to_ds->ds_dir !=
					    dspp.from_ds->ds_dir));
					zb->zbm_creation_txg =
					    dsl_dataset_phys(
					    dspp.from_ds)->ds_creation_txg;
					zb->zbm_creation_time =
					    dsl_dataset_phys(
					    dspp.from_ds)->ds_creation_time;
					zb->zbm_guid = dsl_dataset_phys(
					    dspp.from_ds)->ds_guid;
					zb->zbm_redaction_obj = 0;
					dsl_dataset_rele(dspp.from_ds, FTAG);
					dspp.from_ds = NULL;
				} else {
					dspp.is_clone = B_FALSE;
					err = find_common_ancestor(dspp.dp,
					    dspp.from_ds, dspp.to_ds, zb);
				}
			}
		} else {
			dspp.numfromredactsnaps = UINT64_MAX;
			err = dsl_bookmark_lookup(dspp.dp, fromsnap, dspp.to_ds,
			    zb);
		}

		if (err == 0) {
			/* dmu_send_impl will call dsl_pool_rele for us. */
			err = dmu_send_impl(&dspp);
		} else {
			dsl_pool_rele(dspp.dp, FTAG);
		}

		if (dspp.from_ds != NULL)
			dsl_dataset_rele(dspp.from_ds, FTAG);
	} else {
		dspp.numfromredactsnaps = UINT64_MAX;
		err = dmu_send_impl(&dspp);
	}
out:
	if (dspp.numredactsnaps > 0) {
		for (int i = 0; i < dspp.numredactsnaps; i++)
			dsl_dataset_rele(dspp.redactsnaparr[i], FTAG);
		kmem_free(dspp.redactsnaparr, fnvlist_num_pairs(redactsnaps) *
		    sizeof (dsl_dataset_t *));
	}

	if (owned)
		dsl_dataset_disown(dspp.to_ds, FTAG);
	else
		dsl_dataset_rele(dspp.to_ds, FTAG);
	return (err);
}

static int
dmu_adjust_send_estimate_for_indirects(dsl_dataset_t *ds, uint64_t uncompressed,
    uint64_t compressed, boolean_t stream_compressed, uint64_t *sizep)
{
	int err;
	uint64_t size;
	/*
	 * Assume that space (both on-disk and in-stream) is dominated by
	 * data.  We will adjust for indirect blocks and the copies property,
	 * but ignore per-object space used (eg, dnodes and DRR_OBJECT records).
	 */
	uint64_t recordsize;
	uint64_t record_count;

	/* Assume all (uncompressed) blocks are recordsize. */
	err = dsl_prop_get_int_ds(ds, zfs_prop_to_name(ZFS_PROP_RECORDSIZE),
	    &recordsize);
	if (err != 0)
		return (err);
	record_count = uncompressed / recordsize;

	/*
	 * If we're estimating a send size for a compressed stream, use the
	 * compressed data size to estimate the stream size. Otherwise, use the
	 * uncompressed data size.
	 */
	size = stream_compressed ? compressed : uncompressed;

	/*
	 * Subtract out approximate space used by indirect blocks.
	 * Assume most space is used by data blocks (non-indirect, non-dnode).
	 * Assume no ditto blocks or internal fragmentation.
	 *
	 * Therefore, space used by indirect blocks is sizeof(blkptr_t) per
	 * block.
	 */
	size -= record_count * sizeof (blkptr_t);

	/* Add in the space for the record associated with each block. */
	size += record_count * sizeof (dmu_replay_record_t);

	*sizep = size;

	return (0);
}

int
dmu_send_estimate(dsl_dataset_t *ds, dsl_dataset_t *fromds,
    boolean_t stream_compressed, uint64_t *sizep)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	int err;
	uint64_t uncomp, comp;

	ASSERT(dsl_pool_config_held(dp));

	/* tosnap must be a snapshot */
	if (!ds->ds_is_snapshot)
		return (SET_ERROR(EINVAL));

	/*
	 * fromsnap must be an earlier snapshot from the same fs as tosnap,
	 * or the origin's fs.
	 */
	if (fromds != NULL && !dsl_dataset_is_before(ds, fromds, 0))
		return (SET_ERROR(EXDEV));

	/* Get compressed and uncompressed size estimates of changed data. */
	if (fromds == NULL) {
		uncomp = dsl_dataset_phys(ds)->ds_uncompressed_bytes;
		comp = dsl_dataset_phys(ds)->ds_compressed_bytes;
	} else {
		uint64_t used;
		err = dsl_dataset_space_written(fromds, ds,
		    &used, &comp, &uncomp);
		if (err != 0)
			return (err);
	}

	err = dmu_adjust_send_estimate_for_indirects(ds, uncomp, comp,
	    stream_compressed, sizep);
	return (err);
}

struct calculate_send_arg {
	uint64_t uncompressed;
	uint64_t compressed;
};

/*
 * Simple callback used to traverse the blocks of a snapshot and sum their
 * uncompressed and compressed sizes.
 */
/* ARGSUSED */
static int
dmu_calculate_send_traversal(spa_t *spa, zilog_t *zilog, const blkptr_t *bp,
    const zbookmark_phys_t *zb, const dnode_phys_t *dnp, void *arg)
{
	struct calculate_send_arg *space = arg;
	if (bp != NULL && !BP_IS_HOLE(bp)) {
		space->uncompressed += BP_GET_UCSIZE(bp);
		space->compressed += BP_GET_PSIZE(bp);
	}
	return (0);
}

/*
 * Given a desination snapshot and a TXG, calculate the approximate size of a
 * send stream sent from that TXG. from_txg may be zero, indicating that the
 * whole snapshot will be sent.
 */
int
dmu_send_estimate_from_txg(dsl_dataset_t *ds, uint64_t from_txg,
    boolean_t stream_compressed, uint64_t *sizep)
{
	dsl_pool_t *dp = ds->ds_dir->dd_pool;
	int err;
	struct calculate_send_arg size = { 0 };

	ASSERT(dsl_pool_config_held(dp));

	/* tosnap must be a snapshot */
	if (!ds->ds_is_snapshot)
		return (SET_ERROR(EINVAL));

	// verify that from_txg is before the provided snapshot was taken
	if (from_txg >= dsl_dataset_phys(ds)->ds_creation_txg) {
		return (SET_ERROR(EXDEV));
	}

	/*
	 * traverse the blocks of the snapshot with birth times after
	 * from_txg, summing their uncompressed size
	 */
	err = traverse_dataset(ds, from_txg, TRAVERSE_POST,
	    dmu_calculate_send_traversal, &size);
	if (err)
		return (err);

	err = dmu_adjust_send_estimate_for_indirects(ds, size.uncompressed,
	    size.compressed, stream_compressed, sizep);
	return (err);
}

static int receive_read_payload_and_next_header(dmu_recv_cookie_t *ra, int len,
    void *buf);

struct receive_record_arg {
	dmu_replay_record_t header;
	void *payload; /* Pointer to a buffer containing the payload */
	/*
	 * If the record is a write, pointer to the arc_buf_t containing the
	 * payload.
	 */
	arc_buf_t *write_buf;
	int payload_size;
	uint64_t bytes_read; /* bytes read from stream when record created */
	boolean_t eos_marker; /* Marks the end of the stream */
	bqueue_node_t node;
};

struct receive_writer_arg {
	objset_t *os;
	boolean_t byteswap;
	bqueue_t q;

	/*
	 * These three args are used to signal to the main thread that we're
	 * done.
	 */
	kmutex_t mutex;
	kcondvar_t cv;
	boolean_t done;

	int err;
	/* A map from guid to dataset to help handle dedup'd streams. */
	avl_tree_t *guid_to_ds_map;
	boolean_t resumable;
	uint64_t last_object, last_offset;
	uint64_t bytes_read; /* bytes read when current record created */
};

struct objlist {
	list_t list; /* List of struct receive_objnode. */
	/*
	 * Last object looked up. Used to assert that objects are being looked
	 * up in ascending order.
	 */
	uint64_t last_lookup;
};

struct receive_objnode {
	list_node_t node;
	uint64_t object;
};

typedef struct guid_map_entry {
	uint64_t	guid;
	dsl_dataset_t	*gme_ds;
	avl_node_t	avlnode;
} guid_map_entry_t;

typedef struct dmu_recv_begin_arg {
	const char *drba_origin;
	dmu_recv_cookie_t *drba_cookie;
	cred_t *drba_cred;
	uint64_t drba_snapobj;
} dmu_recv_begin_arg_t;

static int
resume_check(dmu_recv_cookie_t *drc, nvlist_t *begin_nvl)
{
	uint64_t val;
	objset_t *mos = dmu_objset_pool(drc->drc_os)->dp_meta_objset;
	uint64_t dsobj = dmu_objset_id(drc->drc_os);
	uint64_t resume_obj, resume_off;

	if (nvlist_lookup_uint64(begin_nvl,
	    BEGINNV_RESUME_OBJECT, &resume_obj) != 0 ||
	    nvlist_lookup_uint64(begin_nvl,
	    BEGINNV_RESUME_OFFSET, &resume_off) != 0) {
		return (SET_ERROR(EINVAL));
	}
	VERIFY0(zap_lookup(mos, dsobj,
	    DS_FIELD_RESUME_OBJECT, sizeof (val), 1, &val));
	if (resume_obj != val)
		return (SET_ERROR(EINVAL));
	VERIFY0(zap_lookup(mos, dsobj,
	    DS_FIELD_RESUME_OFFSET, sizeof (val), 1, &val));
	if (resume_off != val)
		return (SET_ERROR(EINVAL));

	return (0);
}

/*
 * Check that the new stream we're trying to receive is redacted with respect to
 * a subset of the snapshots that the origin was redacted with respect to.  For
 * the reasons behind this, see the man page on redacted zfs sends and receives.
 */
static boolean_t
compatible_redact_snaps(uint64_t *origin_snaps, uint64_t origin_num_snaps,
    uint64_t *redact_snaps, uint64_t num_redact_snaps)
{
	/*
	 * Short circuit the comparison; if we are redacted with respect to
	 * more snapshots than the origin, we can't be redacted with respect
	 * to a subset.
	 */
	if (num_redact_snaps > origin_num_snaps) {
		return (B_FALSE);
	}

	for (int i = 0; i < num_redact_snaps; i++) {
		if (!redact_snaps_contains(origin_snaps, origin_num_snaps,
		    redact_snaps[i])) {
			return (B_FALSE);
		}
	}
	return (B_TRUE);
}

static boolean_t
redact_check(dmu_recv_begin_arg_t *drba, dsl_dataset_t *origin)
{
	uint64_t *origin_snaps;
	uint64_t origin_num_snaps;
	dmu_recv_cookie_t *drc = drba->drba_cookie;
	struct drr_begin *drrb = drc->drc_drrb;
	int featureflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
	int err = 0;
	boolean_t ret = B_TRUE;
	uint64_t *redact_snaps;
	uint_t numredactsnaps;

	/*
	 * If this is a full send stream, we're safe no matter what.
	 */
	if (drrb->drr_fromguid == 0)
		return (ret);

	VERIFY(dsl_dataset_get_uint64_array_feature(origin,
	    SPA_FEATURE_REDACTED_DATASETS, &origin_num_snaps, &origin_snaps));

	if (nvlist_lookup_uint64_array(drc->drc_begin_nvl,
	    BEGINNV_REDACT_FROM_SNAPS, &redact_snaps, &numredactsnaps) ==
	    0) {
		/*
		 * If the send stream was sent from the redaction bookmark or
		 * the redacted version of the dataset, then we're safe.  Verify
		 * that this is from the a compatible redaction bookmark or
		 * redacted dataset.
		 */
		if (!compatible_redact_snaps(origin_snaps, origin_num_snaps,
		    redact_snaps, numredactsnaps)) {
			err = EINVAL;
		}
	} else if (featureflags & DMU_BACKUP_FEATURE_REDACTED) {
		/*
		 * If the stream is redacted, it must be redacted with respect
		 * to a subset of what the origin is redacted with respect to.
		 * See case number 2 in the zfs man page section on redacted zfs
		 * send.
		 */
		err = nvlist_lookup_uint64_array(drc->drc_begin_nvl,
		    BEGINNV_REDACT_SNAPS, &redact_snaps, &numredactsnaps);

		if (err != 0 || !compatible_redact_snaps(origin_snaps,
		    origin_num_snaps, redact_snaps, numredactsnaps)) {
			err = EINVAL;
		}
	} else if (!redact_snaps_contains(origin_snaps, origin_num_snaps,
	    drrb->drr_toguid)) {
		/*
		 * If the stream isn't redacted but the origin is, this must be
		 * one of the snapshots the origin is redacted with respect to.
		 * See case number 1 in the zfs man page section on redacted zfs
		 * send.
		 */
		err = EINVAL;
	}

	if (err != 0)
		ret = B_FALSE;
	return (ret);
}

static int
recv_begin_check_existing_impl(dmu_recv_begin_arg_t *drba, dsl_dataset_t *ds,
    uint64_t fromguid)
{
	uint64_t val;
	int error;
	dsl_pool_t *dp = ds->ds_dir->dd_pool;

	/* temporary clone name must not exist */
	error = zap_lookup(dp->dp_meta_objset,
	    dsl_dir_phys(ds->ds_dir)->dd_child_dir_zapobj, recv_clone_name,
	    8, 1, &val);
	if (error != ENOENT)
		return (error == 0 ? EBUSY : error);

	/* new snapshot name must not exist */
	error = zap_lookup(dp->dp_meta_objset,
	    dsl_dataset_phys(ds)->ds_snapnames_zapobj,
	    drba->drba_cookie->drc_tosnap, 8, 1, &val);
	if (error != ENOENT)
		return (error == 0 ? EEXIST : error);

	/*
	 * Check snapshot limit before receiving.  We'll recheck again at the
	 * end, but might as well abort before receiving if we're already over
	 * the limit.
	 *
	 * Note that we do not check the file system limit with
	 * dsl_dir_fscount_check because the temporary %clones don't count
	 * against that limit.
	 */
	error = dsl_fs_ss_limit_check(ds->ds_dir, 1, ZFS_PROP_SNAPSHOT_LIMIT,
	    NULL, drba->drba_cred);
	if (error != 0)
		return (error);

	if (fromguid != 0) {
		dsl_dataset_t *snap;
		uint64_t obj = dsl_dataset_phys(ds)->ds_prev_snap_obj;

		/* Find snapshot in this dir that matches fromguid. */
		while (obj != 0) {
			error = dsl_dataset_hold_obj(dp, obj, FTAG,
			    &snap);
			if (error != 0)
				return (SET_ERROR(ENODEV));
			if (snap->ds_dir != ds->ds_dir) {
				dsl_dataset_rele(snap, FTAG);
				return (SET_ERROR(ENODEV));
			}
			if (dsl_dataset_phys(snap)->ds_guid == fromguid)
				break;
			obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
			dsl_dataset_rele(snap, FTAG);
		}
		if (obj == 0)
			return (SET_ERROR(ENODEV));

		if (drba->drba_cookie->drc_force) {
			drba->drba_snapobj = obj;
		} else {
			/*
			 * If we are not forcing, there must be no
			 * changes since fromsnap.
			 */
			if (dsl_dataset_modified_since_snap(ds, snap)) {
				dsl_dataset_rele(snap, FTAG);
				return (SET_ERROR(ETXTBSY));
			}
			drba->drba_snapobj = ds->ds_prev->ds_object;
		}

		if (dsl_dataset_feature_is_active(snap,
		    SPA_FEATURE_REDACTED_DATASETS) && !redact_check(drba,
		    snap)) {
			dsl_dataset_rele(snap, FTAG);
			return (SET_ERROR(EINVAL));
		}

		dsl_dataset_rele(snap, FTAG);
	} else {
		/* if full, most recent snapshot must be $ORIGIN */
		if (dsl_dataset_phys(ds)->ds_prev_snap_txg >= TXG_INITIAL)
			return (SET_ERROR(ENODEV));
		drba->drba_snapobj = dsl_dataset_phys(ds)->ds_prev_snap_obj;
	}

	return (0);

}

static int
dmu_recv_begin_check(void *arg, dmu_tx_t *tx)
{
	dmu_recv_begin_arg_t *drba = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	struct drr_begin *drrb = drba->drba_cookie->drc_drrb;
	uint64_t fromguid = drrb->drr_fromguid;
	int flags = drrb->drr_flags;
	int error;
	uint64_t featureflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
	dsl_dataset_t *ds;
	const char *tofs = drba->drba_cookie->drc_tofs;

	/* already checked */
	ASSERT3U(drrb->drr_magic, ==, DMU_BACKUP_MAGIC);
	ASSERT(!(featureflags & DMU_BACKUP_FEATURE_RESUMING));

	if (DMU_GET_STREAM_HDRTYPE(drrb->drr_versioninfo) ==
	    DMU_COMPOUNDSTREAM ||
	    drrb->drr_type >= DMU_OST_NUMTYPES ||
	    ((flags & DRR_FLAG_CLONE) && drba->drba_origin == NULL))
		return (SET_ERROR(EINVAL));

	/* Verify pool version supports SA if SA_SPILL feature set */
	if ((featureflags & DMU_BACKUP_FEATURE_SA_SPILL) &&
	    spa_version(dp->dp_spa) < SPA_VERSION_SA)
		return (SET_ERROR(ENOTSUP));

	if (drba->drba_cookie->drc_resumable &&
	    !spa_feature_is_enabled(dp->dp_spa,
	    SPA_FEATURE_EXTENSIBLE_DATASET))
		return (SET_ERROR(ENOTSUP));

	/*
	 * The receiving code doesn't know how to translate a WRITE_EMBEDDED
	 * record to a plain WRITE record, so the pool must have the
	 * EMBEDDED_DATA feature enabled if the stream has WRITE_EMBEDDED
	 * records.  Same with WRITE_EMBEDDED records that use LZ4 compression.
	 */
	if ((featureflags & DMU_BACKUP_FEATURE_EMBED_DATA) &&
	    !spa_feature_is_enabled(dp->dp_spa, SPA_FEATURE_EMBEDDED_DATA))
		return (SET_ERROR(ENOTSUP));
	if ((featureflags & DMU_BACKUP_FEATURE_LZ4) &&
	    !spa_feature_is_enabled(dp->dp_spa, SPA_FEATURE_LZ4_COMPRESS))
		return (SET_ERROR(ENOTSUP));

	/*
	 * The receiving code doesn't know how to translate large blocks
	 * to smaller ones, so the pool must have the LARGE_BLOCKS
	 * feature enabled if the stream has LARGE_BLOCKS.
	 */
	if ((featureflags & DMU_BACKUP_FEATURE_LARGE_BLOCKS) &&
	    !spa_feature_is_enabled(dp->dp_spa, SPA_FEATURE_LARGE_BLOCKS))
		return (SET_ERROR(ENOTSUP));

	if ((featureflags & DMU_BACKUP_FEATURE_REDACTED) &&
	    (!spa_feature_is_enabled(dp->dp_spa,
	    SPA_FEATURE_REDACTED_DATASETS) ||
	    !spa_feature_is_enabled(dp->dp_spa,
	    SPA_FEATURE_EXTENSIBLE_DATASET)))
		return (SET_ERROR(ENOTSUP));

	error = dsl_dataset_hold(dp, tofs, FTAG, &ds);
	if (error == 0) {
		/* target fs already exists; recv into temp clone */

		/* Can't recv a clone into an existing fs */
		if (flags & DRR_FLAG_CLONE || drba->drba_origin) {
			dsl_dataset_rele(ds, FTAG);
			return (SET_ERROR(EINVAL));
		}

		error = recv_begin_check_existing_impl(drba, ds, fromguid);
		dsl_dataset_rele(ds, FTAG);
	} else if (error == ENOENT) {
		/* target fs does not exist; must be a full backup or clone */
		char buf[ZFS_MAX_DATASET_NAME_LEN];

		/*
		 * If it's a non-clone incremental, we are missing the
		 * target fs, so fail the recv.
		 */
		if (fromguid != 0 && !((flags & DRR_FLAG_CLONE) ||
		    drba->drba_origin))
			return (SET_ERROR(ENOENT));

		/*
		 * If we're receiving a full send as a clone, and it doesn't
		 * contain all the necessary free records and freeobject
		 * records, reject it.
		 */
		if (fromguid == 0 && drba->drba_origin != NULL &&
		    !(flags & DRR_FLAG_FREERECORDS))
			return (SET_ERROR(EINVAL));

		/* Open the parent of tofs */
		ASSERT3U(strlen(tofs), <, sizeof (buf));
		(void) strlcpy(buf, tofs, strrchr(tofs, '/') - tofs + 1);
		error = dsl_dataset_hold(dp, buf, FTAG, &ds);
		if (error != 0)
			return (error);

		/*
		 * Check filesystem and snapshot limits before receiving.  We'll
		 * recheck snapshot limits again at the end (we create the
		 * filesystems and increment those counts during begin_sync).
		 */
		error = dsl_fs_ss_limit_check(ds->ds_dir, 1,
		    ZFS_PROP_FILESYSTEM_LIMIT, NULL, drba->drba_cred);
		if (error != 0) {
			dsl_dataset_rele(ds, FTAG);
			return (error);
		}

		error = dsl_fs_ss_limit_check(ds->ds_dir, 1,
		    ZFS_PROP_SNAPSHOT_LIMIT, NULL, drba->drba_cred);
		if (error != 0) {
			dsl_dataset_rele(ds, FTAG);
			return (error);
		}

		if (drba->drba_origin != NULL) {
			dsl_dataset_t *origin;
			error = dsl_dataset_hold(dp, drba->drba_origin,
			    FTAG, &origin);
			if (error != 0) {
				dsl_dataset_rele(ds, FTAG);
				return (error);
			}
			if (!origin->ds_is_snapshot) {
				dsl_dataset_rele(origin, FTAG);
				dsl_dataset_rele(ds, FTAG);
				return (SET_ERROR(EINVAL));
			}
			if (dsl_dataset_phys(origin)->ds_guid != fromguid &&
			    fromguid != 0) {
				dsl_dataset_rele(origin, FTAG);
				dsl_dataset_rele(ds, FTAG);
				return (SET_ERROR(ENODEV));
			}

			/*
			 * If the origin is redacted we need to verify that this
			 * send stream can safely be received on top of the
			 * origin.
			 */
			if (dsl_dataset_feature_is_active(origin,
			    SPA_FEATURE_REDACTED_DATASETS)) {
				if (!redact_check(drba, origin)) {
					dsl_dataset_rele(origin, FTAG);
					dsl_dataset_rele(ds, FTAG);
					return (SET_ERROR(EINVAL));
				}
			}

			dsl_dataset_rele(origin, FTAG);
		}
		dsl_dataset_rele(ds, FTAG);
		error = 0;
	}
	return (error);
}

static void
dmu_recv_begin_sync(void *arg, dmu_tx_t *tx)
{
	dmu_recv_begin_arg_t *drba = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	objset_t *mos = dp->dp_meta_objset;
	dmu_recv_cookie_t *drc = drba->drba_cookie;
	struct drr_begin *drrb = drc->drc_drrb;
	const char *tofs = drc->drc_tofs;
	dsl_dataset_t *ds, *newds;
	uint64_t dsobj;
	int error;
	uint64_t crflags = 0;

	if (drrb->drr_flags & DRR_FLAG_CI_DATA)
		crflags |= DS_FLAG_CI_DATASET;

	error = dsl_dataset_hold(dp, tofs, FTAG, &ds);
	if (error == 0) {
		/* create temporary clone */
		dsl_dataset_t *snap = NULL;
		if (drba->drba_snapobj != 0) {
			VERIFY0(dsl_dataset_hold_obj(dp,
			    drba->drba_snapobj, FTAG, &snap));
		}
		dsobj = dsl_dataset_create_sync(ds->ds_dir, recv_clone_name,
		    snap, crflags, drba->drba_cred, tx);
		if (drba->drba_snapobj != 0)
			dsl_dataset_rele(snap, FTAG);
		dsl_dataset_rele(ds, FTAG);
	} else {
		dsl_dir_t *dd;
		const char *tail;
		dsl_dataset_t *origin = NULL;

		VERIFY0(dsl_dir_hold(dp, tofs, FTAG, &dd, &tail));

		if (drba->drba_origin != NULL) {
			VERIFY0(dsl_dataset_hold(dp, drba->drba_origin,
			    FTAG, &origin));
		}

		/* Create new dataset. */
		dsobj = dsl_dataset_create_sync(dd,
		    strrchr(tofs, '/') + 1,
		    origin, crflags, drba->drba_cred, tx);
		if (origin != NULL)
			dsl_dataset_rele(origin, FTAG);

		dsl_dir_rele(dd, FTAG);
		drc->drc_newfs = B_TRUE;
	}

	VERIFY0(dsl_dataset_own_obj_force(dp, dsobj, dmu_recv_tag, &newds));
	if (dsl_dataset_feature_is_active(newds,
	    SPA_FEATURE_REDACTED_DATASETS)) {
		/*
		 * If the origin dataset is redacted, the child will be redacted
		 * when we create it.  We clear the new dataset's
		 * redaction info; if it should be redacted, we'll fill
		 * in its information later.
		 */
		dsl_dataset_deactivate_feature(newds,
		    SPA_FEATURE_REDACTED_DATASETS, tx);
	}

	if (drc->drc_resumable) {
		dsl_dataset_zapify(newds, tx);
		if (drrb->drr_fromguid != 0) {
			VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_FROMGUID,
			    8, 1, &drrb->drr_fromguid, tx));
		}
		VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_TOGUID,
		    8, 1, &drrb->drr_toguid, tx));
		VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_TONAME,
		    1, strlen(drrb->drr_toname) + 1, drrb->drr_toname, tx));
		uint64_t one = 1;
		uint64_t zero = 0;
		VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_OBJECT,
		    8, 1, &one, tx));
		VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_OFFSET,
		    8, 1, &zero, tx));
		VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_BYTES,
		    8, 1, &zero, tx));
		if (DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo) &
		    DMU_BACKUP_FEATURE_LARGE_BLOCKS) {
			VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_LARGEBLOCK,
			    8, 1, &one, tx));
		}
		if (DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo) &
		    DMU_BACKUP_FEATURE_EMBED_DATA) {
			VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_EMBEDOK,
			    8, 1, &one, tx));
		}
		if (DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo) &
		    DMU_BACKUP_FEATURE_COMPRESSED) {
			VERIFY0(zap_add(mos, dsobj, DS_FIELD_RESUME_COMPRESSOK,
			    8, 1, &one, tx));
		}

		uint64_t *redact_snaps;
		uint_t numredactsnaps;
		if (nvlist_lookup_uint64_array(drc->drc_begin_nvl,
		    BEGINNV_REDACT_FROM_SNAPS, &redact_snaps,
		    &numredactsnaps) == 0) {
			VERIFY0(zap_add(mos, dsobj,
			    DS_FIELD_RESUME_REDACT_BOOKMARK_SNAPS,
			    sizeof (*redact_snaps), numredactsnaps,
			    redact_snaps, tx));
		}
	}

	if ((DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo) &
	    DMU_BACKUP_FEATURE_EMBED_MOOCH_BYTESWAP) &&
	    !dsl_dataset_feature_is_active(newds, SPA_FEATURE_MOOCH_BYTESWAP)) {
		dsl_dataset_activate_mooch_byteswap_sync(newds, tx);
	}

	if (DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo) &
	    DMU_BACKUP_FEATURE_REDACTED) {
		uint64_t *redact_snaps;
		uint_t numredactsnaps;
		VERIFY0(nvlist_lookup_uint64_array(drc->drc_begin_nvl,
		    BEGINNV_REDACT_SNAPS, &redact_snaps, &numredactsnaps));
		dsl_dataset_activate_redaction(newds, redact_snaps,
		    numredactsnaps, tx);
	}

	dmu_buf_will_dirty(newds->ds_dbuf, tx);

	dsl_dataset_phys(newds)->ds_flags |= DS_FLAG_INCONSISTENT;

	/*
	 * If we actually created a non-clone, we need to create the
	 * objset in our new dataset.
	 */
	rrw_enter(&newds->ds_bp_rwlock, RW_READER, FTAG);
	if (BP_IS_HOLE(dsl_dataset_get_blkptr(newds))) {
		(void) dmu_objset_create_impl(dp->dp_spa,
		    newds, dsl_dataset_get_blkptr(newds), drrb->drr_type, tx);
	}
	rrw_exit(&newds->ds_bp_rwlock, FTAG);

	drba->drba_cookie->drc_ds = newds;

	spa_history_log_internal_ds(newds, "receive", tx, "");
}

static int
dmu_recv_resume_begin_check(void *arg, dmu_tx_t *tx)
{
	dmu_recv_begin_arg_t *drba = arg;
	dmu_recv_cookie_t *drc = drba->drba_cookie;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	struct drr_begin *drrb = drc->drc_drrb;
	int error;
	uint64_t featureflags = DMU_GET_FEATUREFLAGS(drrb->drr_versioninfo);
	dsl_dataset_t *ds;
	const char *tofs = drc->drc_tofs;

	/* already checked */
	ASSERT3U(drrb->drr_magic, ==, DMU_BACKUP_MAGIC);
	ASSERT(featureflags & DMU_BACKUP_FEATURE_RESUMING);

	if (DMU_GET_STREAM_HDRTYPE(drrb->drr_versioninfo) ==
	    DMU_COMPOUNDSTREAM ||
	    drrb->drr_type >= DMU_OST_NUMTYPES)
		return (SET_ERROR(EINVAL));

	/* Verify pool version supports SA if SA_SPILL feature set */
	if ((featureflags & DMU_BACKUP_FEATURE_SA_SPILL) &&
	    spa_version(dp->dp_spa) < SPA_VERSION_SA)
		return (SET_ERROR(ENOTSUP));

	/*
	 * The receiving code doesn't know how to translate a WRITE_EMBEDDED
	 * record to a plain WRITE record, so the pool must have the
	 * EMBEDDED_DATA feature enabled if the stream has WRITE_EMBEDDED
	 * records.  Same with WRITE_EMBEDDED records that use LZ4 compression.
	 */
	if ((featureflags & DMU_BACKUP_FEATURE_EMBED_DATA) &&
	    !spa_feature_is_enabled(dp->dp_spa, SPA_FEATURE_EMBEDDED_DATA))
		return (SET_ERROR(ENOTSUP));
	if ((featureflags & DMU_BACKUP_FEATURE_LZ4) &&
	    !spa_feature_is_enabled(dp->dp_spa, SPA_FEATURE_LZ4_COMPRESS))
		return (SET_ERROR(ENOTSUP));

	/* 6 extra bytes for /%recv */
	char recvname[ZFS_MAX_DATASET_NAME_LEN + 6];

	(void) snprintf(recvname, sizeof (recvname), "%s/%s",
	    tofs, recv_clone_name);

	if (dsl_dataset_hold(dp, recvname, FTAG, &ds) != 0) {
		/* %recv does not exist; continue in tofs */
		error = dsl_dataset_hold(dp, tofs, FTAG, &ds);
		if (error != 0)
			return (error);
	}

	/* check that ds is marked inconsistent */
	if (!DS_IS_INCONSISTENT(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/* check that there is resuming data, and that the toguid matches */
	if (!dsl_dataset_is_zapified(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}
	uint64_t val;
	error = zap_lookup(dp->dp_meta_objset, ds->ds_object,
	    DS_FIELD_RESUME_TOGUID, sizeof (val), 1, &val);
	if (error != 0 || drrb->drr_toguid != val) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Check if the receive is still running.  If so, it will be owned.
	 * Note that nothing else can own the dataset (e.g. after the receive
	 * fails) because it will be marked inconsistent.
	 */
	if (dsl_dataset_has_owner(ds)) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EBUSY));
	}

	/* There should not be any snapshots of this fs yet. */
	if (ds->ds_prev != NULL && ds->ds_prev->ds_dir == ds->ds_dir) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Note: resume point will be checked when we process the first WRITE
	 * record.
	 */

	/* check that the origin matches */
	val = 0;
	(void) zap_lookup(dp->dp_meta_objset, ds->ds_object,
	    DS_FIELD_RESUME_FROMGUID, sizeof (val), 1, &val);
	if (drrb->drr_fromguid != val) {
		dsl_dataset_rele(ds, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * If we're resuming, and the send is redacted, then the original send
	 * must have been redacted, and must have been redacted with respect to
	 * the same snapshots.
	 */
	if (featureflags & DMU_BACKUP_FEATURE_REDACTED) {
		uint64_t num_ds_redact_snaps;
		uint64_t *ds_redact_snaps;

		uint_t num_stream_redact_snaps;
		uint64_t *stream_redact_snaps;

		if (nvlist_lookup_uint64_array(drc->drc_begin_nvl,
		    BEGINNV_REDACT_SNAPS, &stream_redact_snaps,
		    &num_stream_redact_snaps) != 0) {
			dsl_dataset_rele(ds, FTAG);
			return (SET_ERROR(EINVAL));
		}

		if (!dsl_dataset_get_uint64_array_feature(ds,
		    SPA_FEATURE_REDACTED_DATASETS, &num_ds_redact_snaps,
		    &ds_redact_snaps)) {
			dsl_dataset_rele(ds, FTAG);
			return (SET_ERROR(EINVAL));
		}

		for (int i = 0; i < num_ds_redact_snaps; i++) {
			if (!redact_snaps_contains(ds_redact_snaps,
			    num_ds_redact_snaps, stream_redact_snaps[i])) {
				dsl_dataset_rele(ds, FTAG);
				return (SET_ERROR(EINVAL));
			}
		}
	}

	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static void
dmu_recv_resume_begin_sync(void *arg, dmu_tx_t *tx)
{
	dmu_recv_begin_arg_t *drba = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	const char *tofs = drba->drba_cookie->drc_tofs;
	dsl_dataset_t *ds;
	/* 6 extra bytes for /%recv */
	char recvname[ZFS_MAX_DATASET_NAME_LEN + 6];

	(void) snprintf(recvname, sizeof (recvname), "%s/%s",
	    tofs, recv_clone_name);

	if (dsl_dataset_own_force(dp, recvname, dmu_recv_tag, &ds) != 0) {
		/* %recv does not exist; continue in tofs */
		VERIFY0(dsl_dataset_own_force(dp, tofs, dmu_recv_tag, &ds));
		drba->drba_cookie->drc_newfs = B_TRUE;
	}

	ASSERT(DS_IS_INCONSISTENT(ds));
	rrw_enter(&ds->ds_bp_rwlock, RW_READER, FTAG);
	ASSERT(!BP_IS_HOLE(dsl_dataset_get_blkptr(ds)));
	rrw_exit(&ds->ds_bp_rwlock, FTAG);

	drba->drba_cookie->drc_ds = ds;

	spa_history_log_internal_ds(ds, "resume receive", tx, "");
}

/*
 * NB: callers *MUST* call dmu_recv_stream() if dmu_recv_begin()
 * succeeds; otherwise we will leak the holds on the datasets.
 */
int
dmu_recv_begin(char *tofs, char *tosnap, dmu_replay_record_t *drr_begin,
    boolean_t force, boolean_t resumable, char *origin, dmu_recv_cookie_t *drc,
    vnode_t *vp, offset_t *voffp)
{
	dmu_recv_begin_arg_t drba = { 0 };
	int err;
	bzero(drc, sizeof (dmu_recv_cookie_t));
	drc->drc_drr_begin = drr_begin;
	drc->drc_drrb = &drr_begin->drr_u.drr_begin;
	drc->drc_tosnap = tosnap;
	drc->drc_tofs = tofs;
	drc->drc_force = force;
	drc->drc_resumable = resumable;
	drc->drc_cred = CRED();

	if (drc->drc_drrb->drr_magic == BSWAP_64(DMU_BACKUP_MAGIC)) {
		drc->drc_byteswap = B_TRUE;
		fletcher_4_incremental_byteswap(drr_begin,
		    sizeof (dmu_replay_record_t), &drc->drc_cksum);
		byteswap_record(drr_begin);
	} else if (drc->drc_drrb->drr_magic == DMU_BACKUP_MAGIC) {
		fletcher_4_incremental_native(drr_begin,
		    sizeof (dmu_replay_record_t), &drc->drc_cksum);
	} else {
		return (SET_ERROR(EINVAL));
	}

	drc->drc_vp = vp;
	drc->drc_voff = *voffp;

	uint32_t payloadlen = drc->drc_drr_begin->drr_payloadlen;
	void *payload = NULL;
	if (payloadlen != 0)
		payload = kmem_alloc(payloadlen, KM_SLEEP);

	err = receive_read_payload_and_next_header(drc, payloadlen,
	    payload);
	if (err != 0) {
		kmem_free(payload, payloadlen);
		return (err);
	}
	if (payloadlen != 0) {
		err = nvlist_unpack(payload, payloadlen, &drc->drc_begin_nvl,
		    KM_SLEEP);
		kmem_free(payload, payloadlen);
		if (err != 0) {
			return (err);
		}
	}

	drba.drba_origin = origin;
	drba.drba_cookie = drc;
	drba.drba_cred = CRED();

	if (DMU_GET_FEATUREFLAGS(drc->drc_drrb->drr_versioninfo) &
	    DMU_BACKUP_FEATURE_RESUMING) {
		err = dsl_sync_task(tofs,
		    dmu_recv_resume_begin_check, dmu_recv_resume_begin_sync,
		    &drba, 5, ZFS_SPACE_CHECK_NORMAL);
	} else {
		err = dsl_sync_task(tofs,
		    dmu_recv_begin_check, dmu_recv_begin_sync,
		    &drba, 5, ZFS_SPACE_CHECK_NORMAL);
	}

	if (err != 0) {
		nvlist_free(drc->drc_begin_nvl);
	}
	return (err);
}

static int
guid_compare(const void *arg1, const void *arg2)
{
	const guid_map_entry_t *gmep1 = arg1;
	const guid_map_entry_t *gmep2 = arg2;

	if (gmep1->guid < gmep2->guid)
		return (-1);
	else if (gmep1->guid > gmep2->guid)
		return (1);
	return (0);
}

static void
free_guid_map_onexit(void *arg)
{
	avl_tree_t *ca = arg;
	void *cookie = NULL;
	guid_map_entry_t *gmep;

	while ((gmep = avl_destroy_nodes(ca, &cookie)) != NULL) {
		dsl_dataset_long_rele(gmep->gme_ds, gmep);
		dsl_dataset_rele(gmep->gme_ds, gmep);
		kmem_free(gmep, sizeof (guid_map_entry_t));
	}
	avl_destroy(ca);
	kmem_free(ca, sizeof (avl_tree_t));
}

static int
receive_read(dmu_recv_cookie_t *drc, int len, void *buf)
{
	int done = 0;

	/*
	 * The code doesn't rely on this (lengths being multiples of 8).  See
	 * comment in dump_bytes.
	 */
	ASSERT0(len % 8);

	while (done < len) {
		ssize_t resid;

		drc->drc_err = vn_rdwr(UIO_READ, drc->drc_vp,
		    (char *)buf + done, len - done,
		    drc->drc_voff, UIO_SYSSPACE, FAPPEND,
		    RLIM64_INFINITY, CRED(), &resid);

		if (resid == len - done) {
			/*
			 * Note: ECKSUM indicates that the receive
			 * was interrupted and can potentially be resumed.
			 */
			drc->drc_err = SET_ERROR(ECKSUM);
		}
		drc->drc_voff += len - done - resid;
		done = len - resid;
		if (drc->drc_err != 0)
			return (drc->drc_err);
	}

	drc->drc_bytes_read += len;

	ASSERT3U(done, ==, len);
	return (0);
}

static void
byteswap_record(dmu_replay_record_t *drr)
{
#define	DO64(X) (drr->drr_u.X = BSWAP_64(drr->drr_u.X))
#define	DO32(X) (drr->drr_u.X = BSWAP_32(drr->drr_u.X))
	drr->drr_type = BSWAP_32(drr->drr_type);
	drr->drr_payloadlen = BSWAP_32(drr->drr_payloadlen);

	switch (drr->drr_type) {
	case DRR_BEGIN:
		DO64(drr_begin.drr_magic);
		DO64(drr_begin.drr_versioninfo);
		DO64(drr_begin.drr_creation_time);
		DO32(drr_begin.drr_type);
		DO32(drr_begin.drr_flags);
		DO64(drr_begin.drr_toguid);
		DO64(drr_begin.drr_fromguid);
		break;
	case DRR_OBJECT:
		DO64(drr_object.drr_object);
		DO32(drr_object.drr_type);
		DO32(drr_object.drr_bonustype);
		DO32(drr_object.drr_blksz);
		DO32(drr_object.drr_bonuslen);
		DO64(drr_object.drr_toguid);
		break;
	case DRR_FREEOBJECTS:
		DO64(drr_freeobjects.drr_firstobj);
		DO64(drr_freeobjects.drr_numobjs);
		DO64(drr_freeobjects.drr_toguid);
		break;
	case DRR_WRITE:
		DO64(drr_write.drr_object);
		DO32(drr_write.drr_type);
		DO64(drr_write.drr_offset);
		DO64(drr_write.drr_logical_size);
		DO64(drr_write.drr_toguid);
		ZIO_CHECKSUM_BSWAP(&drr->drr_u.drr_write.drr_key.ddk_cksum);
		DO64(drr_write.drr_key.ddk_prop);
		DO64(drr_write.drr_compressed_size);
		break;
	case DRR_WRITE_BYREF:
		DO64(drr_write_byref.drr_object);
		DO64(drr_write_byref.drr_offset);
		DO64(drr_write_byref.drr_length);
		DO64(drr_write_byref.drr_toguid);
		DO64(drr_write_byref.drr_refguid);
		DO64(drr_write_byref.drr_refobject);
		DO64(drr_write_byref.drr_refoffset);
		ZIO_CHECKSUM_BSWAP(&drr->drr_u.drr_write_byref.
		    drr_key.ddk_cksum);
		DO64(drr_write_byref.drr_key.ddk_prop);
		break;
	case DRR_WRITE_EMBEDDED:
		DO64(drr_write_embedded.drr_object);
		DO64(drr_write_embedded.drr_offset);
		DO64(drr_write_embedded.drr_length);
		DO64(drr_write_embedded.drr_toguid);
		DO32(drr_write_embedded.drr_lsize);
		DO32(drr_write_embedded.drr_psize);
		break;
	case DRR_FREE:
		DO64(drr_free.drr_object);
		DO64(drr_free.drr_offset);
		DO64(drr_free.drr_length);
		DO64(drr_free.drr_toguid);
		break;
	case DRR_SPILL:
		DO64(drr_spill.drr_object);
		DO64(drr_spill.drr_length);
		DO64(drr_spill.drr_toguid);
		break;
	case DRR_END:
		DO64(drr_end.drr_toguid);
		ZIO_CHECKSUM_BSWAP(&drr->drr_u.drr_end.drr_checksum);
		break;
	}

	if (drr->drr_type != DRR_BEGIN) {
		ZIO_CHECKSUM_BSWAP(&drr->drr_u.drr_checksum.drr_checksum);
	}

#undef DO64
#undef DO32
}

static inline uint8_t
deduce_nblkptr(dmu_object_type_t bonus_type, uint64_t bonus_size)
{
	if (bonus_type == DMU_OT_SA) {
		return (1);
	} else {
		return (1 +
		    ((DN_MAX_BONUSLEN - bonus_size) >> SPA_BLKPTRSHIFT));
	}
}

static void
save_resume_state(struct receive_writer_arg *rwa,
    uint64_t object, uint64_t offset, dmu_tx_t *tx)
{
	int txgoff = dmu_tx_get_txg(tx) & TXG_MASK;

	if (!rwa->resumable)
		return;

	/*
	 * We use ds_resume_bytes[] != 0 to indicate that we need to
	 * update this on disk, so it must not be 0.
	 */
	ASSERT(rwa->bytes_read != 0);

	/*
	 * We only resume from write records, which have a valid
	 * (non-meta-dnode) object number.
	 */
	ASSERT(object != 0);

	/*
	 * For resuming to work correctly, we must receive records in order,
	 * sorted by object,offset.  This is checked by the callers, but
	 * assert it here for good measure.
	 */
	ASSERT3U(object, >=, rwa->os->os_dsl_dataset->ds_resume_object[txgoff]);
	ASSERT(object != rwa->os->os_dsl_dataset->ds_resume_object[txgoff] ||
	    offset >= rwa->os->os_dsl_dataset->ds_resume_offset[txgoff]);
	ASSERT3U(rwa->bytes_read, >=,
	    rwa->os->os_dsl_dataset->ds_resume_bytes[txgoff]);

	rwa->os->os_dsl_dataset->ds_resume_object[txgoff] = object;
	rwa->os->os_dsl_dataset->ds_resume_offset[txgoff] = offset;
	rwa->os->os_dsl_dataset->ds_resume_bytes[txgoff] = rwa->bytes_read;
}

static int
receive_object(struct receive_writer_arg *rwa, struct drr_object *drro,
    void *data)
{
	dmu_object_info_t doi;
	dmu_tx_t *tx;
	uint64_t object;
	int err;

	if (drro->drr_type == DMU_OT_NONE ||
	    !DMU_OT_IS_VALID(drro->drr_type) ||
	    !DMU_OT_IS_VALID(drro->drr_bonustype) ||
	    drro->drr_checksumtype >= ZIO_CHECKSUM_FUNCTIONS ||
	    drro->drr_compress >= ZIO_COMPRESS_FUNCTIONS ||
	    P2PHASE(drro->drr_blksz, SPA_MINBLOCKSIZE) ||
	    drro->drr_blksz < SPA_MINBLOCKSIZE ||
	    drro->drr_blksz > spa_maxblocksize(dmu_objset_spa(rwa->os)) ||
	    drro->drr_bonuslen > DN_MAX_BONUSLEN) {
		return (SET_ERROR(EINVAL));
	}

	err = dmu_object_info(rwa->os, drro->drr_object, &doi);

	if (err != 0 && err != ENOENT)
		return (SET_ERROR(EINVAL));
	object = err == 0 ? drro->drr_object : DMU_NEW_OBJECT;

	/*
	 * If we are losing blkptrs or changing the block size this must
	 * be a new file instance.  We must clear out the previous file
	 * contents before we can change this type of metadata in the dnode.
	 */
	if (err == 0) {
		int nblkptr;

		nblkptr = deduce_nblkptr(drro->drr_bonustype,
		    drro->drr_bonuslen);

		if (drro->drr_blksz != doi.doi_data_block_size ||
		    nblkptr < doi.doi_nblkptr) {
			err = dmu_free_long_range(rwa->os, drro->drr_object,
			    0, DMU_OBJECT_END);
			if (err != 0)
				return (SET_ERROR(EINVAL));
		}
	}

	tx = dmu_tx_create(rwa->os);
	dmu_tx_hold_bonus(tx, object);
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}

	if (object == DMU_NEW_OBJECT) {
		/* currently free, want to be allocated */
		err = dmu_object_claim(rwa->os, drro->drr_object,
		    drro->drr_type, drro->drr_blksz,
		    drro->drr_bonustype, drro->drr_bonuslen, tx);
	} else if (drro->drr_type != doi.doi_type ||
	    drro->drr_blksz != doi.doi_data_block_size ||
	    drro->drr_bonustype != doi.doi_bonus_type ||
	    drro->drr_bonuslen != doi.doi_bonus_size) {
		/* currently allocated, but with different properties */
		err = dmu_object_reclaim(rwa->os, drro->drr_object,
		    drro->drr_type, drro->drr_blksz,
		    drro->drr_bonustype, drro->drr_bonuslen, tx);
	}
	if (err != 0) {
		dmu_tx_commit(tx);
		return (SET_ERROR(EINVAL));
	}

	dmu_object_set_checksum(rwa->os, drro->drr_object,
	    drro->drr_checksumtype, tx);
	dmu_object_set_compress(rwa->os, drro->drr_object,
	    drro->drr_compress, tx);

	if (data != NULL) {
		dmu_buf_t *db;

		VERIFY0(dmu_bonus_hold(rwa->os, drro->drr_object, FTAG, &db));
		dmu_buf_will_dirty(db, tx);

		ASSERT3U(db->db_size, >=, drro->drr_bonuslen);
		bcopy(data, db->db_data, drro->drr_bonuslen);
		if (rwa->byteswap) {
			dmu_object_byteswap_t byteswap =
			    DMU_OT_BYTESWAP(drro->drr_bonustype);
			dmu_ot_byteswap[byteswap].ob_func(db->db_data,
			    drro->drr_bonuslen);
		}
		dmu_buf_rele(db, FTAG);
	}
	dmu_tx_commit(tx);

	return (0);
}

/* ARGSUSED */
static int
receive_freeobjects(struct receive_writer_arg *rwa,
    struct drr_freeobjects *drrfo)
{
	uint64_t obj;
	int next_err = 0;

	if (drrfo->drr_firstobj + drrfo->drr_numobjs < drrfo->drr_firstobj)
		return (SET_ERROR(EINVAL));

	for (obj = drrfo->drr_firstobj;
	    obj < drrfo->drr_firstobj + drrfo->drr_numobjs && next_err == 0;
	    next_err = dmu_object_next(rwa->os, &obj, FALSE, 0)) {
		int err;

		if (dmu_object_info(rwa->os, obj, NULL) != 0)
			continue;

		err = dmu_free_long_object(rwa->os, obj);
		if (err != 0)
			return (err);
	}
	if (next_err != ESRCH)
		return (next_err);
	return (0);
}

static int
receive_write(struct receive_writer_arg *rwa, struct drr_write *drrw,
    arc_buf_t *abuf)
{
	dmu_tx_t *tx;
	int err;

	if (drrw->drr_offset + drrw->drr_logical_size < drrw->drr_offset ||
	    !DMU_OT_IS_VALID(drrw->drr_type))
		return (SET_ERROR(EINVAL));

	/*
	 * For resuming to work, records must be in increasing order
	 * by (object, offset).
	 */
	if (drrw->drr_object < rwa->last_object ||
	    (drrw->drr_object == rwa->last_object &&
	    drrw->drr_offset < rwa->last_offset)) {
		return (SET_ERROR(EINVAL));
	}
	rwa->last_object = drrw->drr_object;
	rwa->last_offset = drrw->drr_offset;

	if (dmu_object_info(rwa->os, drrw->drr_object, NULL) != 0)
		return (SET_ERROR(EINVAL));

	tx = dmu_tx_create(rwa->os);

	dmu_tx_hold_write(tx, drrw->drr_object,
	    drrw->drr_offset, drrw->drr_logical_size);
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}
	if (rwa->byteswap) {
		dmu_object_byteswap_t byteswap =
		    DMU_OT_BYTESWAP(drrw->drr_type);
		dmu_ot_byteswap[byteswap].ob_func(abuf->b_data,
		    DRR_WRITE_PAYLOAD_SIZE(drrw));
	}

	/* use the bonus buf to look up the dnode in dmu_assign_arcbuf */
	dmu_buf_t *bonus;
	if (dmu_bonus_hold(rwa->os, drrw->drr_object, FTAG, &bonus) != 0)
		return (SET_ERROR(EINVAL));
	dmu_assign_arcbuf(bonus, drrw->drr_offset, abuf, tx);

	/*
	 * Note: If the receive fails, we want the resume stream to start
	 * with the same record that we last successfully received (as opposed
	 * to the next record), so that we can verify that we are
	 * resuming from the correct location.
	 */
	save_resume_state(rwa, drrw->drr_object, drrw->drr_offset, tx);
	dmu_tx_commit(tx);
	dmu_buf_rele(bonus, FTAG);

	return (0);
}

/*
 * Handle a DRR_WRITE_BYREF record.  This record is used in dedup'ed
 * streams to refer to a copy of the data that is already on the
 * system because it came in earlier in the stream.  This function
 * finds the earlier copy of the data, and uses that copy instead of
 * data from the stream to fulfill this write.
 */
static int
receive_write_byref(struct receive_writer_arg *rwa,
    struct drr_write_byref *drrwbr)
{
	dmu_tx_t *tx;
	int err;
	guid_map_entry_t gmesrch;
	guid_map_entry_t *gmep;
	avl_index_t where;
	objset_t *ref_os = NULL;
	dmu_buf_t *dbp;

	if (drrwbr->drr_offset + drrwbr->drr_length < drrwbr->drr_offset)
		return (SET_ERROR(EINVAL));

	/*
	 * If the GUID of the referenced dataset is different from the
	 * GUID of the target dataset, find the referenced dataset.
	 */
	if (drrwbr->drr_toguid != drrwbr->drr_refguid) {
		gmesrch.guid = drrwbr->drr_refguid;
		if ((gmep = avl_find(rwa->guid_to_ds_map, &gmesrch,
		    &where)) == NULL) {
			return (SET_ERROR(EINVAL));
		}
		if (dmu_objset_from_ds(gmep->gme_ds, &ref_os))
			return (SET_ERROR(EINVAL));
	} else {
		ref_os = rwa->os;
	}

	err = dmu_buf_hold(ref_os, drrwbr->drr_refobject,
	    drrwbr->drr_refoffset, FTAG, &dbp, DMU_READ_PREFETCH);
	if (err != 0)
		return (err);

	tx = dmu_tx_create(rwa->os);

	dmu_tx_hold_write(tx, drrwbr->drr_object,
	    drrwbr->drr_offset, drrwbr->drr_length);
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}
	dmu_write(rwa->os, drrwbr->drr_object,
	    drrwbr->drr_offset, drrwbr->drr_length, dbp->db_data, tx);
	dmu_buf_rele(dbp, FTAG);

	/* See comment in restore_write. */
	save_resume_state(rwa, drrwbr->drr_object, drrwbr->drr_offset, tx);
	dmu_tx_commit(tx);
	return (0);
}

static int
receive_write_embedded(struct receive_writer_arg *rwa,
    struct drr_write_embedded *drrwe, void *data)
{
	dmu_tx_t *tx;
	int err;

	if (drrwe->drr_offset + drrwe->drr_length < drrwe->drr_offset)
		return (EINVAL);

	if (drrwe->drr_psize > BPE_PAYLOAD_SIZE)
		return (EINVAL);

	if (drrwe->drr_etype >= NUM_BP_EMBEDDED_TYPES)
		return (EINVAL);
	if (drrwe->drr_compression >= ZIO_COMPRESS_FUNCTIONS)
		return (EINVAL);

	tx = dmu_tx_create(rwa->os);

	dmu_tx_hold_write(tx, drrwe->drr_object,
	    drrwe->drr_offset, drrwe->drr_length);
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		return (err);
	}

	dmu_write_embedded(rwa->os, drrwe->drr_object,
	    drrwe->drr_offset, data, drrwe->drr_etype,
	    drrwe->drr_compression, drrwe->drr_lsize, drrwe->drr_psize,
	    rwa->byteswap ^ ZFS_HOST_BYTEORDER, tx);

	/* See comment in restore_write. */
	save_resume_state(rwa, drrwe->drr_object, drrwe->drr_offset, tx);
	dmu_tx_commit(tx);
	return (0);
}

static int
receive_spill(struct receive_writer_arg *rwa, struct drr_spill *drrs,
    void *data)
{
	dmu_tx_t *tx;
	dmu_buf_t *db, *db_spill;
	int err;

	if (drrs->drr_length < SPA_MINBLOCKSIZE ||
	    drrs->drr_length > spa_maxblocksize(dmu_objset_spa(rwa->os)))
		return (SET_ERROR(EINVAL));

	if (dmu_object_info(rwa->os, drrs->drr_object, NULL) != 0)
		return (SET_ERROR(EINVAL));

	VERIFY0(dmu_bonus_hold(rwa->os, drrs->drr_object, FTAG, &db));
	if ((err = dmu_spill_hold_by_bonus(db, FTAG, &db_spill)) != 0) {
		dmu_buf_rele(db, FTAG);
		return (err);
	}

	tx = dmu_tx_create(rwa->os);

	dmu_tx_hold_spill(tx, db->db_object);

	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err != 0) {
		dmu_buf_rele(db, FTAG);
		dmu_buf_rele(db_spill, FTAG);
		dmu_tx_abort(tx);
		return (err);
	}
	dmu_buf_will_dirty(db_spill, tx);

	if (db_spill->db_size < drrs->drr_length)
		VERIFY(0 == dbuf_spill_set_blksz(db_spill,
		    drrs->drr_length, tx));
	bcopy(data, db_spill->db_data, drrs->drr_length);

	dmu_buf_rele(db, FTAG);
	dmu_buf_rele(db_spill, FTAG);

	dmu_tx_commit(tx);
	return (0);
}

/* ARGSUSED */
static int
receive_free(struct receive_writer_arg *rwa, struct drr_free *drrf)
{
	int err;

	if (drrf->drr_length != -1ULL &&
	    drrf->drr_offset + drrf->drr_length < drrf->drr_offset)
		return (SET_ERROR(EINVAL));

	if (dmu_object_info(rwa->os, drrf->drr_object, NULL) != 0)
		return (SET_ERROR(EINVAL));

	err = dmu_free_long_range(rwa->os, drrf->drr_object,
	    drrf->drr_offset, drrf->drr_length);

	return (err);
}

/* ARGSUSED */
static int
receive_redact(struct receive_writer_arg *rwa, struct drr_redact *drrr)
{
	int err;

	/*
	 * If the object doesn't exist, there's nothing to redact.
	 */
	if (dmu_object_info(rwa->os, drrr->drr_object, NULL) != 0)
		return (SET_ERROR(EINVAL));

	uint64_t offset = drrr->drr_offset;
	uint64_t length = drrr->drr_length;
	while (length > 0) {
		uint64_t this_length = MIN(length, DMU_MAX_ACCESS);

		dmu_tx_t *tx = dmu_tx_create(rwa->os);

		dmu_tx_hold_write(tx, drrr->drr_object, offset, this_length);
		err = dmu_tx_assign(tx, TXG_WAIT);
		if (err != 0) {
			dmu_tx_abort(tx);
			return (err);
		}

		dmu_redact(rwa->os, drrr->drr_object, offset, this_length, tx);
		dmu_tx_commit(tx);

		offset += this_length;
		length -= this_length;
	}

	return (0);
}

/* used to destroy the drc_ds on error */
static void
dmu_recv_cleanup_ds(dmu_recv_cookie_t *drc)
{
	if (drc->drc_resumable) {
		/* wait for our resume state to be written to disk */
		txg_wait_synced(drc->drc_ds->ds_dir->dd_pool, 0);
		dsl_dataset_disown(drc->drc_ds, dmu_recv_tag);
	} else {
		char name[ZFS_MAX_DATASET_NAME_LEN];
		dsl_dataset_name(drc->drc_ds, name);
		dsl_dataset_disown(drc->drc_ds, dmu_recv_tag);
		(void) dsl_destroy_head(name);
	}
}

static void
receive_cksum(dmu_recv_cookie_t *drc, int len, void *buf)
{
	if (drc->drc_byteswap) {
		fletcher_4_incremental_byteswap(buf, len, &drc->drc_cksum);
	} else {
		fletcher_4_incremental_native(buf, len, &drc->drc_cksum);
	}
}

/*
 * Read the payload into a buffer of size len, and update the current record's
 * payload field.
 * Allocate drc->drc_next_rrd and read the next record's header into
 * drc->drc_next_rrd->header.
 * Verify checksum of payload and next record.
 */
static int
receive_read_payload_and_next_header(dmu_recv_cookie_t *drc, int len, void *buf)
{
	int err;

	if (len != 0) {
		ASSERT3U(len, <=, SPA_MAXBLOCKSIZE);
		err = receive_read(drc, len, buf);
		if (err != 0)
			return (err);
		receive_cksum(drc, len, buf);

		/* note: rrd is NULL when reading the begin record's payload */
		if (drc->drc_rrd != NULL) {
			drc->drc_rrd->payload = buf;
			drc->drc_rrd->payload_size = len;
			drc->drc_rrd->bytes_read = drc->drc_bytes_read;
		}
	}

	drc->drc_prev_cksum = drc->drc_cksum;

	drc->drc_next_rrd = kmem_zalloc(sizeof (*drc->drc_next_rrd), KM_SLEEP);
	err = receive_read(drc, sizeof (drc->drc_next_rrd->header),
	    &drc->drc_next_rrd->header);
	drc->drc_next_rrd->bytes_read = drc->drc_bytes_read;
	if (err != 0) {
		kmem_free(drc->drc_next_rrd, sizeof (*drc->drc_next_rrd));
		drc->drc_next_rrd = NULL;
		return (err);
	}
	if (drc->drc_next_rrd->header.drr_type == DRR_BEGIN) {
		kmem_free(drc->drc_next_rrd, sizeof (*drc->drc_next_rrd));
		drc->drc_next_rrd = NULL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Note: checksum is of everything up to but not including the
	 * checksum itself.
	 */
	ASSERT3U(offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    ==, sizeof (dmu_replay_record_t) - sizeof (zio_cksum_t));
	receive_cksum(drc,
	    offsetof(dmu_replay_record_t, drr_u.drr_checksum.drr_checksum),
	    &drc->drc_next_rrd->header);

	zio_cksum_t cksum_orig =
	    drc->drc_next_rrd->header.drr_u.drr_checksum.drr_checksum;
	zio_cksum_t *cksump =
	    &drc->drc_next_rrd->header.drr_u.drr_checksum.drr_checksum;

	if (drc->drc_byteswap)
		byteswap_record(&drc->drc_next_rrd->header);

	if ((!ZIO_CHECKSUM_IS_ZERO(cksump)) &&
	    !ZIO_CHECKSUM_EQUAL(drc->drc_cksum, *cksump)) {
		kmem_free(drc->drc_next_rrd, sizeof (*drc->drc_next_rrd));
		drc->drc_next_rrd = NULL;
		return (SET_ERROR(ECKSUM));
	}

	receive_cksum(drc, sizeof (cksum_orig), &cksum_orig);

	return (0);
}

static struct objlist *
objlist_create(void)
{
	struct objlist *list = kmem_alloc(sizeof (*list), KM_SLEEP);
	list_create(&list->list, sizeof (struct receive_objnode),
	    offsetof(struct receive_objnode, node));
	list->last_lookup = 0;
	return (list);
}

static void
objlist_destroy(struct objlist *list)
{
	for (struct receive_objnode *n = list_remove_head(&list->list);
	    n != NULL; n = list_remove_head(&list->list)) {
		kmem_free(n, sizeof (*n));
	}
	list_destroy(&list->list);
	kmem_free(list, sizeof (*list));
}

/*
 * This function looks through the objlist to see if the specified object number
 * is contained in the objlist.  In the process, it will remove all object
 * numbers in the list that are smaller than the specified object number.  Thus,
 * any lookup of an object number smaller than a previously looked up object
 * number will always return false; therefore, all lookups should be done in
 * ascending order.
 */
static boolean_t
objlist_exists(struct objlist *list, uint64_t object)
{
	struct receive_objnode *node = list_head(&list->list);
	ASSERT3U(object, >=, list->last_lookup);
	list->last_lookup = object;
	while (node != NULL && node->object < object) {
		VERIFY3P(node, ==, list_remove_head(&list->list));
		kmem_free(node, sizeof (*node));
		node = list_head(&list->list);
	}
	return (node != NULL && node->object == object);
}

/*
 * The objlist is a list of object numbers stored in ascending order.  However,
 * the insertion of new object numbers does not seek out the correct location to
 * store a new object number; instead, it appends it to the list for simplicity.
 * Thus, any users must take care to only insert new object numbers in ascending
 * order.
 */
static void
objlist_insert(struct objlist *list, uint64_t object)
{
	struct receive_objnode *node = kmem_zalloc(sizeof (*node), KM_SLEEP);
	node->object = object;
#ifdef ZFS_DEBUG
	struct receive_objnode *last_object = list_tail(&list->list);
	uint64_t last_objnum = (last_object != NULL ? last_object->object : 0);
	ASSERT3U(node->object, >, last_objnum);
#endif
	list_insert_tail(&list->list, node);
}

/*
 * Issue the prefetch reads for any necessary indirect blocks.
 *
 * We use the object ignore list to tell us whether or not to issue prefetches
 * for a given object.  We do this for both correctness (in case the blocksize
 * of an object has changed) and performance (if the object doesn't exist, don't
 * needlessly try to issue prefetches).  We also trim the list as we go through
 * the stream to prevent it from growing to an unbounded size.
 *
 * The object numbers within will always be in sorted order, and any write
 * records we see will also be in sorted order, but they're not sorted with
 * respect to each other (i.e. we can get several object records before
 * receiving each object's write records).  As a result, once we've reached a
 * given object number, we can safely remove any reference to lower object
 * numbers in the ignore list.  In practice, we receive up to 32 object records
 * before receiving write records, so the list can have up to 32 nodes in it.
 */
/* ARGSUSED */
static void
receive_read_prefetch(dmu_recv_cookie_t *drc, uint64_t object, uint64_t offset,
    uint64_t length)
{
	if (!objlist_exists(drc->drc_ignore_objlist, object)) {
		dmu_prefetch(drc->drc_os, object, 1, offset, length,
		    ZIO_PRIORITY_SYNC_READ);
	}
}

/*
 * Read records off the stream, issuing any necessary prefetches.
 */
static int
receive_read_record(dmu_recv_cookie_t *drc)
{
	int err;

	switch (drc->drc_rrd->header.drr_type) {
	case DRR_OBJECT:
	{
		struct drr_object *drro =
		    &drc->drc_rrd->header.drr_u.drr_object;
		uint32_t size = P2ROUNDUP(drro->drr_bonuslen, 8);
		void *buf = kmem_zalloc(size, KM_SLEEP);
		dmu_object_info_t doi;
		err = receive_read_payload_and_next_header(drc, size, buf);
		if (err != 0) {
			kmem_free(buf, size);
			return (err);
		}
		err = dmu_object_info(drc->drc_os, drro->drr_object, &doi);
		/*
		 * See receive_read_prefetch for an explanation why we're
		 * storing this object in the ignore_obj_list.
		 */
		if (err == ENOENT ||
		    (err == 0 && doi.doi_data_block_size != drro->drr_blksz)) {
			objlist_insert(drc->drc_ignore_objlist,
			    drro->drr_object);
			err = 0;
		}
		return (err);
	}
	case DRR_FREEOBJECTS:
	{
		err = receive_read_payload_and_next_header(drc, 0, NULL);
		return (err);
	}
	case DRR_WRITE:
	{
		struct drr_write *drrw = &drc->drc_rrd->header.drr_u.drr_write;
		arc_buf_t *abuf;
		boolean_t is_meta = DMU_OT_IS_METADATA(drrw->drr_type);
		if (DRR_WRITE_COMPRESSED(drrw)) {
			ASSERT3U(drrw->drr_compressed_size, >, 0);
			ASSERT3U(drrw->drr_logical_size, >=,
			    drrw->drr_compressed_size);
			ASSERT(!is_meta);
			abuf = arc_loan_compressed_buf(
			    dmu_objset_spa(drc->drc_os),
			    drrw->drr_compressed_size, drrw->drr_logical_size,
			    drrw->drr_compressiontype);
		} else {
			abuf = arc_loan_buf(dmu_objset_spa(drc->drc_os),
			    is_meta, drrw->drr_logical_size);
		}

		err = receive_read_payload_and_next_header(drc,
		    DRR_WRITE_PAYLOAD_SIZE(drrw), abuf->b_data);
		if (err != 0) {
			dmu_return_arcbuf(abuf);
			return (err);
		}
		drc->drc_rrd->write_buf = abuf;
		receive_read_prefetch(drc, drrw->drr_object, drrw->drr_offset,
		    drrw->drr_logical_size);
		return (err);
	}
	case DRR_WRITE_BYREF:
	{
		struct drr_write_byref *drrwb =
		    &drc->drc_rrd->header.drr_u.drr_write_byref;
		err = receive_read_payload_and_next_header(drc, 0, NULL);
		receive_read_prefetch(drc, drrwb->drr_object, drrwb->drr_offset,
		    drrwb->drr_length);
		return (err);
	}
	case DRR_WRITE_EMBEDDED:
	{
		struct drr_write_embedded *drrwe =
		    &drc->drc_rrd->header.drr_u.drr_write_embedded;
		uint32_t size = P2ROUNDUP(drrwe->drr_psize, 8);
		void *buf = kmem_zalloc(size, KM_SLEEP);

		err = receive_read_payload_and_next_header(drc, size, buf);
		if (err != 0) {
			kmem_free(buf, size);
			return (err);
		}

		receive_read_prefetch(drc, drrwe->drr_object, drrwe->drr_offset,
		    drrwe->drr_length);
		return (err);
	}
	case DRR_FREE:
	case DRR_REDACT:
	{
		/*
		 * It might be beneficial to prefetch indirect blocks here, but
		 * we don't really have the data to decide for sure.
		 */
		err = receive_read_payload_and_next_header(drc, 0, NULL);
		return (err);
	}
	case DRR_END:
	{
		struct drr_end *drre = &drc->drc_rrd->header.drr_u.drr_end;
		if (!ZIO_CHECKSUM_EQUAL(drc->drc_prev_cksum,
		    drre->drr_checksum))
			return (SET_ERROR(ECKSUM));
		return (0);
	}
	case DRR_SPILL:
	{
		struct drr_spill *drrs = &drc->drc_rrd->header.drr_u.drr_spill;
		void *buf = kmem_zalloc(drrs->drr_length, KM_SLEEP);
		err = receive_read_payload_and_next_header(drc,
		    drrs->drr_length, buf);
		if (err != 0)
			kmem_free(buf, drrs->drr_length);
		return (err);
	}
	default:
		return (SET_ERROR(EINVAL));
	}
}

/*
 * Commit the records to the pool.
 */
static int
receive_process_record(struct receive_writer_arg *rwa,
    struct receive_record_arg *rrd)
{
	int err;

	/* Processing in order, therefore bytes_read should be increasing. */
	ASSERT3U(rrd->bytes_read, >=, rwa->bytes_read);
	rwa->bytes_read = rrd->bytes_read;

	switch (rrd->header.drr_type) {
	case DRR_OBJECT:
	{
		struct drr_object *drro = &rrd->header.drr_u.drr_object;
		err = receive_object(rwa, drro, rrd->payload);
		kmem_free(rrd->payload, rrd->payload_size);
		rrd->payload = NULL;
		return (err);
	}
	case DRR_FREEOBJECTS:
	{
		struct drr_freeobjects *drrfo =
		    &rrd->header.drr_u.drr_freeobjects;
		return (receive_freeobjects(rwa, drrfo));
	}
	case DRR_WRITE:
	{
		struct drr_write *drrw = &rrd->header.drr_u.drr_write;
		err = receive_write(rwa, drrw, rrd->write_buf);
		/* if receive_write() is successful, it consumes the arc_buf */
		if (err != 0)
			dmu_return_arcbuf(rrd->write_buf);
		rrd->write_buf = NULL;
		rrd->payload = NULL;
		return (err);
	}
	case DRR_WRITE_BYREF:
	{
		struct drr_write_byref *drrwbr =
		    &rrd->header.drr_u.drr_write_byref;
		return (receive_write_byref(rwa, drrwbr));
	}
	case DRR_WRITE_EMBEDDED:
	{
		struct drr_write_embedded *drrwe =
		    &rrd->header.drr_u.drr_write_embedded;
		err = receive_write_embedded(rwa, drrwe, rrd->payload);
		kmem_free(rrd->payload, rrd->payload_size);
		rrd->payload = NULL;
		return (err);
	}
	case DRR_FREE:
	{
		struct drr_free *drrf = &rrd->header.drr_u.drr_free;
		return (receive_free(rwa, drrf));
	}
	case DRR_SPILL:
	{
		struct drr_spill *drrs = &rrd->header.drr_u.drr_spill;
		err = receive_spill(rwa, drrs, rrd->payload);
		kmem_free(rrd->payload, rrd->payload_size);
		rrd->payload = NULL;
		return (err);
	}
	case DRR_REDACT:
	{
		struct drr_redact *drrr = &rrd->header.drr_u.drr_redact;
		return (receive_redact(rwa, drrr));
	}
	default:
		return (SET_ERROR(EINVAL));
	}
}

/*
 * dmu_recv_stream's worker thread; pull records off the queue, and then call
 * receive_process_record  When we're done, signal the main thread and exit.
 */
static void
receive_writer_thread(void *arg)
{
	struct receive_writer_arg *rwa = arg;
	struct receive_record_arg *rrd;
	for (rrd = bqueue_dequeue(&rwa->q); !rrd->eos_marker;
	    rrd = bqueue_dequeue(&rwa->q)) {
		/*
		 * If there's an error, the main thread will stop putting things
		 * on the queue, but we need to clear everything in it before we
		 * can exit.
		 */
		if (rwa->err == 0) {
			rwa->err = receive_process_record(rwa, rrd);
		} else if (rrd->write_buf != NULL) {
			dmu_return_arcbuf(rrd->write_buf);
			rrd->write_buf = NULL;
			rrd->payload = NULL;
		} else if (rrd->payload != NULL) {
			kmem_free(rrd->payload, rrd->payload_size);
			rrd->payload = NULL;
		}
		kmem_free(rrd, sizeof (*rrd));
	}
	kmem_free(rrd, sizeof (*rrd));
	mutex_enter(&rwa->mutex);
	rwa->done = B_TRUE;
	cv_signal(&rwa->cv);
	mutex_exit(&rwa->mutex);
}

/*
 * Read in the stream's records, one by one, and apply them to the pool.  There
 * are two threads involved; the thread that calls this function will spin up a
 * worker thread, read the records off the stream one by one, and issue
 * prefetches for any necessary indirect blocks.  It will then push the records
 * onto an internal blocking queue.  The worker thread will pull the records off
 * the queue, and actually write the data into the DMU.  This way, the worker
 * thread doesn't have to wait for reads to complete, since everything it needs
 * (the indirect blocks) will be prefetched.
 *
 * NB: callers *must* call dmu_recv_end() if this succeeds.
 */
int
dmu_recv_stream(dmu_recv_cookie_t *drc, int cleanup_fd,
    uint64_t *action_handlep, offset_t *voffp)
{
	int err = 0;
	struct receive_writer_arg rwa = { 0 };
	int featureflags;

	if (dsl_dataset_is_zapified(drc->drc_ds)) {
		uint64_t bytes;
		(void) zap_lookup(drc->drc_ds->ds_dir->dd_pool->dp_meta_objset,
		    drc->drc_ds->ds_object, DS_FIELD_RESUME_BYTES,
		    sizeof (bytes), 1, &bytes);
		drc->drc_bytes_read += bytes;
	}

	drc->drc_ignore_objlist = objlist_create();

	/* these were verified in dmu_recv_begin */
	ASSERT3U(DMU_GET_STREAM_HDRTYPE(drc->drc_drrb->drr_versioninfo), ==,
	    DMU_SUBSTREAM);
	ASSERT3U(drc->drc_drrb->drr_type, <, DMU_OST_NUMTYPES);

	/*
	 * Open the objset we are modifying.
	 */
	VERIFY0(dmu_objset_from_ds(drc->drc_ds, &drc->drc_os));

	ASSERT(dsl_dataset_phys(drc->drc_ds)->ds_flags & DS_FLAG_INCONSISTENT);

	featureflags = DMU_GET_FEATUREFLAGS(drc->drc_drrb->drr_versioninfo);

	/* if this stream is dedup'ed, set up the avl tree for guid mapping */
	if (featureflags & DMU_BACKUP_FEATURE_DEDUP) {
		minor_t minor;

		if (cleanup_fd == -1) {
			drc->drc_err = SET_ERROR(EBADF);
			goto out;
		}
		drc->drc_err = zfs_onexit_fd_hold(cleanup_fd, &minor);
		if (drc->drc_err != 0) {
			cleanup_fd = -1;
			goto out;
		}

		if (*action_handlep == 0) {
			rwa.guid_to_ds_map =
			    kmem_alloc(sizeof (avl_tree_t), KM_SLEEP);
			avl_create(rwa.guid_to_ds_map, guid_compare,
			    sizeof (guid_map_entry_t),
			    offsetof(guid_map_entry_t, avlnode));
			err = zfs_onexit_add_cb(minor,
			    free_guid_map_onexit, rwa.guid_to_ds_map,
			    action_handlep);
			if (drc->drc_err != 0)
				goto out;
		} else {
			err = zfs_onexit_cb_data(minor, *action_handlep,
			    (void **)&rwa.guid_to_ds_map);
			if (drc->drc_err != 0)
				goto out;
		}

		drc->drc_guid_to_ds_map = rwa.guid_to_ds_map;
	}

	if (featureflags & DMU_BACKUP_FEATURE_RESUMING) {
		err = resume_check(drc, drc->drc_begin_nvl);
		if (err != 0) {
			goto out;
		}
	}

	(void) bqueue_init(&rwa.q, zfs_recv_queue_ff,
	    zfs_recv_queue_length, offsetof(struct receive_record_arg, node));
	cv_init(&rwa.cv, NULL, CV_DEFAULT, NULL);
	mutex_init(&rwa.mutex, NULL, MUTEX_DEFAULT, NULL);
	rwa.os = drc->drc_os;
	rwa.byteswap = drc->drc_byteswap;
	rwa.resumable = drc->drc_resumable;

	(void) thread_create(NULL, 0, receive_writer_thread, &rwa, 0, curproc,
	    TS_RUN, minclsyspri);
	/*
	 * We're reading rwa.err without locks, which is safe since we are the
	 * only reader, and the worker thread is the only writer.  It's ok if we
	 * miss a write for an iteration or two of the loop, since the writer
	 * thread will keep freeing records we send it until we send it an eos
	 * marker.
	 *
	 * We can leave this loop in 3 ways:  First, if rwa.err is
	 * non-zero.  In that case, the writer thread will free the rrd we just
	 * pushed.  Second, if  we're interrupted; in that case, either it's the
	 * first loop and ra.rrd was never allocated, or it's later, and ra.rrd
	 * has been handed off to the writer thread who will free it.  Finally,
	 * if receive_read_record fails or we're at the end of the stream, then
	 * we free ra.rrd and exit.
	 */
	while (rwa.err == 0) {
		if (issig(JUSTLOOKING) && issig(FORREAL)) {
			err = SET_ERROR(EINTR);
			break;
		}

		ASSERT3P(drc->drc_rrd, ==, NULL);
		drc->drc_rrd = drc->drc_next_rrd;
		drc->drc_next_rrd = NULL;
		/* Allocates and loads header into ra.next_rrd */
		err = receive_read_record(drc);

		if (drc->drc_rrd->header.drr_type == DRR_END || err != 0) {
			kmem_free(drc->drc_rrd, sizeof (*drc->drc_rrd));
			drc->drc_rrd = NULL;
			break;
		}

		bqueue_enqueue(&rwa.q, drc->drc_rrd,
		    sizeof (struct receive_record_arg) +
		    drc->drc_rrd->payload_size);
		drc->drc_rrd = NULL;
	}
	if (drc->drc_next_rrd == NULL) {
		drc->drc_next_rrd = kmem_zalloc(sizeof (*drc->drc_next_rrd),
		    KM_SLEEP);
	}
	drc->drc_next_rrd->eos_marker = B_TRUE;
	bqueue_enqueue(&rwa.q, drc->drc_next_rrd, 1);
	bqueue_flush(&rwa.q);

	mutex_enter(&rwa.mutex);
	while (!rwa.done) {
		cv_wait(&rwa.cv, &rwa.mutex);
	}
	mutex_exit(&rwa.mutex);

	cv_destroy(&rwa.cv);
	mutex_destroy(&rwa.mutex);
	bqueue_destroy(&rwa.q);
	if (err == 0)
		err = rwa.err;

out:
	nvlist_free(drc->drc_begin_nvl);
	if ((featureflags & DMU_BACKUP_FEATURE_DEDUP) && (cleanup_fd != -1))
		zfs_onexit_fd_rele(cleanup_fd);

	if (err != 0) {
		/*
		 * Clean up references. If receive is not resumable,
		 * destroy what we created, so we don't leave it in
		 * the inconsistent state.
		 */
		dmu_recv_cleanup_ds(drc);
	}

	objlist_destroy(drc->drc_ignore_objlist);
	drc->drc_ignore_objlist = NULL;
	*voffp = drc->drc_voff;
	return (err);
}

static int
dmu_recv_end_check(void *arg, dmu_tx_t *tx)
{
	dmu_recv_cookie_t *drc = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	int error;

	ASSERT3P(drc->drc_ds->ds_owner, ==, dmu_recv_tag);

	if (!drc->drc_newfs) {
		dsl_dataset_t *origin_head;

		error = dsl_dataset_hold(dp, drc->drc_tofs, FTAG, &origin_head);
		if (error != 0)
			return (error);
		if (drc->drc_force) {
			/*
			 * We will destroy any snapshots in tofs (i.e. before
			 * origin_head) that are after the origin (which is
			 * the snap before drc_ds, because drc_ds can not
			 * have any snaps of its own).
			 */
			uint64_t obj;

			obj = dsl_dataset_phys(origin_head)->ds_prev_snap_obj;
			while (obj !=
			    dsl_dataset_phys(drc->drc_ds)->ds_prev_snap_obj) {
				dsl_dataset_t *snap;
				error = dsl_dataset_hold_obj(dp, obj, FTAG,
				    &snap);
				if (error != 0)
					return (error);
				if (snap->ds_dir != origin_head->ds_dir)
					error = SET_ERROR(EINVAL);
				if (error == 0)  {
					error = dsl_destroy_snapshot_check_impl(
					    snap, B_FALSE);
				}
				obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
				dsl_dataset_rele(snap, FTAG);
				if (error != 0)
					return (error);
			}
		}
		error = dsl_dataset_clone_swap_check_impl(drc->drc_ds,
		    origin_head, drc->drc_force, drc->drc_owner, tx);
		if (error != 0) {
			dsl_dataset_rele(origin_head, FTAG);
			return (error);
		}
		error = dsl_dataset_snapshot_check_impl(origin_head,
		    drc->drc_tosnap, tx, B_TRUE, 1, drc->drc_cred);
		dsl_dataset_rele(origin_head, FTAG);
		if (error != 0)
			return (error);

		error = dsl_destroy_head_check_impl(drc->drc_ds, 1);
	} else {
		error = dsl_dataset_snapshot_check_impl(drc->drc_ds,
		    drc->drc_tosnap, tx, B_TRUE, 1, drc->drc_cred);
	}
	return (error);
}

static void
dmu_recv_end_sync(void *arg, dmu_tx_t *tx)
{
	dmu_recv_cookie_t *drc = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);

	spa_history_log_internal_ds(drc->drc_ds, "finish receiving",
	    tx, "snap=%s", drc->drc_tosnap);

	/*
	 * We must evict the objset, because it may have invalid
	 * dn_origin_obj_refd (see dmu_objset_mooch_obj_refd()).
	 */
	if (drc->drc_ds->ds_objset != NULL) {
		dmu_objset_evict(drc->drc_ds->ds_objset);
		drc->drc_ds->ds_objset = NULL;
	}

	if (!drc->drc_newfs) {
		dsl_dataset_t *origin_head;

		VERIFY0(dsl_dataset_hold(dp, drc->drc_tofs, FTAG,
		    &origin_head));

		if (drc->drc_force) {
			/*
			 * Destroy any snapshots of drc_tofs (origin_head)
			 * after the origin (the snap before drc_ds).
			 */
			uint64_t obj;

			obj = dsl_dataset_phys(origin_head)->ds_prev_snap_obj;
			while (obj !=
			    dsl_dataset_phys(drc->drc_ds)->ds_prev_snap_obj) {
				dsl_dataset_t *snap;
				VERIFY0(dsl_dataset_hold_obj(dp, obj, FTAG,
				    &snap));
				ASSERT3P(snap->ds_dir, ==, origin_head->ds_dir);
				obj = dsl_dataset_phys(snap)->ds_prev_snap_obj;
				dsl_destroy_snapshot_sync_impl(snap,
				    B_FALSE, tx);
				dsl_dataset_rele(snap, FTAG);
			}
		}
		VERIFY3P(drc->drc_ds->ds_prev, ==,
		    origin_head->ds_prev);

		dsl_dataset_clone_swap_sync_impl(drc->drc_ds,
		    origin_head, tx);
		dsl_dataset_snapshot_sync_impl(origin_head,
		    drc->drc_tosnap, tx);

		/* set snapshot's creation time and guid */
		dmu_buf_will_dirty(origin_head->ds_prev->ds_dbuf, tx);
		dsl_dataset_phys(origin_head->ds_prev)->ds_creation_time =
		    drc->drc_drrb->drr_creation_time;
		dsl_dataset_phys(origin_head->ds_prev)->ds_guid =
		    drc->drc_drrb->drr_toguid;
		dsl_dataset_phys(origin_head->ds_prev)->ds_flags &=
		    ~DS_FLAG_INCONSISTENT;

		dmu_buf_will_dirty(origin_head->ds_dbuf, tx);
		dsl_dataset_phys(origin_head)->ds_flags &=
		    ~DS_FLAG_INCONSISTENT;

		drc->drc_newsnapobj =
		    dsl_dataset_phys(origin_head)->ds_prev_snap_obj;

		dsl_dataset_rele(origin_head, FTAG);
		dsl_destroy_head_sync_impl(drc->drc_ds, tx);

		if (drc->drc_owner != NULL)
			VERIFY3P(origin_head->ds_owner, ==, drc->drc_owner);
	} else {
		dsl_dataset_t *ds = drc->drc_ds;

		dsl_dataset_snapshot_sync_impl(ds, drc->drc_tosnap, tx);

		/* set snapshot's creation time and guid */
		dmu_buf_will_dirty(ds->ds_prev->ds_dbuf, tx);
		dsl_dataset_phys(ds->ds_prev)->ds_creation_time =
		    drc->drc_drrb->drr_creation_time;
		dsl_dataset_phys(ds->ds_prev)->ds_guid =
		    drc->drc_drrb->drr_toguid;
		dsl_dataset_phys(ds->ds_prev)->ds_flags &=
		    ~DS_FLAG_INCONSISTENT;

		dmu_buf_will_dirty(ds->ds_dbuf, tx);
		dsl_dataset_phys(ds)->ds_flags &= ~DS_FLAG_INCONSISTENT;
		if (dsl_dataset_has_resume_receive_state(ds)) {
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_FROMGUID, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_OBJECT, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_OFFSET, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_BYTES, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_TOGUID, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_TONAME, tx);
			(void) zap_remove(dp->dp_meta_objset, ds->ds_object,
			    DS_FIELD_RESUME_REDACT_BOOKMARK_SNAPS, tx);
		}
		drc->drc_newsnapobj =
		    dsl_dataset_phys(drc->drc_ds)->ds_prev_snap_obj;
	}
	/*
	 * Release the hold from dmu_recv_begin.  This must be done before
	 * we return to open context, so that when we free the dataset's dnode,
	 * we can evict its bonus buffer.
	 */
	dsl_dataset_disown(drc->drc_ds, dmu_recv_tag);
	drc->drc_ds = NULL;
}

static int
add_ds_to_guidmap(const char *name, avl_tree_t *guid_map, uint64_t snapobj)
{
	dsl_pool_t *dp;
	dsl_dataset_t *snapds;
	guid_map_entry_t *gmep;
	int err;

	ASSERT(guid_map != NULL);

	err = dsl_pool_hold(name, FTAG, &dp);
	if (err != 0)
		return (err);
	gmep = kmem_alloc(sizeof (*gmep), KM_SLEEP);
	err = dsl_dataset_hold_obj(dp, snapobj, gmep, &snapds);
	if (err == 0) {
		gmep->guid = dsl_dataset_phys(snapds)->ds_guid;
		gmep->gme_ds = snapds;
		avl_add(guid_map, gmep);
		dsl_dataset_long_hold(snapds, gmep);
	} else {
		kmem_free(gmep, sizeof (*gmep));
	}

	dsl_pool_rele(dp, FTAG);
	return (err);
}

static int dmu_recv_end_modified_blocks = 3;

static int
dmu_recv_existing_end(dmu_recv_cookie_t *drc)
{
#ifdef _KERNEL
	/*
	 * We will be destroying the ds; make sure its origin is unmounted if
	 * necessary.
	 */
	char name[ZFS_MAX_DATASET_NAME_LEN];
	dsl_dataset_name(drc->drc_ds, name);
	zfs_destroy_unmount_origin(name);
#endif

	return (dsl_sync_task(drc->drc_tofs,
	    dmu_recv_end_check, dmu_recv_end_sync, drc,
	    dmu_recv_end_modified_blocks, ZFS_SPACE_CHECK_NORMAL));
}

static int
dmu_recv_new_end(dmu_recv_cookie_t *drc)
{
	return (dsl_sync_task(drc->drc_tofs,
	    dmu_recv_end_check, dmu_recv_end_sync, drc,
	    dmu_recv_end_modified_blocks, ZFS_SPACE_CHECK_NORMAL));
}

int
dmu_recv_end(dmu_recv_cookie_t *drc, void *owner)
{
	int error;

	drc->drc_owner = owner;

	if (drc->drc_newfs)
		error = dmu_recv_new_end(drc);
	else
		error = dmu_recv_existing_end(drc);

	if (error != 0) {
		dmu_recv_cleanup_ds(drc);
	} else if (drc->drc_guid_to_ds_map != NULL) {
		(void) add_ds_to_guidmap(drc->drc_tofs,
		    drc->drc_guid_to_ds_map,
		    drc->drc_newsnapobj);
	}
	return (error);
}

/*
 * Return TRUE if this objset is currently being received into.
 */
boolean_t
dmu_objset_is_receiving(objset_t *os)
{
	return (os->os_dsl_dataset != NULL &&
	    os->os_dsl_dataset->ds_owner == dmu_recv_tag);
}
