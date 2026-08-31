/*
 * Copyright (c) 2026 The runtt authors
 * SPDX-License-Identifier: MIT OR Apache-2.0
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
 * an RP2040 came up with runtt-mgmt and nothing else.
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
#ifdef CONFIG_RUNTT_IDENTITY
#include <runtt/identity.h>
#endif
LOG_MODULE_REGISTER(runtt_usbd, CONFIG_RUNTT_LOG_LEVEL);

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

USBD_DEVICE_DEFINE(runtt_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_RUNTT_USB_VID, CONFIG_RUNTT_USB_PID);

USBD_DESC_LANG_DEFINE(runtt_lang);
USBD_DESC_MANUFACTURER_DEFINE(runtt_mfr, CONFIG_RUNTT_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(runtt_product, CONFIG_RUNTT_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(runtt_sn)));

#ifdef CONFIG_RUNTT_IDENTITY
/* The provisioned serial, published as the USB serial string descriptor.
 *
 * THIS IS WHAT MAKES `usb:<serial>` PLACEMENT POSSIBLE WITHOUT PROBING. A host
 * resolving a board by serial must be able to read that serial WITHOUT talking
 * to the device: the alternative is opening every candidate tty and asking it
 * over SMP, which means sending frames to boards owned by other containers, and
 * an SMP frame arriving mid-upload on somebody else's board is exactly the
 * corruption ID_MM_DEVICE_IGNORE exists to prevent. In a string descriptor it is
 * visible to udev and sysfs before anyone opens anything.
 *
 * Zephyr's USBD_DESC_STRING_DEFINE builds its buffer from a string literal, so
 * it cannot carry a value read from flash at boot. The node is therefore
 * declared by hand over a mutable buffer, with bLength filled in once the length
 * is known.
 */
static uint8_t provisioned_sn_ascii[RUNTT_IDENTITY_SERIAL_LEN + 1];
static struct usbd_desc_node runtt_provisioned_sn = {
	.str = {
		.utype = USBD_DUT_STRING_SERIAL_NUMBER,
		.ascii7 = true,
	},
	.ptr = provisioned_sn_ascii,
	.bDescriptorType = USB_DESC_STRING,
};
#endif

USBD_DESC_CONFIG_DEFINE(runtt_fs_desc, "runtt FS configuration");
USBD_DESC_CONFIG_DEFINE(runtt_hs_desc, "runtt HS configuration");

/* A macro rather than a `static const`: USBD_CONFIGURATION_DEFINE builds a
 * static initialiser, and a const variable is not a constant expression in C.
 * Bus-powered, which is what a USB-attached MCU board is.
 */
#define RUNTT_USB_ATTRS 0
#define RUNTT_USB_MAX_POWER 250

USBD_CONFIGURATION_DEFINE(runtt_fs_config, RUNTT_USB_ATTRS,
			  RUNTT_USB_MAX_POWER, &runtt_fs_desc);
USBD_CONFIGURATION_DEFINE(runtt_hs_config, RUNTT_USB_ATTRS,
			  RUNTT_USB_MAX_POWER, &runtt_hs_desc);

static int runtt_usbd_init(void)
{
	int err;

	err = usbd_add_descriptor(&runtt_usbd, &runtt_lang);
	if (err) {
		LOG_ERR("failed to add language descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&runtt_usbd, &runtt_mfr);
	if (err) {
		LOG_ERR("failed to add manufacturer descriptor (%d)", err);
		return err;
	}

	err = usbd_add_descriptor(&runtt_usbd, &runtt_product);
	if (err) {
		LOG_ERR("failed to add product descriptor (%d)", err);
		return err;
	}

	/* The serial number descriptor, from the identity record when this board
	 * has one and from the hardware id otherwise.
	 *
	 * An unprovisioned board still publishes SOMETHING unique, which matters:
	 * two identical boards must stay distinguishable in udev and lsusb even
	 * before anyone assigns them names. What changes with provisioning is that
	 * the value becomes something a human chose and a compose file can name.
	 */
	bool sn_added = false;

#ifdef CONFIG_RUNTT_IDENTITY
	const char *provisioned = runtt_identity_serial();

	if (provisioned != NULL) {
		size_t len = strlen(provisioned);

		memcpy(provisioned_sn_ascii, provisioned, len);
		provisioned_sn_ascii[len] = '\0';
		/* Two bytes of header plus two per character: the same arithmetic
		 * USB_STRING_DESCRIPTOR_LENGTH does for a literal.
		 */
		runtt_provisioned_sn.bLength = (uint8_t)((len + 1) * 2);

		err = usbd_add_descriptor(&runtt_usbd, &runtt_provisioned_sn);
		if (err) {
			LOG_WRN("failed to add the provisioned serial descriptor (%d)", err);
		} else {
			LOG_INF("USB serial number is the provisioned \"%s\"", provisioned);
			sn_added = true;
		}
	}
#endif

	if (!sn_added) {
		IF_ENABLED(CONFIG_HWINFO, ({
			err = usbd_add_descriptor(&runtt_usbd, &runtt_sn);
			if (err) {
				LOG_WRN("failed to add serial number descriptor (%d)", err);
			}
		}))
	}

	if (usbd_caps_speed(&runtt_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&runtt_usbd, USBD_SPEED_HS,
					     &runtt_hs_config);
		if (err) {
			LOG_ERR("failed to add high-speed configuration (%d)", err);
			return err;
		}

		/* Every class instance, which is the point of this file. */
		err = usbd_register_all_classes(&runtt_usbd, USBD_SPEED_HS, 1, blocklist);
		if (err) {
			LOG_ERR("failed to register high-speed classes (%d)", err);
			return err;
		}
	}

	err = usbd_add_configuration(&runtt_usbd, USBD_SPEED_FS, &runtt_fs_config);
	if (err) {
		LOG_ERR("failed to add full-speed configuration (%d)", err);
		return err;
	}

	err = usbd_register_all_classes(&runtt_usbd, USBD_SPEED_FS, 1, blocklist);
	if (err) {
		LOG_ERR("failed to register full-speed classes (%d)", err);
		return err;
	}

	err = usbd_init(&runtt_usbd);
	if (err) {
		LOG_ERR("failed to initialise USB device (%d)", err);
		return err;
	}

	err = usbd_enable(&runtt_usbd);
	if (err) {
		LOG_ERR("failed to enable USB device (%d)", err);
		return err;
	}

	LOG_INF("USB device up with all contract channels registered");
	return 0;
}

/* After the CDC-ACM instances exist, before the application runs. */
SYS_INIT(runtt_usbd_init, APPLICATION, CONFIG_RUNTT_USB_INIT_PRIORITY);
