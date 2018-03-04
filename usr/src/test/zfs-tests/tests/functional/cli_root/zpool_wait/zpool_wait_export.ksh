#!/usr/bin/ksh -p
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright (c) 2018 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zpool_wait/zpool_wait.kshlib

#
# DESCRIPTION:
# 'zpool wait' returns when the pool is exported, and can be used to wait for
# an activity that is resumed after import.
#
# STRATEGY:
# 1. Create a pool and start initializing it.
# 2. Start 'zpool wait', and make sure it blocks.
# 3. Export the pool, and make sure the waiting process exits.
# 4. Re-import the pool.
# 5. Start 'zpool wait' again, and make sure it blocks.
# 6. Cancel the initialization operation, make sure the waiting process exits.
#

function cleanup
{
	kill_if_running $pid
	poolexists $TESTPOOL && destroy_pool $TESTPOOL

	[[ "$default_chunk_sz" ]] &&
	    log_must mdb_ctf_set_int zfs_initialize_chunk_size $default_chunk_sz
}

function do_test
{
	typeset start_cmd=$1
	typeset stop_cmd=$2

	log_must eval "$start_cmd"

	log_bkgrnd zpool wait -t initialize $TESTPOOL
	pid=$!

	# Make sure that we are really waiting
	log_must sleep 3
	proc_must_exist $pid

	log_must eval "$stop_cmd"
	bkgrnd_proc_succeeded $pid
}

typeset pid default_chunk_sz
log_onexit cleanup

# Make sure the initialization takes a while
default_chunk_sz=$(mdb_get_hex zfs_initialize_chunk_size)
log_must mdb_ctf_set_int zfs_initialize_chunk_size 0t512

log_must zpool create $TESTPOOL $DISK1

#
# Start intializing a disk, and then make sure that the wait returns when the
# pool is exported.
#
do_test "zpool initialize $TESTPOOL $DISK1" "zpool export $TESTPOOL"
#
# Import the pool, causing the initialization to resume, and make sure we can
# wait for it again.
#
do_test "zpool import $TESTPOOL" "zpool initialize -c $TESTPOOL"

log_pass "'zpool wait' works with pool import/export."
