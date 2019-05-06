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

enum command_types {
	COMMAND_NONE,
	COMMAND_SAVE,
	COMMAND_SET_BODY_RGB_LED,
};

enum value_types {
	VALUE_NONE,
	VALUE_RGB,
};

struct rival_command_data {
	uint8_t value_type;
	uint8_t report_type;

	uint8_t prefix[16];
	uint32_t prefix_length;
	uint8_t suffix[16];
	uint32_t suffix_length;
};

struct rival_led_data {
	char name[32];
	uint32_t brightness;

	enum command_types command_type;
	enum command_types save_command_type;

	struct hid_device *hdev;
	struct led_classdev cdev;
	struct work_struct work;
};

struct rival_mouse_data {
	uint32_t vendor;
	uint32_t product;
	bool registered;

	struct rival_led_data body_led;
};

static struct rival_command_data rival_commands[] = {
	// COMMAND_NONE
	{},

	// COMMAND_SAVE
	{
		.value_type = VALUE_NONE,
		.report_type = HID_OUTPUT_REPORT,
		.prefix = { 0x09, 0x00 },
		.prefix_length = 2,
		.suffix = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
		.suffix_length = 7,
	},

	// COMMAND_SET_BODY_RGB_LED
	{
		.value_type = VALUE_RGB,
		.report_type = HID_OUTPUT_REPORT,
		.prefix = { 0x05, 0x00 },
		.prefix_length = 2,
		.suffix = { 0x00, 0x00, 0x00, 0x00 },
		.suffix_length = 4,
	},
};

static struct rival_mouse_data rival_mice[] = {
	{
		.vendor = USB_VENDOR_ID_STEELSERIES,
		.product = USB_DEVICE_ID_STEELSERIES_RIVAL_110,
		.registered = false,

		.body_led = {
			.name = "rival:rgb:body",
			.command_type = COMMAND_SET_BODY_RGB_LED,
			.save_command_type = COMMAND_SAVE,
		},
	},
};

static int rival_set_report(struct hid_device *hdev, uint8_t report_type,
		uint8_t *buf, size_t buf_size) {
	uint8_t *dmabuf;
	int ret;

	dmabuf = kmemdup(buf, buf_size, GFP_KERNEL);
	if (!dmabuf) {
		return -ENOMEM;
	}

	ret = hid_hw_raw_request(hdev, dmabuf[0], dmabuf, buf_size,
			report_type, HID_REQ_SET_REPORT);
	kfree(dmabuf);

	return ret;
}

static int rival_run_command(struct hid_device *hdev, enum command_types command_type,
		void *data) {
	struct rival_command_data command;
	uint8_t buf[64];
	size_t buf_size = 0;
	size_t i;
	int brightness;
	int ret;

	if (command_type == COMMAND_NONE) {
		return -EINVAL;
	}

	command = rival_commands[command_type];

	// report_id + prefix + value + suffix
	buf[buf_size++] = 0x0;

	for (i = 0; i < command.prefix_length; i++) {
		buf[buf_size++] = command.prefix[i];
	}

	switch (command.value_type) {
	case VALUE_RGB:
		brightness =  *(int*) data;
		buf[buf_size++] = brightness >> 16 & 0xff;
		buf[buf_size++] = brightness >> 8 & 0xff;
		buf[buf_size++] = brightness & 0xff;
		break;
	default:
		break;
	}

	for (i = 0; i < command.suffix_length; i++) {
		buf[buf_size++] = command.suffix[i];
	}

	ret = rival_set_report(hdev, command.report_type, buf, buf_size);
	if (ret < 0) {
		hid_err(hdev, "%s: failed to set led brightness: %d\n", __func__, ret);
	}

	return ret;
}

static void rival_led_work(struct work_struct *work) {
	struct rival_led_data *rival_led = container_of(work, struct rival_led_data, work);

	rival_run_command(rival_led->hdev, rival_led->command_type, &rival_led->brightness);
	rival_run_command(rival_led->hdev, rival_led->save_command_type, NULL);
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
	int ret;

	switch (rival_led->command_type) {
	case COMMAND_SET_BODY_RGB_LED:
		rival_led->cdev.max_brightness = LED_RGB_MAX_BRIGHTNESS;
		break;
	default:
		return 0;
	}

	rival_led->hdev = hdev;
	rival_led->cdev.name = rival_led->name;
	rival_led->cdev.brightness_set = rival_led_brightness_set;
	rival_led->cdev.brightness_get = rival_led_brightness_get;
	INIT_WORK(&rival_led->work, rival_led_work);

	ret = led_classdev_register(&hdev->dev, &rival_led->cdev);
	if (ret < 0) {
		return ret;
	}

	return ret;
}

static void rival_unregister_led(struct hid_device *hdev, struct rival_led_data *rival_led) {
	if (rival_led->command_type == COMMAND_NONE) {
		return;
	}

	cancel_work_sync(&rival_led->work);
	led_classdev_unregister(&rival_led->cdev);
}

static void rival_register_mouse(struct hid_device *hdev, struct rival_mouse_data *rival_mouse) {
	int rc;

	if (rival_mouse->registered) {
		hid_err(hdev, "%s: already registered mouse\n", __func__);
		return;
	}

	rc = rival_register_led(hdev, &rival_mouse->body_led);
	if (rc) {
		hid_err(hdev, "%s: failed to register body led\n", __func__);
	}

	rival_mouse->registered = true;

	hid_info(hdev, "%s: registered mouse\n", __func__);
}

static void rival_unregister_mouse(struct hid_device *hdev, struct rival_mouse_data *rival_mouse) {
	if (!rival_mouse->registered) {
		hid_err(hdev, "%s: already unregistered mouse\n", __func__);
		return;
	}

	rival_unregister_led(hdev, &rival_mouse->body_led);

	rival_mouse->registered = false;

	hid_info(hdev, "%s: unregistered mouse\n", __func__);
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

	for (i = 0; i < ARRAY_SIZE(rival_mice); i++) {
		if (hdev->vendor == rival_mice[i].vendor &&
				hdev->product == rival_mice[i].product) {
			rival_register_mouse(hdev, &rival_mice[i]);
			break;
		}
	}

	return ret;
}

static void rival_remove(struct hid_device *hdev) {
	size_t i;

	for (i = 0; i < ARRAY_SIZE(rival_mice); i++) {
		if (hdev->vendor == rival_mice[i].vendor &&
				hdev->product == rival_mice[i].product) {
			rival_unregister_mouse(hdev, &rival_mice[i]);
			break;
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
