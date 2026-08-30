/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 *
 * The `describe` command: a custom SMP group the host queries to learn what it
 * is talking to.
 *
 * This is what turns version skew into a clear error instead of a timeout. The
 * runtime and the firmware ship from different parties, so the host needs to
 * establish the contract version before it starts pushing images at a board.
 */
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_encode.h>

#ifdef CONFIG_BALENA_MCU_HEALTH
#include <balena_mcu/health.h>
#endif

/*
 * Zephyr generates app_version.h only when the application has a VERSION file,
 * so the include has to be conditional: a customer without one must still build.
 * Without this the version silently reported as "unknown" even for apps that do
 * declare one, because the fallback below was the only definition in scope.
 */
#if defined(__has_include)
#ifdef CONFIG_BALENA_MCU_IDENTITY
#include <balena_mcu/identity.h>
#endif
#ifdef CONFIG_BALENA_MCU_SMP_CAN
#include <balena_mcu/can_id.h>
#endif

#if __has_include(<app_version.h>)
#include <app_version.h>
#endif
#endif

#define BALENA_MCU_GROUP_ID  CONFIG_BALENA_MCU_SMP_GROUP_ID
#define BALENA_MCU_CMD_DESCRIBE 0

#ifndef APP_VERSION_STRING
#define APP_VERSION_STRING "unknown"
#endif

static int balena_mcu_describe(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	bool ok;

	ok = zcbor_tstr_put_lit(zse, "contract") &&
	     zcbor_tstr_put_lit(zse, CONFIG_BALENA_MCU_CONTRACT_VERSION) &&
	     zcbor_tstr_put_lit(zse, "board") &&
	     zcbor_tstr_put_lit(zse, CONFIG_BOARD_TARGET) &&
	     zcbor_tstr_put_lit(zse, "app_version") &&
	     zcbor_tstr_put_lit(zse, APP_VERSION_STRING) &&
	     zcbor_tstr_put_lit(zse, "channels") &&
	     zcbor_uint32_put(zse, CONFIG_BALENA_MCU_CHANNELS) &&
	     /* Whether this device can receive an update at all. A board with no
	      * secondary slot is a legitimate bring-up configuration, but a host
	      * that only finds out when the upload fails reports a raw
	      * MGMT_ERR_ENOTSUP, which explains nothing.
	      */
	     zcbor_tstr_put_lit(zse, "img") &&
	     zcbor_bool_put(zse, IS_ENABLED(CONFIG_BALENA_MCU_IMG_MGMT)) &&
	     /* True only for the provisioning placeholder. Lets the host say
	      * "this board has never received firmware" instead of guessing.
	      */
	     zcbor_tstr_put_lit(zse, "idle") &&
	     zcbor_bool_put(zse, IS_ENABLED(CONFIG_BALENA_MCU_IDLE));

#ifdef CONFIG_BALENA_MCU_IDENTITY
	/* Whether this board carries a valid identity record. "Provisioned and
	 * happens to use the default id" and "never provisioned" are different
	 * operational states and the host should not have to infer which.
	 */
	ok = ok && zcbor_tstr_put_lit(zse, "provisioned") &&
	     zcbor_bool_put(zse, balena_mcu_identity_is_provisioned());

	const char *serial = balena_mcu_identity_serial();

	if (serial != NULL) {
		ok = ok && zcbor_tstr_put_lit(zse, "serial") &&
		     zcbor_tstr_put_term(zse, serial, BALENA_MCU_IDENTITY_SERIAL_LEN);
	}
#endif

#ifdef CONFIG_BALENA_MCU_SMP_CAN
	/* The id this board is ACTUALLY answering on. A host whose placement
	 * label disagrees can only find out once it is already talking, but
	 * "answers to 0x45, your label says 0x42" beats a bare timeout when the
	 * operator is looking at the wrong board of several.
	 */
	ok = ok && zcbor_tstr_put_lit(zse, "can_node_id") &&
	     zcbor_uint32_put(zse, balena_mcu_can_node_id());
#endif

#ifdef CONFIG_BALENA_MCU_HEALTH
	/* Only advertised when the application opted in, so the host can tell
	 * "healthy" from "does not report health".
	 */
	ok = ok && zcbor_tstr_put_lit(zse, "app_healthy") &&
	     zcbor_bool_put(zse, balena_mcu_health_ok());
#endif

	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler balena_mcu_handlers[] = {
	[BALENA_MCU_CMD_DESCRIBE] = {
		.mh_read = balena_mcu_describe,
		.mh_write = NULL,
	},
};

static struct mgmt_group balena_mcu_group = {
	.mg_handlers = balena_mcu_handlers,
	.mg_handlers_count = ARRAY_SIZE(balena_mcu_handlers),
	.mg_group_id = BALENA_MCU_GROUP_ID,
};

static void balena_mcu_register(void)
{
	mgmt_register_group(&balena_mcu_group);
}

MCUMGR_HANDLER_DEFINE(balena_mcu, balena_mcu_register);
