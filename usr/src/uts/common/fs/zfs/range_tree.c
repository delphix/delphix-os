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

#include <sys/zfs_context.h>
#include <sys/zfs_debug.h>
#include <sys/range_tree.h>

static inline void
rs_copy(range_seg_t *src, range_seg_t *dest, range_tree_t *rt)
{
	ASSERT3U(rt->rt_type, <=, RANGE_SEG_NUM_TYPES);
	size_t size = 0;
	switch (rt->rt_type) {
	case RANGE_SEG32:
		size = sizeof (range_seg32_t);
		break;
	case RANGE_SEG64:
		size = sizeof (range_seg64_t);
		break;
	default:
		VERIFY(0);
	}
	bcopy(src, dest, size);
}

void
range_tree_stat_verify(range_tree_t *rt)
{
	range_seg_t *rs;
	btree_index_t where;
	uint64_t hist[RANGE_TREE_HISTOGRAM_SIZE] = { 0 };
	int i;

	for (rs = btree_first(&rt->rt_root, &where); rs != NULL;
	    rs = btree_next(&rt->rt_root, &where, &where)) {
		uint64_t size = rs_get_end(rs, rt) - rs_get_start(rs, rt);
		int idx	= highbit64(size) - 1;

		hist[idx]++;
		ASSERT3U(hist[idx], !=, 0);
	}

	for (i = 0; i < RANGE_TREE_HISTOGRAM_SIZE; i++) {
		if (hist[i] != rt->rt_histogram[i]) {
			zfs_dbgmsg("i=%d, hist=%p, hist=%llu, rt_hist=%llu",
			    i, hist, hist[i], rt->rt_histogram[i]);
		}
		VERIFY3U(hist[i], ==, rt->rt_histogram[i]);
	}
}

static void
range_tree_stat_incr(range_tree_t *rt, range_seg_t *rs)
{
	uint64_t size = rs_get_end(rs, rt) - rs_get_start(rs, rt);
	int idx = highbit64(size) - 1;

	ASSERT(size != 0);
	ASSERT3U(idx, <,
	    sizeof (rt->rt_histogram) / sizeof (*rt->rt_histogram));

	rt->rt_histogram[idx]++;
	ASSERT3U(rt->rt_histogram[idx], !=, 0);
}

static void
range_tree_stat_decr(range_tree_t *rt, range_seg_t *rs)
{
	uint64_t size = rs_get_end(rs, rt) - rs_get_start(rs, rt);
	int idx = highbit64(size) - 1;

	ASSERT(size != 0);
	ASSERT3U(idx, <,
	    sizeof (rt->rt_histogram) / sizeof (*rt->rt_histogram));

	ASSERT3U(rt->rt_histogram[idx], !=, 0);
	rt->rt_histogram[idx]--;
}

static int
range_tree_seg32_compare(const void *x1, const void *x2)
{
	const range_seg32_t *r1 = x1;
	const range_seg32_t *r2 = x2;

	return ((r1->rs_start >= r2->rs_end) - (r1->rs_end <= r2->rs_start));
}

static int
range_tree_seg64_compare(const void *x1, const void *x2)
{
	const range_seg64_t *r1 = x1;
	const range_seg64_t *r2 = x2;

	return ((r1->rs_start >= r2->rs_end) - (r1->rs_end <= r2->rs_start));
}

range_tree_t *
range_tree_create(range_tree_ops_t *ops, range_seg_type_t type, void *arg,
    uint64_t start, uint64_t shift)
{
	range_tree_t *rt;

	rt = kmem_zalloc(sizeof (range_tree_t), KM_SLEEP);

	ASSERT3U(shift, <, 64);
	ASSERT3U(type, <=, RANGE_SEG_NUM_TYPES);
	size_t size;
	int (*compare) (const void *, const void *);
	switch (type) {
	case RANGE_SEG32:
		size = sizeof (range_seg32_t);
		compare = range_tree_seg32_compare;
		break;
	case RANGE_SEG64:
		size = sizeof (range_seg64_t);
		compare = range_tree_seg64_compare;
		break;
	default:
		panic("Invalid range seg type %d", type);
	}
	btree_create(&rt->rt_root, compare, size);

	rt->rt_ops = ops;
	rt->rt_arg = arg;
	rt->rt_type = type;
	rt->rt_start = start;
	rt->rt_shift = shift;

	if (rt->rt_ops != NULL)
		rt->rt_ops->rtop_create(rt, rt->rt_arg);

	return (rt);
}

void
range_tree_destroy(range_tree_t *rt)
{
	VERIFY0(rt->rt_space);

	if (rt->rt_ops != NULL)
		rt->rt_ops->rtop_destroy(rt, rt->rt_arg);

	btree_destroy(&rt->rt_root);
	kmem_free(rt, sizeof (*rt));
}

