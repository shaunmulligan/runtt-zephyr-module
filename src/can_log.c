/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 *
 * The application's console output, sent as raw CAN frames.
 *
 * A CAN target has no second channel the way a USB target does, and the
 * single-channel demux cannot help: it keys off the console transport's
 * base64-and-CRC framing markers, and SMP over ISO-TP carries raw frames with no
 * such markers. So logs need their own path, and this is it.
 *
 * RAW FRAMES, NOT ISO-TP, AND THAT IS THE WHOLE DESIGN.
 *
 * ISO-TP has flow control: the sender waits for the receiver to say go. A device
 * logging over ISO-TP with nobody listening therefore blocks, and a log backend
 * that blocks deadlocks boot -- the first LOG_INF before the host attaches would
 * never return. Raw frames are fire-and-forget. When no mailbox is free the frame
 * is dropped and the system carries on.
 *
 * Lossy under backpressure is the intended behaviour, not a defect. A dropped
 * frame appears as a mangled line, which is an honest signal. Losing log lines is
 * enormously better than not booting.
 *
 * Ordering needs no sequence number: CAN delivers frames of one identifier from
 * one sender in order, and this identifier has exactly one sender.
 *
 * The log id is the highest of the three a node uses, which on CAN means the
 * LOWEST arbitration priority. A firmware upload in progress therefore wins the
 * bus against chatty logs rather than being slowed by them -- the priority
 * ordering falls out of the addressing for free.
 *
 * Host side: crates/transport/src/can.rs, CanLogReader.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_core.h>
#include <zephyr/logging/log_output.h>

#include <balena_mcu/can_id.h>

#define CAN_NODE DT_CHOSEN(zephyr_canbus)

/* The third id a node owns: requests, replies one higher, logs one higher again.
 * Kept in step with the host, which derives the same number from the placement
 * label rather than being told it separately.
 *
 * Resolved at runtime through the same helper the SMP transport uses, so the two
 * cannot be derived from different node ids.
 */
static uint32_t log_id;

static const struct device *const can_dev = DEVICE_DT_GET(CAN_NODE);

/* Set once the CAN device is up. Until then every frame is dropped: the log
 * backend is initialised before drivers are necessarily ready, and sending to an
 * unstarted controller is an error worth not making noise about.
 */
static bool can_log_ready;

/* Counts frames the controller had no room for. Reported through the backend's
 * own dropped() path so the operator learns that output was lost rather than
 * silently seeing a shorter log.
 */
static uint32_t frames_dropped;

/* Frames wait here between the logging context and the sender thread.
 *
 * A queue rather than sending straight from the log call, because the obvious
 * approach does not work: can_send() with K_NO_WAIT fails with -EAGAIN whenever
 * no mailbox is free, and a mailbox only frees once the frame is on the wire. At
 * 125 kbit/s that is most of the time, so nearly every frame was being dropped
 * and the channel produced two frames a run. Measured, not assumed.
 *
 * So the drop boundary moves here. The log path does a non-blocking put and
 * carries on; the sender thread does a blocking send, which it is allowed to do
 * because it is not the logging context and cannot deadlock boot. Frames are
 * lost only when the queue is genuinely full, which is real backpressure rather
 * than an artefact of mailbox timing.
 */
K_MSGQ_DEFINE(tx_q, sizeof(struct can_frame), CONFIG_BALENA_MCU_CAN_LOG_QUEUE_DEPTH, 4);

static K_THREAD_STACK_DEFINE(tx_stack, CONFIG_BALENA_MCU_CAN_LOG_TX_STACK_SIZE);
static struct k_thread tx_thread_data;

static void tx_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		struct can_frame frame;

		k_msgq_get(&tx_q, &frame, K_FOREVER);

		/* Blocking, with a bound. Nothing above depends on this
		 * returning promptly, and a bound stops a wedged controller
		 * parking this thread forever.
		 */
		(void)can_send(can_dev, &frame, K_MSEC(CONFIG_BALENA_MCU_CAN_LOG_TX_TIMEOUT_MS),
			       NULL, NULL);
	}
}

/* One CAN frame's worth of console bytes.
 *
 * Note there is no attempt to align frames to line boundaries. A line spans
 * however many frames it needs and a frame may straddle two lines; the host
 * reassembles a byte stream and splits on newlines, exactly as it would for a
 * UART. Padding frames to line boundaries would waste bus and buy nothing.
 */
static void emit(const uint8_t *data, size_t len)
{
	struct can_frame frame = {
		.id = log_id,
		.dlc = (uint8_t)len,
	};

	memcpy(frame.data, data, len);

	/* K_NO_WAIT plus a callback: queue it if a mailbox is free, otherwise
	 * fail immediately. Never block -- see the file header.
	 */
	/* Never blocks: a full queue means the bus cannot keep up with the log
	 * volume, and dropping is the correct response. See the file header.
	 */
	if (k_msgq_put(&tx_q, &frame, K_NO_WAIT) != 0) {
		frames_dropped++;
	}
}

