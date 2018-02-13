# !/bin/ksh
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

# DESCRIPTION
# Test race conditions for livelist condensing

# STRATEGY
# These tests exercise code paths that deal with a livelist being
# simultaneously condensed and deactivated (deleted, exported or disabled).
# Delays are injected to make the races more likely and a counter variable
# keeps track of whether or not the code path is reached. A test will fail if
# it exceedes the maximum number of test iterations (max_iter) without reaching
# the desired code path.
#
# 1. Deletion race: repeatedly overwrite the same file to trigger condense
# and then delete the clone.
# 2. Disable race: create several files which are shared between the clone and
# the snapshot. Overwrite enough files to trigger condenses and disabling of
# the livelist.
# 3. Export race: repeatedly overwrite the same file to trigger condense and
# then export the pool.

. $STF_SUITE/include/libtest.shlib

function cleanup
{
	log_must zfs destroy -Rf $TESTPOOL/$TESTFS1
	# reset the livelist sublist size to the original value
	mdb_ctf_set_int zfs_livelist_max_entries $ORIGINAL_MAX
	# reset the condense delays to 0
	mdb_ctf_set_int zfs_livelist_condense_zthr_delay 0
	mdb_ctf_set_int zfs_livelist_condense_sync_delay 0
}

function delete_race
{
	iter=0
	mdb_ctf_set_int "$1" 0
	while [[ "0" == "$(mdb_get_uint32 "$1")" ]]; do
		log_must zfs clone $TESTPOOL/$TESTFS1@snap $TESTPOOL/$TESTCLONE
		for i in {1..10}; do
			sync
			log_must mkfile 5m /$TESTPOOL/$TESTCLONE/out
		done
		log_must zfs destroy $TESTPOOL/$TESTCLONE
		((iter+=1))
		[[ $iter -lt $max_iter ]] || \
			log_fail "delete/condense race not reached"
	done
}

function export_race
{
	iter=0
	mdb_ctf_set_int "$1" 0
	log_must zfs clone $TESTPOOL/$TESTFS1@snap $TESTPOOL/$TESTCLONE
	while [[ "0" == "$(mdb_get_uint32 "$1")" ]]; do
		sync
		log_must mkfile 5m /$TESTPOOL/$TESTCLONE/out
		log_must zpool export $TESTPOOL
		log_must zpool import $TESTPOOL
		((iter+=1))
		[[ $iter -lt $max_iter ]] || \
			log_fail "export/condense race not reached"
	done
	log_must zfs destroy $TESTPOOL/$TESTCLONE
}

function disable_race
{
	iter=0
	mdb_ctf_set_int "$1" 0
	while [[ "0" == "$(mdb_get_uint32 "$1")" ]]; do
		log_must zfs clone $TESTPOOL/$TESTFS1@snap $TESTPOOL/$TESTCLONE
		# overwrite 80 percent of the snapshot
		for i in {1..8}; do
			sync
			log_must mkfile 1m /$TESTPOOL/$TESTCLONE/$i
		done
		# overwrite previous files written to clone (causing condense)
		# and the final 20 percent, causing disable
		for i in {1..10}; do
			sync
			log_must mkfile 1m /$TESTPOOL/$TESTCLONE/$i
		done
		log_must zfs destroy $TESTPOOL/$TESTCLONE
		((iter+=1))
		[[ $iter -lt $max_iter ]] || \
			log_fail "disable/condense race not reached"
	done
}

ORIGINAL_MAX=$(mdb_get_hex zfs_livelist_max_entries)

log_onexit cleanup

log_must zfs create $TESTPOOL/$TESTFS1
log_must mkfile 100m /$TESTPOOL/$TESTFS1/atestfile
for i in {1..10}; do
	log_must mkfile 1m /$TESTPOOL/$TESTFS1/$i
	sync
done
log_must zfs snapshot $TESTPOOL/$TESTFS1@snap

# Reduce livelist size to trigger condense more easily
mdb_ctf_set_int zfs_livelist_max_entries 0x14

# The maximum number of iterations each test will attempt before failure.
max_iter=15

# Trigger cancellation path in the zthr
mdb_ctf_set_int zfs_livelist_condense_zthr_delay 0x28
mdb_ctf_set_int zfs_livelist_condense_sync_delay 0
disable_race "zfs_livelist_condense_cancel_zthr"
delete_race "zfs_livelist_condense_cancel_zthr"
export_race "zfs_livelist_condense_cancel_zthr"

# Trigger cancellation path in the synctask
mdb_ctf_set_int zfs_livelist_condense_zthr_delay 0
mdb_ctf_set_int zfs_livelist_condense_sync_delay 0x28
disable_race "zfs_livelist_condense_cancel_sync"
delete_race "zfs_livelist_condense_cancel_sync"

log_pass "Clone livelist condense race conditions passed."
