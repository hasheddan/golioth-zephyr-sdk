/*
 * Copyright (c) 2021 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(golioth_dfu);

#include <stdio.h>
#include <zephyr/storage/flash_map.h>

#include "flash.h"

#include <golioth/compat/init.h>

#ifdef CONFIG_TRUSTED_EXECUTION_NONSECURE
#define SLOT0_LABEL	slot0_ns_partition
#else
#define SLOT0_LABEL	slot0_partition
#endif /* CONFIG_TRUSTED_EXECUTION_NONSECURE */

/* FIXED_PARTITION_ID() values used below are auto-generated by DT */
#define FLASH_AREA_IMAGE_PRIMARY FIXED_PARTITION_ID(SLOT0_LABEL)

char current_version_str[sizeof("255.255.65535")];

static int current_version_init(void)
{
	struct mcuboot_img_header hdr;
	int written;
	int err;

	err = boot_read_bank_header(FLASH_AREA_IMAGE_PRIMARY, &hdr, sizeof(hdr));
	if (err) {
		LOG_ERR("Failed to read primary area (%u) header: %d",
			FLASH_AREA_IMAGE_PRIMARY, err);
		return err;
	}

	written = snprintf(current_version_str, sizeof(current_version_str),
			   "%u.%u.%u",
			   (unsigned int) hdr.h.v1.sem_ver.major,
			   (unsigned int) hdr.h.v1.sem_ver.minor,
			   (unsigned int) hdr.h.v1.sem_ver.revision);
	if (written >= sizeof(current_version_str)) {
		LOG_ERR("Version string is too long!");
		current_version_str[0] = '\0';
		return -ENOMEM;
	}

	return 0;
}

SYS_INIT(current_version_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/*
 * @note This is a copy of ERASED_VAL_32() from mcumgr.
 */
#define ERASED_VAL_32(x) (((x) << 24) | ((x) << 16) | ((x) << 8) | (x))

/**
 * Determines if the specified area of flash is completely unwritten.
 *
 * @note This is a copy of zephyr_img_mgmt_flash_check_empty() from mcumgr.
 */
static int flash_area_check_empty(const struct flash_area *fa,
				  bool *out_empty)
{
	uint32_t data[16];
	off_t addr;
	off_t end;
	int bytes_to_read;
	int rc;
	int i;
	uint8_t erased_val;
	uint32_t erased_val_32;

	__ASSERT_NO_MSG(fa->fa_size % 4 == 0);

	erased_val = flash_area_erased_val(fa);
	erased_val_32 = ERASED_VAL_32(erased_val);

	end = fa->fa_size;
	for (addr = 0; addr < end; addr += sizeof(data)) {
		if (end - addr < sizeof(data)) {
			bytes_to_read = end - addr;
		} else {
			bytes_to_read = sizeof(data);
		}

		rc = flash_area_read(fa, addr, data, bytes_to_read);
		if (rc != 0) {
			flash_area_close(fa);
			return rc;
		}

		for (i = 0; i < bytes_to_read / 4; i++) {
			if (data[i] != erased_val_32) {
				*out_empty = false;
				flash_area_close(fa);
				return 0;
			}
		}
	}

	*out_empty = true;

	return 0;
}

static int flash_img_erase_if_needed(struct flash_img_context *ctx)
{
	bool empty;
	int err;

	if (IS_ENABLED(CONFIG_IMG_ERASE_PROGRESSIVELY)) {
		return 0;
	}

	err = flash_area_check_empty(ctx->flash_area, &empty);
	if (err) {
		return err;
	}

	if (empty) {
		return 0;
	}

	err = flash_area_erase(ctx->flash_area, 0, ctx->flash_area->fa_size);
	if (err) {
		return err;
	}

	return 0;
}

static const char *swap_type_str(int swap_type)
{
	switch (swap_type) {
	case BOOT_SWAP_TYPE_NONE:
		return "none";
	case BOOT_SWAP_TYPE_TEST:
		return "test";
	case BOOT_SWAP_TYPE_PERM:
		return "perm";
	case BOOT_SWAP_TYPE_REVERT:
		return "revert";
	case BOOT_SWAP_TYPE_FAIL:
		return "fail";
	}

	return "unknown";
}

int flash_img_prepare(struct flash_img_context *flash)
{
	int swap_type;
	int err;

	swap_type = mcuboot_swap_type();
	switch (swap_type) {
	case BOOT_SWAP_TYPE_REVERT:
		LOG_WRN("'revert' swap type detected, it is not safe to continue");
		return -EBUSY;
	default:
		LOG_INF("swap type: %s", swap_type_str(swap_type));
		break;
	}

	err = flash_img_init(flash);
	if (err) {
		LOG_ERR("failed to init: %d", err);
		return err;
	}

	err = flash_img_erase_if_needed(flash);
	if (err) {
		LOG_ERR("failed to erase: %d", err);
		return err;
	}

	return 0;
}
