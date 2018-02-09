#!/bin/ksh
#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#

#
# Copyright (c) 2016, 2018 by Delphix. All rights reserved.
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/tests/functional/cli_root/zfs_copies/zfs_copies.kshlib

#
# DESCRIPTION:
#	Verify that copies cannot be set with pool version 1
#
# STRATEGY:
#	1. Create filesystems with copies set in a pool with version 1
#	2. Verify that the create operations fail
#

verify_runnable "global"

function cleanup
{
	if poolexists $ZPOOL_VERSION_1_NAME; then
		destroy_pool $ZPOOL_VERSION_1_NAME
	fi

	cd $TESTDIR && rm -f ${ZPOOL_VERSION_1_FILES//\.bz2/}
}

log_onexit cleanup

typeset dir=$STF_SUITE/tests/functional/cli_root/zpool_upgrade/blockfiles
for file in $ZPOOL_VERSION_1_FILES; do
	bzcat $dir/$file >$TESTDIR/$file
done
log_must zpool import -d $TESTDIR $ZPOOL_VERSION_1_NAME
log_must zfs create $ZPOOL_VERSION_1_NAME/$TESTFS
log_must zfs create -V 1m $ZPOOL_VERSION_1_NAME/$TESTVOL

for val in 3 2 1; do
	log_mustnot zfs set copies=$val $ZPOOL_VERSION_1_NAME/$TESTFS
	log_mustnot zfs set copies=$val $ZPOOL_VERSION_1_NAME/$TESTVOL

	log_mustnot zfs create -o copies=$val $ZPOOL_VERSION_1_NAME/$TESTFS1
	log_mustnot zfs create -o copies=$val $ZPOOL_VERSION_1_NAME/$TESTVOL1
done

log_pass "Verification pass: copies cannot be set with pool version 1. "
