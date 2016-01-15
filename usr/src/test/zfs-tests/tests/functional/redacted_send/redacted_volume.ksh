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
# Copyright (c) 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/tests/functional/redacted_send/redacted.kshlib

#
# Description:
# Verify that redacted send works on volumes.
#
# Strategy:
# 1. Write to a volume, then make a clone of that volume.
# 2. Receive a redacted stream that sends all blocks.
# 3. Receive a redacted stream that redacts the first half of the written area.
#

typeset ds_name="volume"
typeset sendvol="$POOL/$ds_name"
typeset recvvol="$POOL2/$ds_name"
typeset clone="$POOL/${ds_name}_clone"
typeset tmpdir="$(get_prop mountpoint $POOL)/tmp"
typeset stream=$(mktemp $tmpdir/stream.XXXX)
typeset send_file="/dev/zvol/rdsk/$sendvol"
typeset recv_file="/dev/zvol/rdsk/$recvvol"
typeset clone_file="/dev/zvol/rdsk/$clone"

log_onexit redacted_cleanup $sendvol $recvvol

log_must zfs create -b 8k -V 1g $sendvol
log_must dd if=/dev/urandom of=$send_file bs=8k count=64
log_must zfs snapshot $sendvol@snap
log_must zfs clone $sendvol@snap $clone
log_must zfs snapshot $clone@snap

echo "zfs_allow_redacted_dataset_mount/W 1" | sudo mdb -kw
log_must eval "zfs send --redact $clone@snap $sendvol@snap book1 >$stream"
log_must eval "zfs recv $recvvol <$stream"
log_must dd if=$send_file of=$tmpdir/send.dd bs=8k count=64
log_must dd if=$recv_file of=$tmpdir/recv.dd bs=8k count=64
log_must diff $tmpdir/send.dd $tmpdir/recv.dd
log_must zfs destroy -R $recvvol

log_must dd if=/dev/urandom of=$clone_file bs=8k count=32
log_must zfs snapshot $clone@snap1
log_must eval "zfs send --redact $clone@snap1 $sendvol@snap book2 >$stream"
log_must eval "zfs recv $recvvol <$stream"
for off in {0..31}; do
	log_mustnot dd if=$recv_file of=/dev/null bs=8k seek=$off
done
log_must dd if=$send_file of=$tmpdir/send.dd bs=8k count=32 iseek=32
log_must dd if=$recv_file of=$tmpdir/recv.dd bs=8k count=32 iseek=32
log_must diff $tmpdir/send.dd $tmpdir/recv.dd

log_pass "Redacted send works correctly with volumes."
