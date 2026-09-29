// SPDX-License-Identifier: BSD-3-Clause
/* Copyright (c) 2019-2022, Stephan Gerhold <stephan@gerhold.net> */

#include <debug.h>
#include <dev/flash.h>
#include <lib/ptable.h>
#include <partition_parser.h>
#include <string.h>
#include <target.h>

#include <lk2nd/init.h>

static void partition_split_mmc(const char *base_name, const char *name,
				uint32_t num_blocks, bool end)
{
	struct partition_entry *base, *split;
	int index = partition_get_index(base_name);

	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "%s partition not found (as base for %s)\n",
			base_name, name);
		return;
	}
	base = &partition_get_partition_entries()[index];

	if (base->size < num_blocks) {
		dprintf(CRITICAL, "%s partition has not enough space for %s (%llu < %u)\n",
			base_name, name, base->size, num_blocks);
		return;
	}

	split = partition_allocate();
	if (split) {
		memcpy(split, base, sizeof(*split));
		strlcpy((char*)split->name, name, sizeof(split->name));
		if (end)
			split->first_lba = split->last_lba - num_blocks + 1;
		else
			split->last_lba = split->first_lba + num_blocks - 1;
		split->size = num_blocks;
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot add virtual %s partition\n",
			name);
	}

	if (end)
		base->last_lba -= num_blocks;
	else
		base->first_lba += num_blocks;
	base->size -= num_blocks;
}

static void partition_split_flash(struct ptable *ptable, const char *base_name,
				  const char *name, unsigned length, bool end)
{
	struct ptentry *base = ptable_find(ptable, base_name);

	if (!base) {
		dprintf(CRITICAL, "%s partition not found (as base for %s)\n",
			base_name, name);
		return;
	}

	if (base->length < length) {
		dprintf(CRITICAL, "%s partition has not enough space for %s (%u < %u)\n",
			base_name, name, base->length, length);
		return;
	}

	if (ptable_size(ptable) < MAX_PTABLE_PARTS) {
		unsigned start = base->start;
		if (end)
			start += base->length - length;

		ptable_add(ptable, (char*)name, start, length, base->flags,
			   base->type, base->perm);
	} else {
		dprintf(CRITICAL, "Too many partitions, cannot add virtual %s partition\n",
			name);
	}

	if (!end)
		base->start += length;
	base->length -= length;
}

static void lk2nd_partition_split_mmc(void)
{
	uint32_t block_size __UNUSED = mmc_get_device_blocksize();

#ifdef LK2ND_PARTITION_SIZE
	partition_split_mmc(LK2ND_PARTITION_BASE, LK2ND_PARTITION_NAME,
			    LK2ND_PARTITION_SIZE / block_size, false);
#endif
}

static void lk2nd_partition_split_flash(void)
{
	struct ptable *ptable = flash_get_ptable();
	unsigned block_size __UNUSED = flash_block_size();

	if (!ptable)
		return;

#ifdef LK2ND_PARTITION_SIZE
	partition_split_flash(ptable, LK2ND_PARTITION_BASE, LK2ND_PARTITION_NAME,
			      LK2ND_PARTITION_SIZE / block_size, false);
#endif
}

#if defined(LK2ND_RECOVERY_BASE) && defined(LK2ND_PARTITION_BASE)

static struct partition_entry *partition_find(const char *name)
{
	int index = partition_get_index(name);

	if (index == INVALID_PTN) {
		dprintf(CRITICAL, "%s partition not found\n", name);
		return NULL;
	}

	return &partition_get_partition_entries()[index];
}

static void partition_rename(struct partition_entry *ptn, const char *name)
{
	dprintf(SPEW, "Renaming partition %s to %s\n", ptn->name, name);
	strlcpy((char *)ptn->name, name, sizeof(ptn->name));
}

/**
 * lk2nd_partition_move_recovery_mmc() - Move the Android recovery image away
 * from the partition lk2nd is installed into.
 */
static void lk2nd_partition_move_recovery_mmc(void)
{
	uint32_t block_size = mmc_get_device_blocksize();
	unsigned long long num_blocks = LK2ND_RECOVERY_OFFSET / block_size;
	struct partition_entry *android, *boot;

	if (!strcmp(LK2ND_PARTITION_BASE, LK2ND_RECOVERY_BASE)) {
		dprintf(CRITICAL, "Cannot move recovery into the lk2nd partition\n");
		return;
	}

	android = partition_find(LK2ND_RECOVERY_BASE);
	boot = partition_find(LK2ND_PARTITION_BASE);
	if (!android || !boot)
		return;

#ifdef LK2ND_PARTITION_NAME
	/* Make sure lk2nd was really split out of its partition */
	if (!partition_find(LK2ND_PARTITION_NAME))
		return;
#endif

	if (android->size <= num_blocks) {
		dprintf(CRITICAL, "%s partition has not enough space for recovery (%llu <= %llu)\n",
			LK2ND_RECOVERY_BASE, android->size, num_blocks);
		return;
	}

	/*
	 * The Android boot image stays at the beginning of the partition,
	 * rename it to free up the "boot" name for the image lk2nd boots.
	 */
	partition_rename(android, LK2ND_ANDROID_BOOT_NAME);

	/* Everything after lk2nd in its own partition becomes "boot". */
	partition_rename(boot, "boot");

	/* The rest of the original boot partition holds the recovery image. */
	partition_split_mmc(LK2ND_ANDROID_BOOT_NAME, "recovery",
			    android->size - num_blocks, true);

	dprintf(INFO, "lk2nd: boot: %s+%u, recovery: %s+%llu, %s: %s+0\n",
		LK2ND_PARTITION_BASE, LK2ND_PARTITION_SIZE,
		LK2ND_RECOVERY_BASE, (unsigned long long)LK2ND_RECOVERY_OFFSET,
		LK2ND_ANDROID_BOOT_NAME, LK2ND_RECOVERY_BASE);
}

static void lk2nd_partition_move_recovery_flash(void)
{
	dprintf(CRITICAL, "Moving the recovery partition is only supported on eMMC\n");
}

#endif /* LK2ND_RECOVERY_BASE */

static void lk2nd_device2nd_partition_split(void)
{
	if (target_is_emmc_boot()) {
		lk2nd_partition_split_mmc();
#if defined(LK2ND_RECOVERY_BASE) && defined(LK2ND_PARTITION_BASE)
		lk2nd_partition_move_recovery_mmc();
#endif
	} else {
		lk2nd_partition_split_flash();
#if defined(LK2ND_RECOVERY_BASE) && defined(LK2ND_PARTITION_BASE)
		lk2nd_partition_move_recovery_flash();
#endif
	}
}
LK2ND_INIT(lk2nd_device2nd_partition_split);
