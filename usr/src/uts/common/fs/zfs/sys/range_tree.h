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
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright (c) 2013, 2019 by Delphix. All rights reserved.
 */

#ifndef _SYS_RANGE_TREE_H
#define	_SYS_RANGE_TREE_H

#include <sys/btree.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	RANGE_TREE_HISTOGRAM_SIZE	64

typedef struct range_tree_ops range_tree_ops_t;

typedef enum range_seg_type {
	RANGE_SEG32,
	RANGE_SEG64,
	RANGE_SEG_NUM_TYPES,
} range_seg_type_t;

/*
 * Note: the range_tree may not be accessed concurrently; consumers
 * must provide external locking if required.
 */
typedef struct range_tree {
	btree_t		rt_root;	/* offset-ordered segment b-tree */
	uint64_t	rt_space;	/* sum of all segments in the map */
	range_seg_type_t rt_type;	/* type of range_seg_t in use */
	/*
	 * All data that is stored in the range tree must have a start higher
	 * than or equal to rt_start, and all sizes and offsets must be
	 * multiples of 1 << rt_shift.
	 */
	uint8_t		rt_shift;
	uint64_t	rt_start;
	range_tree_ops_t *rt_ops;
	void		*rt_arg;

	/*
	 * The rt_histogram maintains a histogram of ranges. Each bucket,
	 * rt_histogram[i], contains the number of ranges whose size is:
	 * 2^i <= size of range in bytes < 2^(i+1)
	 */
	uint64_t	rt_histogram[RANGE_TREE_HISTOGRAM_SIZE];
} range_tree_t;

typedef struct range_seg32 {
	uint32_t	rs_start;	/* starting offset of this segment */
	uint32_t	rs_end;		/* ending offset (non-inclusive) */
} range_seg32_t;

/*
 * Extremely large metaslabs, vdev-wide trees, and dnode-wide trees may
 * require 64-bit integers for ranges.
 */
typedef struct range_seg64 {
	uint64_t	rs_start;	/* starting offset of this segment */
	uint64_t	rs_end;		/* ending offset (non-inclusive) */
} range_seg64_t;

/*
 * This type needs to be the largest of the range segs, since it will be stack
 * allocated and then cast the actual type to do tree operations.
 */
typedef range_seg64_t range_seg_max_t;

/*
 * This is just for clarity of code purposes, so we can make it clear that a
 * pointer is to a range seg of some type; when we need to do the actual math,
 * we'll figure out the real type.
 */
typedef void range_seg_t;

struct range_tree_ops {
	void    (*rtop_create)(range_tree_t *rt, void *arg);
	void    (*rtop_destroy)(range_tree_t *rt, void *arg);
	void	(*rtop_add)(range_tree_t *rt, void *rs, void *arg);
	void    (*rtop_remove)(range_tree_t *rt, void *rs, void *arg);
	void	(*rtop_vacate)(range_tree_t *rt, void *arg);
};

static inline uint64_t
rs_get_start_raw(range_seg_t *rs, range_tree_t *rt)
{
	ASSERT3U(rt->rt_type, <=, RANGE_SEG_NUM_TYPES);
	switch (rt->rt_type) {
	case RANGE_SEG32:
		return (((range_seg32_t *)rs)->rs_start);
	case RANGE_SEG64:
		return (((range_seg64_t *)rs)->rs_start);
	default:
		VERIFY(0);
		return (0);
	}
}

static inline uint64_t
rs_get_end_raw(range_seg_t *rs, range_tree_t *rt)
{
	ASSERT3U(rt->rt_type, <=, RANGE_SEG_NUM_TYPES);
	switch (rt->rt_type) {
	case RANGE_SEG32:
		return (((range_seg32_t *)rs)->rs_end);
	case RANGE_SEG64:
		return (((range_seg64_t *)rs)->rs_end);
	default:
		VERIFY(0);
		return (0);
	}
}

static inline uint64_t
rs_get_start(range_seg_t *rs, range_tree_t *rt)
{
	return ((rs_get_start_raw(rs, rt) << rt->rt_shift) + rt->rt_start);
}

