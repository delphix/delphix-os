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
# Test that redacted send correctly detects invalid arguments.
#

typeset sendfs="$POOL2/sendfs"
typeset recvfs="$POOL2/recvfs"
typeset clone1="$POOL2/clone1"
typeset clone2="$POOL2/clone2"
typeset clone3="$POOL2/clone3"
typeset tmpdir="$(get_prop mountpoint $POOL)/tmp"
typeset stream=$(mktemp $tmpdir/stream.XXXX)

log_onexit redacted_cleanup $sendfs $recvfs $clone3

log_must zfs create $sendfs
log_must zfs snapshot $sendfs@snap1
log_must zfs snapshot $sendfs@snap2
log_must zfs snapshot $sendfs@snap3
log_must zfs clone $sendfs@snap2 $clone1
log_must zfs snapshot $clone1@snap
log_must zfs bookmark $clone1@snap $clone1#book
log_must zfs clone $sendfs@snap2 $clone2
log_must zfs snapshot $clone2@snap

# Incompatible flags
typeset flag
for flag in -D -n -p -P -R -v; do
	log_mustnot eval "zfs send $flag --redact \"\" $sendfs@snap1 \
	    book >/dev/null"
done

# Bad bookmark arguments
typeset arg
for arg in "$sendfs@snap2" "$sendfs" "$clone1#book"; do
	log_mustnot eval "zfs send --redact $clone1@snap $arg >/dev/null"
done

# Bad redaction list arguments
log_mustnot eval "zfs send --redact $sendfs@snap2 book >/dev/null"
log_mustnot eval "zfs send --redact $clone1@snap,$clone2 \
    $sendfs@snap2 book >/dev/null"
log_mustnot eval "zfs send --redact $clone1@snap,$clone2@snap, \
    $sendfs@snap2 book >/dev/null"
for arg in "$sendfs" "$clone1@none" "$clone1#none" "$clone1#book"; do
	log_mustnot eval "zfs send --redact $arg $sendfs@snap2 book >/dev/null"
done

# Redaction snapshots not a descendant of tosnap
log_mustnot eval "zfs send --redact $sendfs@snap2 $sendfs@snap2 book >/dev/null"
log_must eval "zfs send --redact \"\" $sendfs@snap2 book1 >/dev/null"
log_must eval "zfs send --redact $clone1@snap,$clone2@snap \
    $sendfs@snap2 book2 >$stream"
log_must eval "zfs send -i $sendfs@snap1 --redact \
    $clone1@snap,$clone2@snap $sendfs@snap2 book3 >/dev/null"
log_mustnot eval "zfs send --redact $clone1@snap -i $POOL2/test#book2 \
    $sendfs@snap3 book4 > /dev/null"

# Full redacted sends of redacted datasets are not allowed.
log_must eval "zfs recv $recvfs <$stream"
log_must zfs snapshot $recvfs@snap
log_must zfs clone $recvfs@snap $clone3
log_must zfs snapshot $clone3@snap
log_mustnot eval "zfs send --redact \"\" $recvfs@snap book5 >/dev/null"
log_mustnot eval "zfs send --redact \"$clone3@snap\" $recvfs@snap \
    book6 >/dev/null"
# Nor may a redacted dataset appear in the redaction list.
log_mustnot eval "zfs send --redact testpool2/recvfs@snap \
    testpool2/recvfs@snap2 book7 >/dev/null"

log_pass "Verify that redacted send correctly detects invalid arguments."
