/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License Version 1.0 (CDDL-1.0).
 * You can obtain a copy of the license from the top-level file
 * "OPENSOLARIS.LICENSE" or at <http://opensource.org/licenses/CDDL-1.0>.
 * You may not use this file except in compliance with the license.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016, Intel Corporation.
 */

#ifdef HAVE_LIBUDEV

#include <errno.h>
#include <fcntl.h>
#include <libnvpair.h>
#include <libudev.h>
#include <pthread.h>
#include <stdlib.h>

#include <sys/sysevent/eventdefs.h>
#include <sys/sysevent/dev.h>

#include "zed_log.h"
#include "zed_disk_event.h"
#include "agents/zfs_agents.h"

/*
 * Portions of ZED need to see disk events for disks belonging to ZFS pools.
 * A libudev monitor is established to monitor block device actions and pass
 * them on to internal ZED logic modules.  Initially, zfs_mod.c is the only
 * consumer and is the Linux equivalent for the illumos syseventd ZFS SLM
 * module responsible for handeling disk events for ZFS.
 */

pthread_t g_mon_tid;
struct udev *g_udev;
struct udev_monitor *g_mon;


#define	DEV_BYID_PATH	"/dev/disk/by-id/"

/* 64MB is minimum usable disk for ZFS */
#define	MINIMUM_SECTORS		131072


/*
 * Post disk event to SLM module
 *
 * occurs in the context of monitor thread
 */
