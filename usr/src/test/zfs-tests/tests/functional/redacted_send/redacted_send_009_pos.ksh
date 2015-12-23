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
# Copyright (c) 2015 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/redacted_send/redacted_send.kshlib

#
# Description:
# Test that receiving redacted sends produces redacted bookmarks,
# and that reads to those bookmarks cause EIO.
#

verify_runnable "both"

log_assert "Verify that redacted sends create redacted blkptrs"

streamfile=$($MKTEMP /tmp/stream.XXXXXX)

function cleanup
{
        $ZFS destroy -R $POOL/fs1
        $ZFS destroy -R $POOL/rfs
        $RM $streamfile
}

log_onexit cleanup

# Set up all the filesystems and clones.
log_must $ZFS create -o recordsize=512 -o checksum=sha256 -o compression=lz4 \
    $POOL/fs1
typeset mntpnt=$(get_prop mountpoint $POOL/fs1)
log_must $DD if=/dev/urandom of=$mntpnt/f1 count=1 bs=512
log_must $DD if=/dev/urandom of=$mntpnt/f2 count=1 bs=512
log_must $ZFS snapshot $POOL/fs1@s
log_must $DD if=/dev/urandom of=$mntpnt/f1 count=1 bs=512
log_must $ZFS snapshot $POOL/fs1@s2
log_must $ZFS snapshot $POOL/fs1@s3

log_must $ZFS clone $POOL/fs1@s $POOL/cl1
typeset mntpnt=$(get_prop mountpoint $POOL/cl1)
log_must $DD if=/dev/urandom of=$mntpnt/f2 count=1 bs=512
log_must $ZFS snapshot $POOL/cl1@s

log_must $ZFS clone $POOL/fs1@s2 $POOL/cl2
typeset mntpnt=$(get_prop mountpoint $POOL/cl2)
log_must $DD if=/dev/urandom of=$mntpnt/f1 count=1 bs=512
log_must $ZFS snapshot $POOL/cl2@s


log_must eval "$ZFS send --redact $POOL/cl1@s $POOL/fs1@s book >$streamfile"
$CAT $streamfile | log_must $ZFS receive $POOL/rfs
log_must echo "zfs_allow_redacted_dataset_mount/W 1" | log_must $MDB -kw
log_must $ZFS mount -f $POOL/rfs
typeset mntpnt=$(get_prop mountpoint $POOL/rfs)
log_must dd if=$mntpnt/f1 of=/dev/null bs=512 count=1
log_mustnot dd if=$mntpnt/f2 of=/dev/null bs=512 count=1
log_must $ZFS umount $POOL/rfs

log_must eval "$ZFS send --redact $POOL/cl2@s -i $POOL/fs1#book $POOL/fs1@s2 \
    book2 >$streamfile"
$CAT $streamfile | log_must $ZFS receive -F $POOL/rfs
log_must $ZFS mount -f $POOL/rfs
typeset mntpnt=$(get_prop mountpoint $POOL/rfs)
log_mustnot dd if=$mntpnt/f1 of=/dev/null bs=512 count=1
log_must dd if=$mntpnt/f2 of=/dev/null bs=512 count=1
log_must $ZFS umount $POOL/rfs

log_must $ZPOOL export $POOL
log_must $ZPOOL import $POOL

log_must eval "$ZFS send -i $POOL/fs1#book2 $POOL/fs1@s3 >$streamfile"
log_must echo "zfs_allow_redacted_dataset_mount/W 0" | log_must $MDB -kw
$CAT $streamfile | log_must $ZFS receive -F $POOL/rfs
log_must $ZFS mount $POOL/rfs
typeset mntpnt=$(get_prop mountpoint $POOL/rfs)
log_must dd if=$mntpnt/f1 of=/dev/null bs=512 count=1
log_must dd if=$mntpnt/f2 of=/dev/null bs=512 count=1

log_must $ZPOOL export $POOL
log_must $ZPOOL import $POOL

log_pass "Redacted sends create redacted blkptrs"
