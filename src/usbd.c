/*
 * Copyright (c) 2026 balena
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB device setup for the contract's two channels.
 *
 * Zephyr's canned initialiser (CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT) cannot
 * be used here, and the reason is stated in its own source:
 *
 *     "This code only registers the first CDC-ACM instance."
 *
 * So a board declaring both a management and a log channel enumerates with only
 * one, and the log channel silently never appears. Observed on real hardware:
 * an RP2040 came up with balena-mcu-mgmt and nothing else.
 *
 * Registering every class instance is the whole job, and
 * usbd_register_all_classes() does it. Interface string descriptors still come
 * from each node's devicetree `label`, handled by the CDC-ACM driver itself --
 * that part needs no code.
 */
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/usb/usbd.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(balena_mcu_usbd, CONFIG_BALENA_MCU_LOG_LEVEL);

/*
 * DFU is excluded deliberately. It is a second, unauthenticated path to write
 * firmware, and the contract's whole safety argument rests on updates going
 * through SMP so that confirmation is only reachable by a device that still
 * speaks the contract.
 */
static const char *const blocklist[] = {
	"dfu_dfu",
	NULL,
};

USBD_DEVICE_DEFINE(balena_mcu_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_BALENA_MCU_USB_VID, CONFIG_BALENA_MCU_USB_PID);

USBD_DESC_LANG_DEFINE(balena_mcu_lang);
USBD_DESC_MANUFACTURER_DEFINE(balena_mcu_mfr, CONFIG_BALENA_MCU_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(balena_mcu_product, CONFIG_BALENA_MCU_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(balena_mcu_sn)));

USBD_DESC_CONFIG_DEFINE(balena_mcu_fs_desc, "balena MCU FS configuration");
USBD_DESC_CONFIG_DEFINE(balena_mcu_hs_desc, "balena MCU HS configuration");

/* A macro rather than a `static const`: USBD_CONFIGURATION_DEFINE builds a
 * static initialiser, and a const variable is not a constant expression in C.
 * Bus-powered, which is what a USB-attached MCU board is.
 */
#define BALENA_MCU_USB_ATTRS 0
#define BALENA_MCU_USB_MAX_POWER 250

USBD_CONFIGURATION_DEFINE(balena_mcu_fs_config, BALENA_MCU_USB_ATTRS,
			  BALENA_MCU_USB_MAX_POWER, &balena_mcu_fs_desc);
USBD_CONFIGURATION_DEFINE(balena_mcu_hs_config, BALENA_MCU_USB_ATTRS,
			  BALENA_MCU_USB_MAX_POWER, &balena_mcu_hs_desc);

static int balena_mcu_usbd_init(void)
{
	int err;

	err = usbd_add_descriptor(&balena_mcu_usbd, &balena_mcu_lang);
	if (err) {
		LOG_ERR("failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&balena_mcu_usbd, &balena_mcu_mfr);
	if (err) {
		LOG_ERR("failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&balena_mcu_usbd, &balena_mcu_product);
	if (err) {
		LOG_ERR("failed to add product descriptor (%d)", err);
		return err;
	}

	IF_ENABLED(CONFIG_HWINFO, ({
		/* A per-device serial number derived from hardware ID. Not part
		 * of the contract -- placement is by port path -- but it makes
		 * two identical boards distinguishable in udev and lsusb.
		 */
		err = usbd_add_descriptor(&balena_mcu_usbd, &balena_mcu_sn);
		if (err) {
			LOG_WRN("failed to add serial number descriptor (%d)", err);
		}
	}))

	if (usbd_caps_speed(&balena_mcu_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&balena_mcu_usbd, USBD_SPEED_HS,
					     &balena_mcu_hs_config);
		if (err) {
			LOG_ERR("failed to add high-speed configuration (%d)", err);
			return err;
		}

		/* Every class instance, which is the point of this file. */
		err = usbd_register_all_classes(&balena_mcu_usbd, USBD_SPEED_HS, 1, blocklist);
		if (err) {
			LOG_ERR("failed to register high-speed classes (%d)", err);
			return err;
		}
	}

	err = usbd_add_configuration(&balena_mcu_usbd, USBD_SPEED_FS, &balena_mcu_fs_config);
	if (err) {
		LOG_ERR("failed to add full-speed configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&balena_mcu_usbd, USBD_SPEED_FS, 1, blocklist);
	if (err) {
		LOG_ERR("failed to register full-speed classes (%d)", err);
		return err;
	}

	err = usbd_init(&balena_mcu_usbd);
	if (err) {
		LOG_ERR("failed to initialise USB device (%d)", err);
		return err;
	}

	err = usbd_enable(&balena_mcu_usbd);
	if (err) {
		LOG_ERR("failed to enable USB device (%d)", err);
		return err;
	}

	LOG_INF("USB device up with all contract channels registered");
	return 0;
}

/* After the CDC-ACM instances exist, before the application runs. */
SYS_INIT(balena_mcu_usbd_init, APPLICATION, CONFIG_BALENA_MCU_USB_INIT_PRIORITY);