static void
zed_udev_event(const char *class, const char *subclass, nvlist_t *nvl)
{
	char *strval;
	uint64_t numval;

	zed_log_msg(LOG_INFO, "zed_disk_event:");
	zed_log_msg(LOG_INFO, "\tclass: %s", class);
	zed_log_msg(LOG_INFO, "\tsubclass: %s", subclass);
	if (nvlist_lookup_string(nvl, DEV_NAME, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_NAME, strval);
	if (nvlist_lookup_string(nvl, DEV_PATH, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_PATH, strval);
	if (nvlist_lookup_string(nvl, DEV_IDENTIFIER, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_IDENTIFIER, strval);
	if (nvlist_lookup_string(nvl, DEV_PHYS_PATH, &strval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %s", DEV_PHYS_PATH, strval);
	if (nvlist_lookup_uint64(nvl, DEV_SIZE, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", DEV_SIZE, numval);
	if (nvlist_lookup_uint64(nvl, ZFS_EV_POOL_GUID, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", ZFS_EV_POOL_GUID, numval);
	if (nvlist_lookup_uint64(nvl, ZFS_EV_VDEV_GUID, &numval) == 0)
		zed_log_msg(LOG_INFO, "\t%s: %llu", ZFS_EV_VDEV_GUID, numval);

	(void) zfs_slm_event(class, subclass, nvl);
}

/*
 * Get the persistent device id string (describes "what")
 *
 * used by auto-{online,expand,replace}
 */
static int
zfs_device_get_devid(struct udev_device *dev, char *bufptr, size_t buflen)
{
	struct udev_list_entry *entry;
	const char *bus;
	char devbyid[64];

	bus = udev_device_get_property_value(dev, "ID_BUS");
	if (bus == NULL)
		return (ENODATA);

	/*
	 * locate the bus specific by-id link
	 */
	(void) snprintf(devbyid, sizeof (devbyid), "%s%s-", DEV_BYID_PATH, bus);
	entry = udev_device_get_devlinks_list_entry(dev);
	while (entry != NULL) {
		const char *name;

		name = udev_list_entry_get_name(entry);
		if (strncmp(name, devbyid, strlen(devbyid)) == 0) {
			name += strlen(DEV_BYID_PATH);
			strncpy(bufptr, name, buflen);
			return (0);
		}
		entry = udev_list_entry_get_next(entry);
	}

	return (ENODATA);
}

/*
 * Get the persistent physical location string (describes "where")
 *
 * used by auto-{online,expand,replace}
 */
static int
zfs_device_get_physical(struct udev_device *dev, char *bufptr, size_t buflen)
{
	const char *physpath;

	physpath = udev_device_get_property_value(dev, "ID_PATH");
	if (physpath != NULL) {
		(void) strncpy(bufptr, physpath, buflen);
		return (0);
	}

	return (ENODATA);
}

/*
 * dev_event_nvlist: place event schema into an nv pair list
 *
 * NAME			VALUE (example)
 * --------------	--------------------------------------------------------
 * DEV_NAME		/dev/sdl
 * DEV_PATH		/devices/pci0000:00/0000:00:03.0/0000:04:00.0/host0/...
 * DEV_IDENTIFIER	ata-Hitachi_HTS725050A9A362_100601PCG420VLJ37DMC
 * DEV_PHYS_PATH	pci-0000:04:00.0-sas-0x4433221101000000-lun-0
 * DEV_IS_PART		---
 * DEV_SIZE		500107862016
 * ZFS_EV_POOL_GUID	17523635698032189180
 * ZFS_EV_VDEV_GUID	14663607734290803088
 */
static nvlist_t *
dev_event_nvlist(struct udev_device *dev)
{
	nvlist_t *nvl;
	char strval[128];
	const char *value, *path;
	uint64_t guid;

	if (nvlist_alloc(&nvl, NV_UNIQUE_NAME, 0) != 0)
		return (NULL);

	if (zfs_device_get_devid(dev, strval, sizeof (strval)) == 0)
		(void) nvlist_add_string(nvl, DEV_IDENTIFIER, strval);
	if (zfs_device_get_physical(dev, strval, sizeof (strval)) == 0)
		(void) nvlist_add_string(nvl, DEV_PHYS_PATH, strval);
	if ((path = udev_device_get_devnode(dev)) != NULL)
		(void) nvlist_add_string(nvl, DEV_NAME, path);
	if ((value = udev_device_get_devpath(dev)) != NULL)
		(void) nvlist_add_string(nvl, DEV_PATH, value);
	if ((value = udev_device_get_devtype(dev)) != NULL) {
		if (strcmp("partition", value) == 0)
			(void) nvlist_add_boolean(nvl, DEV_IS_PART);
	}

	if ((value = udev_device_get_sysattr_value(dev, "size")) != NULL) {
		uint64_t numval = DEV_BSIZE;

		numval *= strtoull(value, NULL, 10);
		(void) nvlist_add_uint64(nvl, DEV_SIZE, numval);
	}

	/*
	 * Grab the pool and vdev guids from blkid cache
	 */
	value = udev_device_get_property_value(dev, "ID_FS_UUID");
	if (value != NULL && (guid = strtoull(value, NULL, 10)) != 0)
		(void) nvlist_add_uint64(nvl, ZFS_EV_POOL_GUID, guid);

	value = udev_device_get_property_value(dev, "ID_FS_UUID_SUB");
	if (value != NULL && (guid = strtoull(value, NULL, 10)) != 0)
		(void) nvlist_add_uint64(nvl, ZFS_EV_VDEV_GUID, guid);

	/*
	 * Either a vdev guid or a devid must be present for matching
	 */
	if (!nvlist_exists(nvl, DEV_IDENTIFIER) &&
	    !nvlist_exists(nvl, ZFS_EV_VDEV_GUID)) {
		nvlist_free(nvl);
		return (NULL);
	}

	return (nvl);
}

/*
 *  Listen for block device uevents
 */
static void *
zed_udev_monitor(void *arg)
{
	struct udev_monitor *mon = arg;

	zed_log_msg(LOG_INFO, "Waiting for new uduev disk events...");

	while (1) {
		struct udev_device *dev;
		const char *action, *bus, *type, *part, *size;
		const char *class, *subclass;
		nvlist_t *nvl;
		boolean_t is_zfs = B_FALSE;

		/* allow a cancellation while blocked (recvmsg) */
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

		/* blocks at recvmsg until an event occurs */
		if ((dev = udev_monitor_receive_device(mon)) == NULL) {
			zed_log_msg(LOG_WARNING, "zed_udev_monitor: receive "
			    "device error %d", errno);
			continue;
		}

		/* allow all steps to complete before a cancellation */
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);

		/*
		 * Strongly typed device is the prefered filter
		 */
		type = udev_device_get_property_value(dev, "ID_FS_TYPE");
		if (type != NULL) {
			if (strcmp(type, "zfs_member") == 0) {
				is_zfs = B_TRUE;
			} else {
				/* not ours, so skip */
				zed_log_msg(LOG_INFO, "zed_udev_monitor: skip "
				    "%s (in use by %s)",
				    udev_device_get_devnode(dev), type);
				udev_device_unref(dev);
				continue;
			}
		}

		/*
		 * if this is a disk and it is partitioned, then the
		 * zfs label will reside in a DEVTYPE=partition and
		 * we can skip passing this event
		 */
		type = udev_device_get_property_value(dev, "DEVTYPE");
		part = udev_device_get_property_value(dev,
		    "ID_PART_TABLE_TYPE");
		if (type != NULL && strcmp(type, "disk") == 0 && part != NULL) {
			/* skip and wait for partition event */
			udev_device_unref(dev);
			continue;
		}

		/*
		 * ignore small partitions
		 */
		size = udev_device_get_property_value(dev,
		    "ID_PART_ENTRY_SIZE");
		if (size != NULL &&
		    strtoull(size, NULL, 10) < MINIMUM_SECTORS) {
			udev_device_unref(dev);
			continue;
		}

		/*
		 * if blkid probe didn't find ZFS, we'll need a devid
		 */
		bus = udev_device_get_property_value(dev, "ID_BUS");
		if (!is_zfs && bus == NULL) {
			zed_log_msg(LOG_INFO, "zed_udev_monitor: %s no bus key",
			    udev_device_get_devnode(dev));
			udev_device_unref(dev);
			continue;
		}

		action = udev_device_get_action(dev);
		if (strcmp(action, "add") == 0) {
			class = EC_DEV_ADD;
			subclass = ESC_DISK;
		} else if (strcmp(action, "remove") == 0) {
			class = EC_DEV_REMOVE;
			subclass = ESC_DISK;
		} else if (strcmp(action, "change") == 0) {
			class = EC_DEV_STATUS;
			subclass = ESC_DEV_DLE;
		} else {
			zed_log_msg(LOG_WARNING, "zed_udev_monitor: %s unknown",
			    action);
			udev_device_unref(dev);
			continue;
		}

		if ((nvl = dev_event_nvlist(dev)) != NULL) {
			zed_udev_event(class, subclass, nvl);
			nvlist_free(nvl);
		}

		udev_device_unref(dev);
	}

	return (NULL);
}

int
zed_disk_event_init()
{
	int fd, fflags;

	if ((g_udev = udev_new()) == NULL) {
		zed_log_msg(LOG_WARNING, "udev_new failed (%d)", errno);
		return (-1);
	}

	/* Set up a udev monitor for block devices */
	g_mon = udev_monitor_new_from_netlink(g_udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(g_mon, "block", "disk");
	udev_monitor_filter_add_match_subsystem_devtype(g_mon, "block",
	    "partition");
	udev_monitor_enable_receiving(g_mon);

	/* Make sure monitoring socket is blocking */
	fd = udev_monitor_get_fd(g_mon);
	if ((fflags = fcntl(fd, F_GETFL)) & O_NONBLOCK)
		(void) fcntl(fd, F_SETFL, fflags & ~O_NONBLOCK);

	/* spawn a thread to monitor events */
	if (pthread_create(&g_mon_tid, NULL, zed_udev_monitor, g_mon) != 0) {
		udev_monitor_unref(g_mon);
		udev_unref(g_udev);
		zed_log_msg(LOG_WARNING, "pthread_create failed");
		return (-1);
	}

	zed_log_msg(LOG_INFO, "zed_disk_event_init");

	return (0);
}

void
zed_disk_event_fini()
{
	/* cancel monitor thread at recvmsg() */
	(void) pthread_cancel(g_mon_tid);
	(void) pthread_join(g_mon_tid, NULL);

	/* cleanup udev resources */
	udev_monitor_unref(g_mon);
	udev_unref(g_udev);

	zed_log_msg(LOG_INFO, "zed_disk_event_fini");
}

#else

#include "zed_disk_event.h"

int
zed_disk_event_init()
{
	return (0);
}

void
zed_disk_event_fini()
{
}

#endif /* HAVE_LIBUDEV */
