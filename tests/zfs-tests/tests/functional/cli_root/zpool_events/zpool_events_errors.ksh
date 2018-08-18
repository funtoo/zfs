#!/bin/ksh -p
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
#	Verify the number of IO and checksum events match the error counters
#	in zpool status.
#
# STRATEGY:
#	1. Create a raidz or mirror pool
#	2. Inject read/write IO errors or checksum errors
#	3. Verify the number of errors in zpool status match the corresponding
#	   number of error events.
#	4. Repeat for all combinations of raidz/mirror and io/checksum errors.
#

. $STF_SUITE/include/libtest.shlib

verify_runnable "both"

VDEVDIR=$(mktemp -d)
MOUNTDIR=$VDEVDIR/mount
VDEV1=$VDEVDIR/file1
VDEV2=$VDEVDIR/file2
VDEV3=$VDEVDIR/file3
POOL=error_pool
POOLTYPE=raidz
FILESIZE=10485760

OLD_CHECKSUMS=$(cat /sys/module/zfs/parameters/zfs_checksums_per_second)
OLD_LEN_MAX=$(cat /sys/module/zfs/parameters/zfs_zevent_len_max)

function cleanup
{
	echo "$OLD_CHECKSUMS" > /sys/module/zfs/parameters/zfs_checksums_per_second
	echo "$OLD_LEN_MAX" > /sys/module/zfs/parameters/zfs_zevent_len_max

	zinject -c all
	zpool events -c

	destroy_pool $POOL
	rm -fr $VDEVDIR
}

log_assert "Check that the number of zpool errors match the number of events"

log_onexit cleanup

echo 999999999 > /sys/module/zfs/parameters/zfs_checksums_per_second
echo 999999999 > /sys/module/zfs/parameters/zfs_zevent_len_max

log_must truncate -s 200M $VDEV1 $VDEV2 $VDEV3
log_must mkdir -p $MOUNTDIR

# Run error test on a specific type of pool
#
# $1: pool - raidz, mirror
# $2: test type - corrupt (checksum error), io
# $3: read, write
function do_test
{
	POOLTYPE=$1
	ERR=$2
	RW=$3

	log_must zpool create -f -m $MOUNTDIR -o failmode=continue $POOL $POOLTYPE $VDEV1 $VDEV2 $VDEV3
	log_must zpool events -c
	log_must zfs set compression=off $POOL

	if [ "$RW" == "read" ] ; then
		log_must mkfile $FILESIZE $MOUNTDIR/file
	fi

	log_must zinject -d $VDEV1 -e $ERR -T $RW -f 90 $POOL

	if [ "$RW" == "write" ] ; then
		log_must mkfile $FILESIZE $MOUNTDIR/file
	else
		scrub_and_wait $POOL
	fi

	log_must zinject -c all

	out="$(zpool status | grep $VDEV1)"

	if [ "$ERR" == "corrupt" ] ; then
		events=$(zpool events | grep checksum | wc -l)
		val=$(echo "$out" | awk '{print $5}')
		str="checksum"
	elif [ "$ERR" == "io" ] ; then
		events=$(zpool events | grep io | wc -l)
		if [ "$RW" == "read" ] ; then
			str="read IO"
			val=$(echo "$out" | awk '{print $3}')
		else
			str="write IO"
			val=$(echo "$out" | awk '{print $4}')
		fi
	fi

	if [ "$val" == "0" ] || [ "$events" == "" ] ; then
		log_fail "Didn't see any errors or events ($val/$events)"
	fi

	if [ "$val" != "$events" ] ; then
		log_fail "$val $POOLTYPE $str errors != $events events"
	else
		log_note "$val $POOLTYPE $str errors == $events events"
	fi

	log_must zpool destroy $POOL
}

# Test all types of errors on mirror and raidz pools
for i in mirror raidz ; do
	do_test $i corrupt read
	do_test $i io read
	do_test $i io write
done

log_pass "The number of errors matched the number of events"