/* One frame's worth of bytes, filled a little at a time.
 *
 * THIS BATCHING IS NOT AN OPTIMISATION, IT IS LOAD-BEARING. Under
 * CONFIG_LOG_MODE_IMMEDIATE the log core hands this backend ONE BYTE PER CALL --
 * it does not buffer, because the point of immediate mode is that output appears
 * without waiting. Emitting a frame per call therefore meant one CAN frame per
 * character: a 60-character line became 60 frames, overran the queue on the
 * first line, and the channel delivered fragments. Filling a frame before
 * sending cuts that by eight and is what makes the channel usable at all.
 */
static uint8_t frame_fill[CAN_MAX_DLEN];
static size_t frame_used;
static struct k_spinlock fill_lock;

/* Caller holds fill_lock. */
static void flush_locked(void)
{
	if (frame_used > 0) {
		emit(frame_fill, frame_used);
		frame_used = 0;
	}
}

static int out(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	if (!can_log_ready) {
		/* Claim the bytes anyway. Returning short makes log_output
		 * retry forever against a device that is not there yet.
		 */
		return (int)length;
	}

	k_spinlock_key_t key = k_spin_lock(&fill_lock);

	for (size_t i = 0; i < length; i++) {
		frame_fill[frame_used++] = data[i];
		if (frame_used == sizeof(frame_fill)) {
			flush_locked();
		}
	}

	k_spin_unlock(&fill_lock, key);

	return (int)length;
}

/* Push out a part-filled frame. Called at the end of each message so a line
 * does not sit in the accumulator waiting for the next one to fill it.
 */
static void flush_frame(void)
{
	k_spinlock_key_t key = k_spin_lock(&fill_lock);

	flush_locked();
	k_spin_unlock(&fill_lock, key);
}

static uint8_t out_buf[CONFIG_BALENA_MCU_CAN_LOG_BUF_SIZE];
LOG_OUTPUT_DEFINE(log_output_can, out, out_buf, sizeof(out_buf));

static void process(const struct log_backend *const backend, union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	uint32_t flags = LOG_OUTPUT_FLAG_LEVEL | LOG_OUTPUT_FLAG_TIMESTAMP;

	log_output_msg_process(&log_output_can, &msg->log, flags);
	flush_frame();
}

static void dropped(const struct log_backend *const backend, uint32_t cnt)
{
	ARG_UNUSED(backend);

	log_output_dropped_process(&log_output_can, cnt);
	flush_frame();
}

static void panic(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);

	/* Nothing to flush: every frame was already handed to the controller or
	 * dropped. There is no queue of ours to drain.
	 */
	log_output_flush(&log_output_can);
	flush_frame();
}

static void init_backend(const struct log_backend *const backend)
{
	ARG_UNUSED(backend);

	log_output_ctx_set(&log_output_can, NULL);
}

/* Deliberately NO .is_ready hook.
 *
 * Reporting not-ready looks like the careful thing to do -- the CAN controller
 * genuinely is not started when the log core initialises -- but it is a trap. A
 * backend that fails is_ready() at init is parked in a retry mask, and the only
 * thing that drains that mask is the log processing thread (log_core.c:973).
 * Under CONFIG_LOG_MODE_IMMEDIATE there is no such thread, so the backend is
 * disabled at boot and never reconsidered. It cost an entirely silent channel.
 *
 * This backend is always willing to accept bytes; out() discards them while the
 * controller is down. That is the correct place for the readiness check, because
 * it is re-evaluated on every message rather than once, forever.
 */
static const struct log_backend_api can_log_api = {
	.process = process,
	.dropped = dropped,
	.panic = panic,
	.init = init_backend,
};

LOG_BACKEND_DEFINE(balena_mcu_can_log, can_log_api, true);

/* Runs after smp_can_init, which is what calls can_start(). Sharing the started
 * controller rather than starting it again is why the priority matters.
 */
static int can_log_init(void)
{
	if (!device_is_ready(can_dev)) {
		return -ENODEV;
	}

	log_id = balena_mcu_can_node_id() + BALENA_MCU_CAN_LOG_ID_OFFSET;

	k_thread_create(&tx_thread_data, tx_stack, K_THREAD_STACK_SIZEOF(tx_stack),
			tx_thread, NULL, NULL, NULL,
			CONFIG_BALENA_MCU_CAN_LOG_TX_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&tx_thread_data, "balena_mcu_canlog");

	can_log_ready = true;

	return 0;
}

SYS_INIT(can_log_init, APPLICATION, CONFIG_BALENA_MCU_CAN_LOG_INIT_PRIORITY);

/* Test hook: how many frames the controller had no room for. */
uint32_t balena_mcu_can_log_dropped(void)
{
	return frames_dropped;
}
