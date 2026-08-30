/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one place the CAN node id is decided.
 *
 * Both the SMP transport and the log backend need it, and they must not be able
 * to disagree -- a management id and a console id derived from different numbers
 * would be a silent, baffling failure. So both call this.
 */

#ifndef BALENA_MCU_CAN_ID_H_
#define BALENA_MCU_CAN_ID_H_

#include <zephyr/kernel.h>

#ifdef CONFIG_BALENA_MCU_IDENTITY
#include <balena_mcu/identity.h>
#endif

/** How far above the node id the device's console frames sit. */
#define BALENA_MCU_CAN_LOG_ID_OFFSET 2U

static inline uint32_t balena_mcu_can_node_id(void)
{
#ifdef CONFIG_BALENA_MCU_IDENTITY
	return balena_mcu_identity_can_node_id();
#else
	return (uint32_t)CONFIG_BALENA_MCU_CAN_NODE_ID;
#endif
}

#endif /* BALENA_MCU_CAN_ID_H_ */