void
range_tree_add(void *arg, uint64_t start, uint64_t size)
{
	range_tree_t *rt = arg;
	btree_index_t where;
	range_seg_t *rs_before, *rs_after, *rs;
	range_seg_max_t tmp, rsearch;
	uint64_t end = start + size;
	boolean_t merge_before, merge_after;

	VERIFY(size != 0);
	ASSERT3U(start + size, >, start);

	rs_set_start(&rsearch, rt, start);
	rs_set_end(&rsearch, rt, end);
	rs = btree_find(&rt->rt_root, &rsearch, &where);

	/* Make sure we don't overlap with either of our neighbors */
	VERIFY3P(rs, ==, NULL);

	btree_index_t where_before, where_after;
	rs_before = btree_prev(&rt->rt_root, &where, &where_before);
	rs_after = btree_next(&rt->rt_root, &where, &where_after);

	merge_before = (rs_before != NULL && rs_get_end(rs_before, rt) ==
	    start);
	merge_after = (rs_after != NULL && rs_get_start(rs_after, rt) == end);

	if (merge_before && merge_after) {
		if (rt->rt_ops != NULL) {
			rt->rt_ops->rtop_remove(rt, rs_before, rt->rt_arg);
			rt->rt_ops->rtop_remove(rt, rs_after, rt->rt_arg);
		}

		range_tree_stat_decr(rt, rs_before);
		range_tree_stat_decr(rt, rs_after);

		rs_copy(rs_after, &tmp, rt);
		uint64_t before_start = rs_get_start_raw(rs_before, rt);
		btree_remove_from(&rt->rt_root, &where_before);

		/*
		 * We have to re-find the node because our old reference is
		 * invalid as soon as we do any mutating btree operations.
		 */
		rs_after = btree_find(&rt->rt_root, &tmp, &where_after);
		rs_set_start_raw(rs_after, rt, before_start);
		rs = rs_after;
	} else if (merge_before) {
		if (rt->rt_ops != NULL)
			rt->rt_ops->rtop_remove(rt, rs_before, rt->rt_arg);

		range_tree_stat_decr(rt, rs_before);

		rs_set_end(rs_before, rt, end);
		rs = rs_before;
	} else if (merge_after) {
		if (rt->rt_ops != NULL)
			rt->rt_ops->rtop_remove(rt, rs_after, rt->rt_arg);

		range_tree_stat_decr(rt, rs_after);

		rs_set_start(rs_after, rt, start);
		rs = rs_after;
	} else {
		rs = &tmp;
		rs_set_start(rs, rt, start);
		rs_set_end(rs, rt, end);
		btree_insert(&rt->rt_root, rs, &where);
	}

	if (rt->rt_ops != NULL)
		rt->rt_ops->rtop_add(rt, rs, rt->rt_arg);

	range_tree_stat_incr(rt, rs);
	rt->rt_space += size;
}

void
range_tree_remove(void *arg, uint64_t start, uint64_t size)
{
	range_tree_t *rt = arg;
	btree_index_t where;
	range_seg_t *rs;
	range_seg_max_t rsearch, rs_tmp;
	uint64_t end = start + size;
	boolean_t left_over, right_over;

	VERIFY3U(size, !=, 0);
	VERIFY3U(size, <=, rt->rt_space);
	if (rt->rt_type == RANGE_SEG64)
		ASSERT3U(start + size, >, start);

	rs_set_start(&rsearch, rt, start);
	rs_set_end(&rsearch, rt, end);
	rs = btree_find(&rt->rt_root, &rsearch, &where);

	/* Make sure we completely overlap with someone */
	if (rs == NULL) {
		zfs_panic_recover("zfs: removing nonexistent segment from "
		    "range tree (offset=%llu size=%llu)",
		    (longlong_t)start, (longlong_t)size);
		return;
	}
	VERIFY3U(rs_get_start(rs, rt), <=, start);
	VERIFY3U(rs_get_end(rs, rt), >=, end);

	left_over = (rs_get_start(rs, rt) != start);
	right_over = (rs_get_end(rs, rt) != end);

	range_tree_stat_decr(rt, rs);

	if (rt->rt_ops != NULL)
		rt->rt_ops->rtop_remove(rt, rs, rt->rt_arg);

	if (left_over && right_over) {
		range_seg_max_t newseg;
		rs_set_start(&newseg, rt, end);
		rs_set_end_raw(&newseg, rt, rs_get_end_raw(rs, rt));
		range_tree_stat_incr(rt, &newseg);

		// This modifies the buffer already inside the range tree
		rs_set_end(rs, rt, start);

		rs_copy(rs, &rs_tmp, rt);
		if (btree_next(&rt->rt_root, &where, &where) != NULL)
			btree_insert(&rt->rt_root, &newseg, &where);
		else
			btree_add(&rt->rt_root, &newseg);

		if (rt->rt_ops != NULL)
			rt->rt_ops->rtop_add(rt, &newseg, rt->rt_arg);
	} else if (left_over) {
		// This modifies the buffer already inside the range tree
		rs_set_end(rs, rt, start);
		rs_copy(rs, &rs_tmp, rt);
	} else if (right_over) {
		// This modifies the buffer already inside the range tree
		rs_set_start(rs, rt, end);
		rs_copy(rs, &rs_tmp, rt);
	} else {
		btree_remove_from(&rt->rt_root, &where);
		rs = NULL;
	}

	if (rs != NULL) {
		range_tree_stat_incr(rt, &rs_tmp);

		if (rt->rt_ops != NULL)
			rt->rt_ops->rtop_add(rt, &rs_tmp, rt->rt_arg);
	}

	rt->rt_space -= size;
}

