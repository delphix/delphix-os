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
# 'zpool wait' returns promptly when sent a signal
#
# STRATEGY:
# 1. Create a pool.
# 2. Start some long-running activity and wait for it.
# 3. Send the 'zpool wait' process SIGTERM, and make sure it returns from the
#    kernel and exits promptly.
#

function cleanup
{
	kill_if_running $pid

	poolexists $TESTPOOL && destroy_pool $TESTPOOL

	[[ "$default_chunk_sz" ]] &&
	    log_must mdb_ctf_set_int zfs_initialize_chunk_size $default_chunk_sz
}

typeset pid default_chunk_sz

log_onexit cleanup

log_must zpool create -f $TESTPOOL $DISK1

default_chunk_sz=$(mdb_get_hex zfs_initialize_chunk_size)
log_must mdb_ctf_set_int zfs_initialize_chunk_size 0t512

log_must zpool initialize $TESTPOOL $DISK1
log_bkgrnd zpool wait -t initialize $TESTPOOL
pid=$!

# Make sure that we have really started waiting
log_must sleep 3
proc_must_exist $pid

#
# Send SIGTERM to 'zpool wait' and make sure that it exits promptly, and with
# a non-zero status
#
log_must kill $pid
log_must sleep $WAIT_EXIT_GRACE
proc_must_not_exist $pid
wait $pid && log_fail 'zpool wait did not exit with an error'

log_pass "'zpool wait' returned when canceled."
