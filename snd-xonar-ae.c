/*
 * ASUS XONAR AE - Output switch kernel module
 *
 * Switches between headphones and speakers via a vendor-specific
 * USB control transfer. Exposes a sysfs attribute for userspace control.
 *
 * The card uses a proprietary USB Audio Class control:
 *   SET CUR on Output Terminal 7, CS=0x08
 *   data = [0x01, 0x03] for speakers, [0x02, 0x03] for headphones
 *
 * usb_control_msg() on endpoint 0 works without owning the interface,
 * so this module coexists peacefully with snd-usb-audio.
 *
 * Build:
 *   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
 *
 * Load:
 *   sudo insmod snd-xonar-ae.ko
 *
 * Usage:
 *   cat /sys/module/snd_xonar_ae/parameters/output
 *   echo speakers | sudo tee /sys/module/snd_xonar_ae/parameters/output
 *   echo headphones | sudo tee /sys/module/snd_xonar_ae/parameters/output
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>

#define XONAR_VENDOR_ID   0x0B05
#define XONAR_PRODUCT_ID  0x180F

/* USB Audio Class SET/GET CUR */
#define UAC2_CS_CUR       0x01

/* Vendor-specific control on Output Terminal 7 */
#define XONAR_OUTPUT_SEL_CS  0x08
#define XONAR_CONNECTOR_CS   0x02
#define XONAR_OT7_ID         7

static struct usb_device *xonar_udev;
static DEFINE_MUTEX(xonar_mutex);

/* Callback for usb_for_each_dev to find our device */
static int match_xonar(struct usb_device *udev, void *data)
{
	struct usb_device **found = data;

	if (le16_to_cpu(udev->descriptor.idVendor) == XONAR_VENDOR_ID &&
	    le16_to_cpu(udev->descriptor.idProduct) == XONAR_PRODUCT_ID) {
		*found = usb_get_dev(udev);
		return 1; /* stop iteration */
	}
	return 0;
}

static struct usb_device *find_xonar(void)
{
	struct usb_device *found = NULL;
	usb_for_each_dev(&found, match_xonar);
	return found;
}

static int xonar_switch(struct usb_device *udev, int speakers)
{
	u8 *buf;
	int ret;

	buf = kmalloc(2, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = speakers ? 0x01 : 0x02;
	buf[1] = 0x03;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
		UAC2_CS_CUR,
		USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_OUT,
		(XONAR_OUTPUT_SEL_CS << 8),      /* wValue: CS=8, CN=0 */
		(XONAR_OT7_ID << 8) | 0,         /* wIndex: Entity=7, Iface=0 */
		buf, 2, 1000);

	kfree(buf);
	return ret;
}

static int xonar_get_status(struct usb_device *udev)
{
	u8 *buf;
	int ret, result;

	buf = kmalloc(6, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
		UAC2_CS_CUR,
		USB_TYPE_CLASS | USB_RECIP_INTERFACE | USB_DIR_IN,
		(XONAR_CONNECTOR_CS << 8),       /* wValue: CS=2, CN=0 */
		(XONAR_OT7_ID << 8) | 0,         /* wIndex: Entity=7, Iface=0 */
		buf, 6, 1000);

	if (ret < 0) {
		kfree(buf);
		return ret;
	}

	/* buf[0] = bNrChannels: 8 = speakers, 2 = headphones */
	result = (buf[0] == 8) ? 1 : 0;
	kfree(buf);
	return result;
}

/*
 * Module parameter "output" - readable and writable via sysfs
 * /sys/module/snd_xonar_ae/parameters/output
 */
static int output_set(const char *val, const struct kernel_param *kp);
static int output_get(char *buf, const struct kernel_param *kp);

static const struct kernel_param_ops output_ops = {
	.set = output_set,
	.get = output_get,
};

static int current_output = -1; /* -1 = unknown */
module_param_cb(output, &output_ops, &current_output, 0644);
MODULE_PARM_DESC(output, "Output select: 'speakers' or 'headphones'");

static int output_set(const char *val, const struct kernel_param *kp)
{
	int speakers, ret;

	if (!xonar_udev)
		return -ENODEV;

	if (sysfs_streq(val, "speakers") || sysfs_streq(val, "1"))
		speakers = 1;
	else if (sysfs_streq(val, "headphones") || sysfs_streq(val, "0"))
		speakers = 0;
	else
		return -EINVAL;

	mutex_lock(&xonar_mutex);
	ret = xonar_switch(xonar_udev, speakers);
	if (ret >= 0) {
		current_output = speakers;
		pr_info("xonar-ae: switched to %s\n",
			speakers ? "speakers" : "headphones");
	}
	mutex_unlock(&xonar_mutex);

	return (ret < 0) ? ret : 0;
}

static int output_get(char *buf, const struct kernel_param *kp)
{
	int status;

	if (!xonar_udev)
		return sprintf(buf, "disconnected\n");

	mutex_lock(&xonar_mutex);
	status = xonar_get_status(xonar_udev);
	if (status >= 0)
		current_output = status;
	mutex_unlock(&xonar_mutex);

	if (current_output == 1)
		return sprintf(buf, "speakers\n");
	else if (current_output == 0)
		return sprintf(buf, "headphones\n");
	else
		return sprintf(buf, "unknown\n");
}

static int __init xonar_ae_init(void)
{
	int status;

	xonar_udev = find_xonar();
	if (!xonar_udev) {
		pr_err("xonar-ae: ASUS XONAR AE (0B05:180F) not found\n");
		return -ENODEV;
	}

	status = xonar_get_status(xonar_udev);
	current_output = (status >= 0) ? status : -1;

	pr_info("xonar-ae: loaded (current output: %s)\n",
		current_output == 1 ? "speakers" :
		current_output == 0 ? "headphones" : "unknown");

	return 0;
}

static void __exit xonar_ae_exit(void)
{
	if (xonar_udev)
		usb_put_dev(xonar_udev);
	xonar_udev = NULL;
	pr_info("xonar-ae: unloaded\n");
}

module_init(xonar_ae_init);
module_exit(xonar_ae_exit);

MODULE_AUTHOR("Reverse-engineered from ASUS Windows driver USB capture");
MODULE_DESCRIPTION("ASUS XONAR AE output switch (headphones/speakers)");
MODULE_LICENSE("GPL");
