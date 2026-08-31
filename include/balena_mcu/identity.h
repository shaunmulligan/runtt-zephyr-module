/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * Per-board identity, written once at provisioning.
 *
 * THE POINT OF THIS FILE IS THAT THE FIRMWARE IMAGE IS NOT BOARD-SPECIFIC.
 *
 * A CAN node id was originally a Kconfig symbol, which meant every board on a
 * bus needed its own build. That is not merely inconvenient to distribute: the
 * firmware ships as an OCI image, so a per-board setting makes the *service
 * image* per-board, and a fleet of N boards becomes N images in the registry
 * with deltas computed against the wrong baselines. Identity has to live outside
 * the image.
 *
 * So it lives in `storage_partition`, which every board we support already
 * declares upstream, and which sits OUTSIDE both MCUboot slots. That placement
 * is the useful part: a firmware update swaps slots and cannot touch identity,
 * so a board cannot lose its address by being updated.
 *
 * The record is not code and is deliberately not covered by MCUboot's signature.
 * Nothing here is trusted for anything but addressing, and an attacker able to
 * write flash already has better options than renumbering a CAN node.
 *
 * A board with no valid record falls back to the Kconfig defaults. That is what
 * keeps a plain `west build` and native_sim working with nothing provisioned.
 */

#ifndef BALENA_MCU_IDENTITY_H_
#define BALENA_MCU_IDENTITY_H_

#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "blna", little-endian, at the start of a valid record. */
#define BALENA_MCU_IDENTITY_MAGIC 0x616e6c62U

/** The only record version this firmware understands. */
#define BALENA_MCU_IDENTITY_VERSION 1U

/** Length of the serial field, including any NUL padding. */
#define BALENA_MCU_IDENTITY_SERIAL_LEN 16

/** `can_node_id` when the record does not assign one. */
#define BALENA_MCU_IDENTITY_NO_NODE_ID 0xffffU

/*
 * Exactly 32 bytes, little-endian, no implicit padding.
 *
 * The layout is contractual: docs/WIRE_CONTRACT.md carries it, and the host-side
 * writer in scripts/ builds the same bytes. Extend only by claiming reserved
 * space and bumping the version -- never by reordering.
 */
struct balena_mcu_identity {
	uint32_t magic;
	uint8_t version;
	uint8_t _pad[3];
	/** Base CAN id; the node also owns this + 1 and + 2. */
	uint16_t can_node_id;
	uint16_t _reserved;
	/** NUL-padded ASCII. All-zero means "no serial assigned". */
	uint8_t serial[BALENA_MCU_IDENTITY_SERIAL_LEN];
	/** CRC32-IEEE over the preceding 28 bytes. */
	uint32_t crc;
} __packed;

BUILD_ASSERT(sizeof(struct balena_mcu_identity) == 32,
	     "the identity record layout is contractual and must stay 32 bytes");

/**
 * @brief The CAN node id this board should use.
 *
 * The provisioned value when a valid record assigns one, otherwise
 * CONFIG_BALENA_MCU_CAN_NODE_ID.
 */
uint16_t balena_mcu_identity_can_node_id(void);

/**
 * @brief This board's serial, or NULL when none is assigned.
 *
 * Points at static storage owned by the module; the caller must not free it.
 */
const char *balena_mcu_identity_serial(void);

/**
 * @brief Whether a valid record was found in flash.
 *
 * Distinguishes "provisioned, and happens to use the default id" from "never
 * provisioned", which are different operational states.
 */
bool balena_mcu_identity_is_provisioned(void);

/**
 * @brief Whether a record is present but unusable.
 *
 * ABSENT AND DAMAGED ARE NOT THE SAME THING, and the difference decides whether
 * falling back to the built-in default is safe.
 *
 * Absent is the factory state: a fresh board must fall back, or it could never
 * be talked to at all and the idle app could not answer `describe`.
 *
 * Damaged means someone assigned this board an address and we cannot read it.
 * Falling back would put it on the default id -- where a correctly provisioned
 * neighbour may already be answering. Two ISO-TP responders on one identifier is
 * a confusing failure that damages a working board, so a CAN transport refuses
 * to start instead. One board missing is a far better symptom than two boards
 * fighting, and recovery is the SWD path provisioning already uses.
 */
bool balena_mcu_identity_is_corrupt(void);

#ifdef __cplusplus
}
#endif

#endif /* BALENA_MCU_IDENTITY_H_ */