static inline uint64_t
rs_get_end(range_seg_t *rs, range_tree_t *rt)
{
	return ((rs_get_end_raw(rs, rt) << rt->rt_shift) + rt->rt_start);
}

static inline void
rs_set_start_raw(range_seg_t *rs, range_tree_t *rt, uint64_t start)
{
	ASSERT3U(rt->rt_type, <=, RANGE_SEG_NUM_TYPES);
	switch (rt->rt_type) {
	case RANGE_SEG32:
		ASSERT3U(start, <=, UINT32_MAX);
		((range_seg32_t *)rs)->rs_start = (uint32_t)start;
		break;
	case RANGE_SEG64:
		((range_seg64_t *)rs)->rs_start = start;
		break;
	default:
		VERIFY(0);
	}
}

static inline void
rs_set_end_raw(range_seg_t *rs, range_tree_t *rt, uint64_t end)
{
	ASSERT3U(rt->rt_type, <=, RANGE_SEG_NUM_TYPES);
	switch (rt->rt_type) {
	case RANGE_SEG32:
		ASSERT3U(end, <=, UINT32_MAX);
		((range_seg32_t *)rs)->rs_end = (uint32_t)end;
		break;
	case RANGE_SEG64:
		((range_seg64_t *)rs)->rs_end = end;
		break;
	default:
		VERIFY(0);
	}
}

static inline void
rs_set_start(range_seg_t *rs, range_tree_t *rt, uint64_t start)
{
	ASSERT3U(start, >=, rt->rt_start);
	ASSERT(IS_P2ALIGNED(start, 1ULL << rt->rt_shift));
	rs_set_start_raw(rs, rt, (start - rt->rt_start) >> rt->rt_shift);
}

static inline void
rs_set_end(range_seg_t *rs, range_tree_t *rt, uint64_t end)
{
	ASSERT3U(end, >=, rt->rt_start);
	ASSERT(IS_P2ALIGNED(end, 1ULL << rt->rt_shift));
	rs_set_end_raw(rs, rt, (end - rt->rt_start) >> rt->rt_shift);
}

typedef void range_tree_func_t(void *arg, uint64_t start, uint64_t size);

range_tree_t *range_tree_create(range_tree_ops_t *ops, range_seg_type_t type,
    void *arg, uint64_t start, uint64_t shift);
void range_tree_destroy(range_tree_t *rt);
boolean_t range_tree_contains(range_tree_t *rt, uint64_t start, uint64_t size);
boolean_t range_tree_find_in(range_tree_t *rt, uint64_t start, uint64_t size,
    uint64_t *ostart, uint64_t *osize);
void range_tree_verify_not_present(range_tree_t *rt,
    uint64_t start, uint64_t size);
uint64_t range_tree_space(range_tree_t *rt);
uint64_t range_tree_numsegs(range_tree_t *rt);
boolean_t range_tree_is_empty(range_tree_t *rt);
void range_tree_swap(range_tree_t **rtsrc, range_tree_t **rtdst);
void range_tree_stat_verify(range_tree_t *rt);
uint64_t range_tree_min(range_tree_t *rt);
uint64_t range_tree_max(range_tree_t *rt);
uint64_t range_tree_span(range_tree_t *rt);

void range_tree_add(void *arg, uint64_t start, uint64_t size);
void range_tree_remove(void *arg, uint64_t start, uint64_t size);
void range_tree_clear(range_tree_t *rt, uint64_t start, uint64_t size);

void range_tree_vacate(range_tree_t *rt, range_tree_func_t *func, void *arg);
void range_tree_walk(range_tree_t *rt, range_tree_func_t *func, void *arg);

void range_tree_remove_xor_add_segment(uint64_t start, uint64_t end,
    range_tree_t *removefrom, range_tree_t *addto);
void range_tree_remove_xor_add(range_tree_t *rt, range_tree_t *removefrom,
    range_tree_t *addto);

#ifdef	__cplusplus
}
#endif

#endif	/* _SYS_RANGE_TREE_H */
