#include <linux/device.h>
#include <linux/hid.h>
#include <linux/hidraw.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>

#define HID_RIVAL_VERSION "0.1"

#define USB_VENDOR_ID_STEELSERIES			0x1038
#define USB_DEVICE_ID_STEELSERIES_RIVAL_110		0x1729

#define LED_RGB_MAX_BRIGHTNESS 16777216

enum led_type {
	LED_RGB = 0,
};

struct rival_led_data {
	uint32_t vendor;
	uint32_t product;
	bool registered;

	char name[32];
	uint32_t brightness;
	enum led_type type;

	struct hid_device *hdev;
	struct led_classdev cdev;
	struct work_struct work;

	uint32_t report_type;
	uint8_t command[16];
	uint32_t command_length;
	uint8_t suffix[16];
	uint32_t suffix_length;
};

static struct rival_led_data rival_leds[] = {
	{
		.vendor = USB_VENDOR_ID_STEELSERIES,
		.product = USB_DEVICE_ID_STEELSERIES_RIVAL_110,
		.registered = false,

		.name = "rival:rgb:body",
		.brightness = 0,
		.type = LED_RGB,

		.report_type = HID_OUTPUT_REPORT,
		.command = { 0x05, 0x00 },
		.command_length = 2,
		.suffix = { 0x00, 0x00, 0x00, 0x00 },
		.suffix_length = 4,
	},
};

static int rival_set_report(struct rival_led_data *rival_led,
		uint8_t *buf, size_t buf_size) {
	uint8_t *dmabuf;
	int ret;

	dmabuf = kmemdup(buf, buf_size, GFP_KERNEL);
	if (!dmabuf) {
		return -ENOMEM;
	}

	ret = hid_hw_raw_request(rival_led->hdev, dmabuf[0], dmabuf,
			buf_size, rival_led->report_type, HID_REQ_SET_REPORT);
	kfree(dmabuf);

	return ret;
}

static void rival_led_work(struct work_struct *work) {
	struct rival_led_data *rival_led = container_of(work, struct rival_led_data, work);
	uint8_t buf[64];
	size_t buf_size = 0;
	size_t i;
	int ret;

	// report_id + command + color + suffix
	buf[buf_size++] = 0x0;

	for (i = 0; i < rival_led->command_length; i++) {
		buf[buf_size++] = rival_led->command[i];
	}

	if (rival_led->type == LED_RGB) {
		buf[buf_size++] = rival_led->brightness >> 16 & 0xff;
		buf[buf_size++] = rival_led->brightness >> 8 & 0xff;
		buf[buf_size++] = rival_led->brightness & 0xff;
	}

	for (i = 0; i < rival_led->suffix_length; i++) {
		buf[buf_size++] = rival_led->suffix[0];
	}

	ret = rival_set_report(rival_led, buf, buf_size);
	if (ret < 0) {
		hid_err(rival_led->hdev, "%s: failed to set led brightness: %d\n", __func__, ret);
	}
}

static void rival_led_brightness_set(struct led_classdev *led_cdev,
		enum led_brightness brightness) {
	struct rival_led_data *rival_led = container_of(led_cdev, struct rival_led_data, cdev);

	rival_led->brightness = brightness;
	schedule_work(&rival_led->work);
}

static enum led_brightness rival_led_brightness_get(struct led_classdev *led_cdev) {
	struct rival_led_data *rival_led = container_of(led_cdev, struct rival_led_data, cdev);

	return rival_led->brightness;
}

static int rival_register_led(struct hid_device *hdev, struct rival_led_data *rival_led) {
	int ret = 0;

	if (rival_led->registered) {
		hid_err(hdev, "%s: already registered led %s\n", __func__, rival_led->name);
		return ret;
	}

	rival_led->hdev = hdev;
	rival_led->cdev.name = rival_led->name;
	rival_led->cdev.brightness_set = rival_led_brightness_set;
	rival_led->cdev.brightness_get = rival_led_brightness_get;
	INIT_WORK(&rival_led->work, rival_led_work);

	if (rival_led->type == LED_RGB) {
		rival_led->cdev.max_brightness = LED_RGB_MAX_BRIGHTNESS;
	}

	ret = led_classdev_register(&hdev->dev, &rival_led->cdev);
	if (ret < 0) {
		hid_err(hdev, "%s: failed to register led %s\n", __func__, rival_led->name);
		return ret;
	}

	hid_err(hdev, "%s: registered led %s\n", __func__, rival_led->name);
	rival_led->registered = true;

	return ret;
}

static void rival_unregister_led(struct hid_device *hdev, struct rival_led_data *rival_led) {
	if (!rival_led->registered) {
		hid_err(hdev, "%s: already unregistered led %s\n", __func__, rival_led->name);
		return;
	}

	cancel_work_sync(&rival_led->work);
	led_classdev_unregister(&rival_led->cdev);
	hid_err(hdev, "%s: unregistered led %s\n", __func__, rival_led->name);

	rival_led->registered = false;
}

static int rival_probe(struct hid_device *hdev, const struct hid_device_id *id) {
	size_t i;
	int ret;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "%s: hid parse failed: %d\n", __func__, ret);
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (ret) {
		hid_err(hdev, "%s: hid start failed: %d\n", __func__, ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(rival_leds); i++) {
		if (hdev->vendor == rival_leds[i].vendor &&
				hdev->product == rival_leds[i].product) {
			rival_register_led(hdev, &rival_leds[i]);
		}
	}

	return ret;
}

static void rival_remove(struct hid_device *hdev) {
	size_t i;

	for (i = 0; i < ARRAY_SIZE(rival_leds); i++) {
		if (hdev->vendor == rival_leds[i].vendor &&
				hdev->product == rival_leds[i].product) {
			rival_unregister_led(hdev, &rival_leds[i]);
		}
	}

	hid_hw_stop(hdev);
}

static const struct hid_device_id rival_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_STEELSERIES,
		USB_DEVICE_ID_STEELSERIES_RIVAL_110) },

	{ }
};
MODULE_DEVICE_TABLE(hid, rival_devices);

static struct hid_driver rival_driver = {
	.name		= "rival",
	.id_table	= rival_devices,
	.probe		= rival_probe,
	.remove		= rival_remove,
};
module_hid_driver(rival_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Control Rival Mouse RGB");
MODULE_AUTHOR("Demon Singur <demonsingur@gmail.com>");
MODULE_VERSION(HID_RIVAL_VERSION);
