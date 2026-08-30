/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reading the per-board identity record. See include/balena_mcu/identity.h for
 * why it exists and why it lives where it does.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>

#include <balena_mcu/identity.h>

LOG_MODULE_REGISTER(balena_mcu_identity, CONFIG_BALENA_MCU_LOG_LEVEL);

/* Not every board declares a storage partition. Without one there is nowhere to
 * put a record, so the module compiles down to the built-in defaults rather than
 * failing the build -- identity is optional, and a board without it still works.
 */
#define HAVE_STORAGE FIXED_PARTITION_EXISTS(storage_partition)

#if HAVE_STORAGE
#define STORAGE_ID FIXED_PARTITION_ID(storage_partition)
#endif

/* Everything but the trailing CRC. */
#define CRC_COVERED_LEN (sizeof(struct balena_mcu_identity) - sizeof(uint32_t))

static struct balena_mcu_identity record;
static bool provisioned;
/* A record that is PRESENT but unusable. Kept apart from "absent" because the
 * two warrant opposite responses -- see balena_mcu_identity_is_corrupt().
 */
static bool corrupt;
/* One byte longer than the field so an unterminated serial still prints. */
static char serial_str[BALENA_MCU_IDENTITY_SERIAL_LEN + 1];

static bool record_is_valid(const struct balena_mcu_identity *r)
{
	if (r->magic != BALENA_MCU_IDENTITY_MAGIC) {
		/* Erased flash reads as 0xff, so an unprovisioned board lands
		 * here. Not a warning: it is the factory state.
		 */
		return false;
	}

	if (r->version != BALENA_MCU_IDENTITY_VERSION) {
		LOG_ERR("identity record version %u is not %u", r->version,
			BALENA_MCU_IDENTITY_VERSION);
		corrupt = true;
		return false;
	}

	uint32_t want = crc32_ieee((const uint8_t *)r, CRC_COVERED_LEN);

	if (r->crc != want) {
		/* A torn write during provisioning, or a partition reused for
		 * something else.
		 */
		LOG_ERR("identity record CRC is %08x, expected %08x", r->crc, want);
		corrupt = true;
		return false;
	}

	return true;
}

#if HAVE_STORAGE
static int identity_init(void)
{
	const struct flash_area *fa;
	int rc = flash_area_open(STORAGE_ID, &fa);

	if (rc != 0) {
		LOG_WRN("no storage partition (%d); using built-in defaults", rc);
		return 0;
	}

	rc = flash_area_read(fa, 0, &record, sizeof(record));
	flash_area_close(fa);

	if (rc != 0) {
		LOG_WRN("could not read the identity record (%d); using built-in defaults", rc);
		return 0;
	}

	if (!record_is_valid(&record)) {
		return 0;
	}

	provisioned = true;

	memcpy(serial_str, record.serial, sizeof(record.serial));
	serial_str[sizeof(record.serial)] = '\0';

	LOG_INF("provisioned: can node id %#x, serial \"%s\"", record.can_node_id,
		serial_str[0] != '\0' ? serial_str : "(none)");

	return 0;
}

/* Before anything that consumes identity. The CAN transport reads the node id
 * when it binds, so this has to have run by then.
 */
SYS_INIT(identity_init, APPLICATION, CONFIG_BALENA_MCU_IDENTITY_INIT_PRIORITY);
#else
BUILD_ASSERT(true, "no storage_partition on this board; identity uses defaults");
#endif /* HAVE_STORAGE */

uint16_t balena_mcu_identity_can_node_id(void)
{
	if (provisioned && record.can_node_id != BALENA_MCU_IDENTITY_NO_NODE_ID) {
		return record.can_node_id;
	}

#ifdef CONFIG_BALENA_MCU_CAN_NODE_ID
	return (uint16_t)CONFIG_BALENA_MCU_CAN_NODE_ID;
#else
	/* Identity can be enabled without the CAN transport, in which case there
	 * is no Kconfig default to fall back to.
	 */
	return BALENA_MCU_IDENTITY_NO_NODE_ID;
#endif
}

const char *balena_mcu_identity_serial(void)
{
	if (!provisioned || serial_str[0] == '\0') {
		return NULL;
	}

	return serial_str;
}

bool balena_mcu_identity_is_provisioned(void)
{
	return provisioned;
}

bool balena_mcu_identity_is_corrupt(void)
{
	return corrupt;
}