static range_seg_t *
range_tree_find_impl(range_tree_t *rt, uint64_t start, uint64_t size)
{
	range_seg_max_t rsearch;
	uint64_t end = start + size;

	VERIFY(size != 0);

	rs_set_start(&rsearch, rt, start);
	rs_set_end(&rsearch, rt, end);
	return (btree_find(&rt->rt_root, &rsearch, NULL));
}

static range_seg_t *
range_tree_find(range_tree_t *rt, uint64_t start, uint64_t size)
{
	if (rt->rt_type == RANGE_SEG64)
		ASSERT3U(start + size, >, start);

	range_seg_t *rs = range_tree_find_impl(rt, start, size);
	if (rs != NULL && rs_get_start(rs, rt) <= start &&
	    rs_get_end(rs, rt) >= start + size) {
		return (rs);
	}
	return (NULL);
}

void
range_tree_verify_not_present(range_tree_t *rt, uint64_t off, uint64_t size)
{
	range_seg_t *rs;

	rs = range_tree_find(rt, off, size);
	if (rs != NULL)
		panic("segment already in tree; rs=%p", (void *)rs);
}

boolean_t
range_tree_contains(range_tree_t *rt, uint64_t start, uint64_t size)
{
	return (range_tree_find(rt, start, size) != NULL);
}

/*
 * Returns the first subset of the given range which overlaps with the range
 * tree. Returns true if there is a segment in the range, and false if there
 * isn't.
 */
boolean_t
range_tree_find_in(range_tree_t *rt, uint64_t start, uint64_t size,
    uint64_t *ostart, uint64_t *osize)
{
	if (rt->rt_type == RANGE_SEG64)
		ASSERT3U(start + size, >, start);

	range_seg_max_t rsearch;
	rs_set_start(&rsearch, rt, start);
	rs_set_end_raw(&rsearch, rt, rs_get_start_raw(&rsearch, rt) + 1);

	btree_index_t where;
	range_seg_t *rs = btree_find(&rt->rt_root, &rsearch, &where);
	if (rs != NULL) {
		*ostart = start;
		*osize = MIN(size, rs_get_end(rs, rt) - start);
		return (B_TRUE);
	}

	rs = btree_next(&rt->rt_root, &where, &where);
	if (rs == NULL || rs_get_start(rs, rt) > start + size)
		return (B_FALSE);

	*ostart = rs_get_start(rs, rt);
	*osize = MIN(start + size, rs_get_end(rs, rt)) -
	    rs_get_start(rs, rt);
	return (B_TRUE);
}

/*
 * Ensure that this range is not in the tree, regardless of whether
 * it is currently in the tree.
 */
void
range_tree_clear(range_tree_t *rt, uint64_t start, uint64_t size)
{
	range_seg_t *rs;

	if (size == 0)
		return;

	if (rt->rt_type == RANGE_SEG64)
		ASSERT3U(start + size, >, start);

	while ((rs = range_tree_find_impl(rt, start, size)) != NULL) {
		uint64_t free_start = MAX(rs_get_start(rs, rt), start);
		uint64_t free_end = MIN(rs_get_end(rs, rt), start + size);
		range_tree_remove(rt, free_start, free_end - free_start);
	}
}

void
range_tree_swap(range_tree_t **rtsrc, range_tree_t **rtdst)
{
	range_tree_t *rt;

	ASSERT0(range_tree_space(*rtdst));
	ASSERT0(btree_numnodes(&(*rtdst)->rt_root));

	rt = *rtsrc;
	*rtsrc = *rtdst;
	*rtdst = rt;
}

void
range_tree_vacate(range_tree_t *rt, range_tree_func_t *func, void *arg)
{
	if (rt->rt_ops != NULL)
		rt->rt_ops->rtop_vacate(rt, rt->rt_arg);

	if (func != NULL) {
		range_seg_t *rs;
		btree_index_t *cookie = NULL;

		while ((rs = btree_destroy_nodes(&rt->rt_root, &cookie)) !=
		    NULL) {
			func(arg, rs_get_start(rs, rt), rs_get_end(rs, rt) -
			    rs_get_start(rs, rt));
		}
	} else {
		btree_clear(&rt->rt_root);
	}

	bzero(rt->rt_histogram, sizeof (rt->rt_histogram));
	rt->rt_space = 0;
}

