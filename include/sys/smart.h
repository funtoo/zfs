#ifndef _SYS_SMART_H
#define _SYS_SMART_H

enum smart_type {
	SMART_STATUS,	/* 0 for "OK" or "PASSED", non-zero for bad */
	SMART_TEMP,	/* In Celsius */
	SMART_REALC,	/* SATA: Reallocated sectors */
	SMART_COR,	/* SAS: Corrected errors */
	SMART_UNCOR,	/* SAS: Uncorrected errors */
	SMART_VAL_COUNT, /* Always make last element */
};

typedef struct smart_disk {
	char *dev;
	int64_t val[SMART_VAL_COUNT]; /* -1 for unused */
} smart_disk_t;

extern const char* smart_header_table[SMART_VAL_COUNT];

/* Populate smart_info.  Assumes smart_info[].dev is filled in */
extern int get_smart(smart_disk_t *sd, unsigned int cnt);

#endif
