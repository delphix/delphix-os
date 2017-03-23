#!/usr/bin/ksh

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
# Copyright (c) 2017 by Delphix. All rights reserved.
#

. $STF_SUITE/tests/functional/redacted_send/redacted.kshlib

#
# Description:
# Verify the list of redacted snapshot guids as properties.
#
# Strategy:
# 1. Create a redacted dataset and receive it into another pool.
# 2. Verify the received redaction list matches the sender's
#    redact_snaps property.
#

typeset ds_name="props"
typeset sendfs="$POOL/$ds_name"
typeset recvfs="$POOL2/$ds_name"
typeset clone="$POOL/${ds_name}_clone"
typeset tmpdir="$(get_prop mountpoint $POOL)/tmp"
typeset stream=$(mktemp $tmpdir/stream.XXXX)
setup_dataset $ds_name ''
typeset mntpnt redaction_list

log_onexit redacted_cleanup $sendfs $recvfs

# Verify a plain dataset, snapshot or bookmark has an empty list.
log_must zfs snapshot $sendfs@empty_snapshot
log_must zfs bookmark $sendfs@empty_snapshot $sendfs#empty_bookmark
found_list=$(get_prop redact_snaps $sendfs)
[[ $found_list = "-" ]] || log_fail "Unexpected dataset list: $found_list"
found_list=$(get_prop redact_snaps $sendfs@empty_snapshot)
[[ $found_list = "-" ]] || log_fail "Unexpected snapshot list: $found_list"
found_list=$(get_prop redact_snaps $sendfs#empty_bookmark)
[[ $found_list = "-" ]] || log_fail "Unexpected bookmark list: $found_list"

# Fill in a different block in every clone.
for i in {1..16}; do
	log_must zfs clone $sendfs@snap ${clone}$i
	mntpnt=$(get_prop mountpoint ${clone}$i)
	log_must dd if=/dev/urandom of=$mntpnt/f2 bs=64k count=1 seek=$i \
	    conv=notrunc
	log_must zfs snapshot ${clone}$i@snap
done

redaction_list=$(echo $clone{1..16}@snap | sed 's/ /,/g')
log_must eval "zfs send --redact $redaction_list $sendfs@snap book1 >$stream"
log_must eval "zfs recv $recvfs <$stream"

get_guid_list $tmpdir/prop_list $sendfs#book1
get_guid_list $tmpdir/recvd_prop_list $recvfs@snap

count=$(wc -l $tmpdir/prop_list | awk '{print $1}')
[[ $count -eq 16 ]] || log_fail "Found incorrect number of redaction snapshots."

diff $tmpdir/prop_list $tmpdir/recvd_prop_list || \
    log_fail "Received property list differed from sent"

log_pass "The redaction list is consistent between sent and received datasets."
