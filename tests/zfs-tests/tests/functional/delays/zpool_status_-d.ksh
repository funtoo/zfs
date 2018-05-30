#!/bin/ksh -p
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
# Copyright (c) 2018 by Lawrence Livermore National Security, LLC.
#

# DESCRIPTION:
#	Verify zpool status -d (delays) works
#
# STRATEGY:
#	1. Create a file
#	2. Inject delays into the pool
#	3. Verify we can see the delays with "zpool status -d".
#

. $STF_SUITE/include/libtest.shlib
. $STF_SUITE/include/zpool_script.shlib

DISK=${DISKS%% *}

verify_runnable "both"

function cleanup
{
	zinject -c all
}

log_onexit cleanup

# Mark any IOs greater than 10ms as delays
OLD_DELAY=$(cat /sys/module/zfs/parameters/zio_delay_max)
echo 10 > /sys/module/zfs/parameters/zio_delay_max

# Create 20ms IOs
log_must zinject -d $DISK -D20:100 $TESTPOOL
log_must mkfile 409600 $TESTDIR/testfile
log_must zpool sync $TESTPOOL
log_must zinject -c all
echo $OLD_DELAY > /sys/module/zfs/parameters/zio_delay_max

DELAYS=$(zpool status -dp | grep "$DISK" | awk '{print $6}')
if [ "$DELAYS" -gt "0" ] ; then
	log_pass "Correctly saw $DELAYS delays"
else
	log_fail "No delays seen"
fi
