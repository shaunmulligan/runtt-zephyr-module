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
	     zcbor_uint32_put(zse, CONFIG_BALENA_MCU_CHANNELS);

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
