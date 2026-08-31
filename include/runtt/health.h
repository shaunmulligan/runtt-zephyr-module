/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
 */
#ifndef RUNTT_HEALTH_H_
#define RUNTT_HEALTH_H_

#include <stdbool.h>

/**
 * @brief Report that the application is still doing useful work.
 *
 * The only C API this module has, and deliberately the whole surface.
 *
 * SMP echo proves the kernel is alive, not that the application thread is. An
 * app that has deadlocked in its own logic still answers echo perfectly well,
 * so a host that confirms on echo alone can confirm a broken image. Calling
 * this from the application's main loop extends the host's confirm gate from
 * "kernel alive" to "application alive".
 *
 * Optional: firmware that does not call it is still fully manageable, it just
 * gets the weaker guarantee.
 */
void runtt_health_feed(void);

/**
 * @brief Whether the application has fed the watchdog within its window.
 *
 * Consulted by the describe command so the host can see liveness without
 * needing a separate channel.
 */
bool runtt_health_ok(void);

#endif /* RUNTT_HEALTH_H_ */
