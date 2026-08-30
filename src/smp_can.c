/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMP over ISO-TP on a CAN bus.
 *
 * Zephyr ships MCUmgr transports for serial, shell, BLE, UDP and LoRaWAN, but
 * none for CAN. This is that transport.
 *
 * A CAN frame carries 8 bytes (64 on CAN-FD) and an SMP packet runs to a
 * kilobyte, so something has to segment. Rather than invent that, this rides
 * ISO-TP (ISO 15765-2), which Zephyr already implements in subsys/canbus/isotp
 * and Linux implements in the mainline `can-isotp` module. Both ends get
 * segmentation, flow control and reassembly for free, and exchange whole SMP
 * packets -- an 8-byte header plus CBOR, with none of the base64-and-CRC
 * framing the console transport needs.
 *
 * Addressing matches the host side (see crates/transport/src/can.rs): the device
 * receives on BALENA_MCU_CAN_NODE_ID and replies one id higher, so a placement
 * label stays a single number.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/net_buf.h>
#include <zephyr/canbus/isotp.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zephyr/mgmt/mcumgr/transport/smp.h>

#include <mgmt/mcumgr/transport/smp_internal.h>

LOG_MODULE_REGISTER(balena_mcu_can, CONFIG_BALENA_MCU_LOG_LEVEL);

#define CAN_NODE DT_CHOSEN(zephyr_canbus)
BUILD_ASSERT(DT_NODE_HAS_STATUS(CAN_NODE, okay),
	     "BALENA_MCU_SMP_CAN needs /chosen/zephyr,canbus to name an enabled CAN device");

/* The device listens here and answers one id higher. Kept a convention rather
 * than two settings so the host's placement label is one number.
 */
#define RX_ID ((uint32_t)CONFIG_BALENA_MCU_CAN_NODE_ID)
#define TX_ID (RX_ID + 1U)

static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);
static struct smp_transport smp_can_transport;
static struct isotp_recv_ctx recv_ctx;

static const struct isotp_msg_id rx_addr = {
	.std_id = RX_ID,
	.dl = 8,
};
static const struct isotp_msg_id tx_addr = {
	.std_id = TX_ID,
	.dl = 8,
};

/* Block size 8, STmin 0: let the peer send eight frames between flow-control
 * acknowledgements and pause for nothing in between. The host is a Linux box
 * that can always keep up; the device is the slow side, and its limit is the
 * SMP buffer rather than the wire.
 */
static const struct isotp_fc_opts fc_opts = {
	.bs = 8,
	.stmin = 0,
};

static uint16_t smp_can_get_mtu(const struct net_buf *nb)
{
	ARG_UNUSED(nb);

	/* ISO-TP segments for us, so this is the SMP buffer's limit, not CAN's. */
	return CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE;
}

static struct isotp_send_ctx send_ctx;
static K_SEM_DEFINE(tx_done, 0, 1);
static int tx_result;

static void smp_can_tx_complete(int error_nr, void *arg)
{
	ARG_UNUSED(arg);

	tx_result = error_nr;
	k_sem_give(&tx_done);
}

static int smp_can_tx_pkt(struct net_buf *nb)
{
	int rc;

	/* isotp_send() rather than isotp_send_buf(): the buffered variant needs
	 * both ISOTP_USE_TX_BUF and ISOTP_ENABLE_CONTEXT_BUFFERS, and depending on
	 * optional Kconfig for the only way to reply is a poor trade.
	 *
	 * It is asynchronous and does not copy, so the packet has to outlive the
	 * transfer -- hence waiting for the completion callback before freeing.
	 * Blocking here is fine: MCUmgr calls output() from its own thread.
	 */
	rc = isotp_send(&send_ctx, can_dev, nb->data, nb->len, &tx_addr, &rx_addr,
			smp_can_tx_complete, NULL);
	if (rc == ISOTP_N_OK) {
		if (k_sem_take(&tx_done, K_MSEC(CONFIG_BALENA_MCU_CAN_TX_TIMEOUT_MS)) != 0) {
			/* No flow control from the peer, or nobody listening. The
			 * transfer may still be in flight, so the packet cannot be
			 * freed yet without risking a use-after-free.
			 */
			LOG_ERR("ISO-TP send timed out after %d ms; leaking one SMP packet "
				"rather than freeing a buffer the driver may still be reading",
				CONFIG_BALENA_MCU_CAN_TX_TIMEOUT_MS);
			return -ETIMEDOUT;
		}
		rc = tx_result;
	}

	if (rc != ISOTP_N_OK) {
		LOG_ERR("ISO-TP send failed (%d), dropping a %u-byte SMP response",
			rc, nb->len);
	}

	smp_packet_free(nb);

	return rc == ISOTP_N_OK ? 0 : -EIO;
}

