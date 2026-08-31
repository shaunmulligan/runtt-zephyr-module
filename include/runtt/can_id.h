/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * The one place the CAN node id is decided.
 *
 * Both the SMP transport and the log backend need it, and they must not be able
 * to disagree -- a management id and a console id derived from different numbers
 * would be a silent, baffling failure. So both call this.
 */

#ifndef RUNTT_CAN_ID_H_
#define RUNTT_CAN_ID_H_

#include <zephyr/kernel.h>

#ifdef CONFIG_RUNTT_IDENTITY
#include <runtt/identity.h>
#endif

/** How far above the node id the device's console frames sit. */
#define RUNTT_CAN_LOG_ID_OFFSET 2U

static inline uint32_t runtt_can_node_id(void)
{
#ifdef CONFIG_RUNTT_IDENTITY
	return runtt_identity_can_node_id();
#else
	return (uint32_t)CONFIG_RUNTT_CAN_NODE_ID;
#endif
}

#endif /* RUNTT_CAN_ID_H_ */