void
range_tree_walk(range_tree_t *rt, range_tree_func_t *func, void *arg)
{
	btree_index_t where;
	for (range_seg_t *rs = btree_first(&rt->rt_root, &where); rs != NULL;
	    rs = btree_next(&rt->rt_root, &where, &where)) {
		func(arg, rs_get_start(rs, rt), rs_get_end(rs, rt) -
		    rs_get_start(rs, rt));
	}
}

uint64_t
range_tree_space(range_tree_t *rt)
{
	return (rt->rt_space);
}

uint64_t
range_tree_numsegs(range_tree_t *rt)
{
	return ((rt == NULL) ? 0 : btree_numnodes(&rt->rt_root));
}

boolean_t
range_tree_is_empty(range_tree_t *rt)
{
	ASSERT(rt != NULL);
	return (range_tree_space(rt) == 0);
}

/*
 * Remove any overlapping ranges between the given segment [start, end)
 * from removefrom. Add non-overlapping leftovers to addto.
 */
void
range_tree_remove_xor_add_segment(uint64_t start, uint64_t end,
    range_tree_t *removefrom, range_tree_t *addto)
{
	btree_index_t where;
	range_seg_max_t starting_rs;
	rs_set_start(&starting_rs, removefrom, start);
	rs_set_end_raw(&starting_rs, removefrom, rs_get_start_raw(&starting_rs,
	    removefrom) + 1);

	range_seg_t *current = btree_find(&removefrom->rt_root,
	    &starting_rs, &where);

	if (current == NULL)
		current = btree_next(&removefrom->rt_root, &where, &where);

	range_seg_t *next;
	for (; current != NULL; current = next) {
		if (start == end)
			return;
		VERIFY3U(start, <, end);

		/* there is no overlap */
		if (end <= rs_get_start(current, removefrom)) {
			range_tree_add(addto, start, end - start);
			return;
		}

		uint64_t overlap_start = MAX(rs_get_start(current, removefrom),
		    start);
		uint64_t overlap_end = MIN(rs_get_end(current, removefrom),
		    end);
		uint64_t overlap_size = overlap_end - overlap_start;
		ASSERT3S(overlap_size, >, 0);
		range_seg_max_t rs;
		rs_copy(current, &rs, removefrom);

		range_tree_remove(removefrom, overlap_start, overlap_size);

		if (start < overlap_start)
			range_tree_add(addto, start, overlap_start - start);

		start = overlap_end;
		next = btree_find(&removefrom->rt_root, &rs, &where);
		/*
		 * If we find something here, we only removed part of the
		 * current segment. Either there's some left at the end
		 * because we're reached the end of the range we're removing,
		 * or there's some left at the start because we started
		 * partway through the range.  Either way, we continue with
		 * the loop. If it's the former, we'll return at the start of
		 * the loop, and if it's the latter we'll see if there is more
		 * area to process.
		 */
		if (next != NULL) {
			ASSERT(start == end || start == rs_get_end(&rs,
			    removefrom));
		}

		next = btree_next(&removefrom->rt_root, &where, &where);
	}
	VERIFY3P(current, ==, NULL);

	if (start != end) {
		VERIFY3U(start, <, end);
		range_tree_add(addto, start, end - start);
	} else {
		VERIFY3U(start, ==, end);
	}
}

/*
 * For each entry in rt, if it exists in removefrom, remove it
 * from removefrom. Otherwise, add it to addto.
 */
void
range_tree_remove_xor_add(range_tree_t *rt, range_tree_t *removefrom,
    range_tree_t *addto)
{
	btree_index_t where;
	for (range_seg_t *rs = btree_first(&rt->rt_root, &where); rs;
	    rs = btree_next(&rt->rt_root, &where, &where)) {
		range_tree_remove_xor_add_segment(rs_get_start(rs, rt),
		    rs_get_end(rs, rt), removefrom, addto);
	}
}

uint64_t
range_tree_min(range_tree_t *rt)
{
	range_seg_t *rs = btree_first(&rt->rt_root, NULL);
	return (rs != NULL ? rs_get_start(rs, rt) : 0);
}

uint64_t
range_tree_max(range_tree_t *rt)
{
	range_seg_t *rs = btree_last(&rt->rt_root, NULL);
	return (rs != NULL ? rs_get_end(rs, rt) : 0);
}

uint64_t
range_tree_span(range_tree_t *rt)
{
	return (range_tree_max(rt) - range_tree_min(rt));
}