static void smp_can_rx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		struct net_buf *isotp_buf = NULL;
		struct net_buf *nb;
		int rc;

		/* ISO-TP hands back whole messages, so there is no reassembly to
		 * do here and no partial-packet state to get wrong.
		 */
		rc = isotp_recv_net(&recv_ctx, &isotp_buf, K_FOREVER);
		if (rc != ISOTP_N_OK) {
			LOG_ERR("ISO-TP receive failed (%d)", rc);
			continue;
		}

		/* Copy into an SMP packet: the buffer above belongs to ISO-TP's
		 * own pool and must go back to it.
		 */
		nb = smp_packet_alloc();
		if (nb == NULL) {
			LOG_ERR("no SMP packet available; dropping %u bytes",
				net_buf_frags_len(isotp_buf));
			net_buf_unref(isotp_buf);
			continue;
		}

		for (struct net_buf *frag = isotp_buf; frag != NULL; frag = frag->frags) {
			if (net_buf_tailroom(nb) < frag->len) {
				LOG_ERR("SMP packet too small for a %u-byte request",
					net_buf_frags_len(isotp_buf));
				smp_packet_free(nb);
				nb = NULL;
				break;
			}
			net_buf_add_mem(nb, frag->data, frag->len);
		}
		net_buf_unref(isotp_buf);

		if (nb != NULL) {
			smp_rx_req(&smp_can_transport, nb);
		}
	}
}

K_THREAD_STACK_DEFINE(smp_can_rx_stack, CONFIG_BALENA_MCU_CAN_RX_STACK_SIZE);
static struct k_thread smp_can_rx_thread_data;

static int smp_can_init(void)
{
	int rc;

	if (!device_is_ready(can_dev)) {
		LOG_ERR("CAN device %s is not ready", can_dev->name);
		return -ENODEV;
	}

	rc = can_start(can_dev);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("failed to start CAN device %s (%d)", can_dev->name, rc);
		return rc;
	}

	rc = isotp_bind(&recv_ctx, can_dev, &rx_addr, &tx_addr, &fc_opts, K_FOREVER);
	if (rc != ISOTP_N_OK) {
		LOG_ERR("failed to bind ISO-TP on id %#x (%d)", RX_ID, rc);
		return -EIO;
	}

	smp_can_transport.functions.output = smp_can_tx_pkt;
	smp_can_transport.functions.get_mtu = smp_can_get_mtu;

	rc = smp_transport_init(&smp_can_transport);
	if (rc != 0) {
		LOG_ERR("failed to register the CAN SMP transport (%d)", rc);
		isotp_unbind(&recv_ctx);
		return rc;
	}

	k_thread_create(&smp_can_rx_thread_data, smp_can_rx_stack,
			K_THREAD_STACK_SIZEOF(smp_can_rx_stack),
			smp_can_rx_thread, NULL, NULL, NULL,
			CONFIG_BALENA_MCU_CAN_RX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&smp_can_rx_thread_data, "balena_mcu_can");

	LOG_INF("SMP over ISO-TP on %s, receiving on %#x and replying on %#x",
		can_dev->name, RX_ID, TX_ID);

	return 0;
}

SYS_INIT(smp_can_init, APPLICATION, CONFIG_BALENA_MCU_CAN_INIT_PRIORITY);
