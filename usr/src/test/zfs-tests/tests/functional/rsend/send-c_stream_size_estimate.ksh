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
# Copyright (c) 2015, 2016 by Delphix. All rights reserved.
#

. $STF_SUITE/tests/functional/rsend/rsend.kshlib

#
# Description:
# Verify the stream size estimate given by -P accounts for compressed send.
# Verify the stream size given by -P accounts for compressed send."
#
# Strategy:
# 1. For datasets of varied compression types do the following:
# 2. Write data, verify stream size estimates with and without -c
#

verify_runnable "both"
typeset compress_types="off gzip lz4"
typeset send_ds="$POOL2/testfs"
typeset send_vol="$POOL2/vol"
typeset send_voldev="/dev/zvol/rdsk/$POOL2/vol"
typeset file="$BACKDIR/file.0"
typeset megs="16"
typeset compress

function get_estimated_size
{
	typeset cmd=$1
	typeset ds=${cmd##* }
	typeset tmpfile=$(mktemp -p $BACKDIR)

	eval "$cmd >$tmpfile"
	[[ $? -eq 0 ]] || log_fail "get_estimated_size: $cmd"
	typeset size=$(eval "awk '\$2 == \"$ds\" {print \$3}' $tmpfile")
	rm -f $tmpfile

	echo $size
}

log_assert "Verify the stream size given by -P accounts for compressed send."
log_onexit cleanup_pool $POOL2

write_compressible $BACKDIR ${megs}m

for compress in $compress_types; do
	datasetexists $send_ds && log_must zfs destroy -r $send_ds
	datasetexists $send_vol && log_must zfs destroy -r $send_vol
	log_must zfs create -o compress=$compress $send_ds
	log_must zfs create -V 1g -o compress=$compress $send_vol

	typeset dir=$(get_prop mountpoint $send_ds)
	log_must cp $file $dir
	log_must zfs snapshot $send_ds@snap
	log_must dd if=$file of=$send_voldev
	log_must zfs snapshot $send_vol@snap

	typeset ds_size=$(get_estimated_size "zfs send -nP $send_ds@snap")
	typeset ds_lrefer=$(get_prop lrefer $send_ds)
	within_percent $ds_size $ds_lrefer 90 || log_fail \
	    "$ds_size and $ds_lrefer differed by too much"

	typeset vol_size=$(get_estimated_size "zfs send -nP $send_vol@snap")
	typeset vol_lrefer=$(get_prop lrefer $send_vol)
	within_percent $vol_size $vol_lrefer 90 || log_fail \
	    "$vol_size and $vol_lrefer differed by too much"

	# fio can write extremely compressible files here. If that happens,
	# the adjustment made for indirect blocks breaks down due to the small
	# size of the compressed stream. Skip this test in that case to avoid
	# incorrect failures.
	typeset ratio=$(get_prop compressratio $send_ds)
	if [[ ${ratio%%\.*} -lt 20 ]]; then
		typeset ds_csize=$(get_estimated_size "zfs send -nP -c \
		    $send_ds@snap")
		typeset ds_refer=$(get_prop refer $send_ds)
		within_percent $ds_csize $ds_refer 90 || log_fail \
		    "$ds_csize and $ds_refer differed by too much"
	else
		log_note "Skipped $compress compressed dataset test due to" \
		    "a compression ratio of $ratio"
	fi

	typeset vol_csize=$(get_estimated_size "zfs send -nP -c $send_vol@snap")
	typeset vol_refer=$(get_prop refer $send_vol)
	within_percent $vol_csize $vol_refer 90 || log_fail \
	    "$vol_csize and $vol_refer differed by too much"
done

log_pass "The the stream size given by -P accounts for compressed send."
