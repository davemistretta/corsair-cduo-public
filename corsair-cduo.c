// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * corsair-cduo.c - Corsair Commander Duo hwmon driver
 *
 * Supports: Corsair Commander Duo (USB PID 0x0c56)
 * Provides: 2 temperature sensors (temp1, temp2), 2 fan RPM inputs (fan1, fan2),
 *           2 PWM outputs (pwm1, pwm2) with independent per-channel control
 *
 * The device uses a CommanderCore protocol over 64-byte HID reports.
 * Communication uses a single handle (0xfc) with endpoint open/close cycles:
 *
 *   Close endpoint:  [08 05 01 fc]
 *   Open endpoint:   [08 0d fc <endpoint>]
 *   Read endpoint:   [08 08 fc]
 *   Write endpoint:  [08 06 fc <len_lo> <len_hi> 00 00 <dtype_lo> <dtype_hi> <data...>]
 *
 * Endpoints:
 *   0x17 - fan RPM (dtype 0x06)
 *   0x21 - temperature (dtype 0x10)
 *   0x18 - fan speed control (dtype 0x07, write-only)
 *
 * Temperature (dtype 0x10) response layout:
 *   byte[5]         = sensor count
 *   byte[6 + n*3]   = status (0x00 = OK)
 *   byte[7 + n*3]   = temp_lo  (LE16 deci-degrees C)
 *   byte[8 + n*3]   = temp_hi
 *
 * Fan RPM (dtype 0x06) response layout:
 *   byte[5]         = fan count
 *   byte[6 + n*2]   = rpm_lo  (LE16 RPM)
 *   byte[7 + n*2]   = rpm_hi
 *
 * Fan speed write (dtype 0x07) data layout:
 *   byte[0]         = number of channels
 *   byte[1 + n*4]   = channel ID
 *   byte[2 + n*4]   = 0x00
 *   byte[3 + n*4]   = duty percent (0-100)
 *   byte[4 + n*4]   = 0x00
 *
 * The device latches commanded fan speeds; no continuous resend required.
 */

#include <linux/delay.h>
#include <linux/hid.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif

#define USB_VENDOR_ID_CORSAIR		0x1b1c
#define USB_PRODUCT_ID_CMDR_DUO		0x0c56

#define PKT_LEN		64
#define OUT_BUF_LEN	(1 + PKT_LEN)	/* report ID byte + payload */
#define CMD_TIMEOUT_MS	1000
#define NUM_TEMPS	2
#define NUM_FANS	2

#define HANDLE_ID	0xfc	/* CommanderCore communication handle */

#define DTYPE_TEMP		0x10
#define DTYPE_FAN		0x06
#define DTYPE_PWM		0x07
#define DTYPE_FAN_STATUS	0x09

#define EP_SPEEDS	0x17
#define EP_TEMPS	0x21
#define EP_FAN_SPEED	0x18
#define EP_FAN_STATUS	0x1a	/* per-channel tach status (0x03 = tach present) */

#define POLL_RETRY_COUNT	5	/* read_endpoint max retries */
#define POLL_SLEEP_MS		50	/* ms between endpoint operations */
#define INIT_SETTLE_MS		100	/* ms after entering software mode */
#define INIT_RETRY_DELAY_MS	5000	/* ms before reattempting a failed init */

/* Response byte offsets */
#define RESP_ERR_OFF		2
#define RESP_DTYPE_OFF		3
#define RESP_COUNT_OFF		5
#define RESP_FAN_DATA_OFF	6
#define RESP_TEMP_DATA_OFF	6

struct csduo_data {
	struct hid_device *hdev;
	struct device *hwmon_dev;
	struct mutex lock;
	struct completion wait_input;
	u8 expect_cmd;   /* opcode of the in-flight command; other reports dropped */
	u8 *cmd_buffer;  /* DMA-safe heap buffer, OUT_BUF_LEN bytes */
	u8 resp[PKT_LEN];
	bool initialized;
	unsigned long init_last_fail; /* jiffies of last failed init attempt */
	unsigned long temp_updated; /* jiffies of last poll cycle */
	long temp_cache[NUM_TEMPS]; /* millidegrees */
	bool temp_valid[NUM_TEMPS];
	long fan_cache[NUM_FANS]; /* RPM */
	bool fan_valid[NUM_FANS];
	u8 pwm_cache[NUM_FANS];    /* last-written PWM value, 0-255 */
	bool pwm_written[NUM_FANS]; /* channel has a userspace-commanded duty */
	bool fan_status_logged;    /* one-shot probe-time fan status dmesg */
	char firmware[16];         /* "v1.v2.v3" from cmd [02 13] */
};

static const char * const csduo_temp_labels[] = {
	"Probe 1",
	"Probe 2",
};

static const char * const csduo_fan_labels[] = {
	"Fan 1",
	"Fan 2",
};

static umode_t csduo_is_visible(const void *data, enum hwmon_sensor_types type,
				u32 attr, int channel)
{
	if (type == hwmon_temp && channel < NUM_TEMPS)
		return 0444;
	if (type == hwmon_fan && channel < NUM_FANS)
		return 0444;
	if (type == hwmon_pwm && channel < NUM_FANS)
		return 0644;
	return 0;
}

static const struct hwmon_channel_info * const csduo_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	NULL
};

/* Caller must hold priv->lock. Returns 0 on success, -ETIMEDOUT if no response. */
static int csduo_send_recv(struct csduo_data *priv, const u8 *cmd, int len)
{
	int ret;

	memset(priv->cmd_buffer, 0, OUT_BUF_LEN);
	priv->cmd_buffer[0] = 0x00; /* report ID */
	memcpy(priv->cmd_buffer + 1, cmd, min_t(int, len, PKT_LEN));
	priv->expect_cmd = len > 1 ? cmd[1] : 0;
	reinit_completion(&priv->wait_input);

	ret = hid_hw_output_report(priv->hdev, priv->cmd_buffer, OUT_BUF_LEN);
	if (ret < 0) {
		hid_dbg(priv->hdev, "output_report failed: %d\n", ret);
		return ret;
	}

	if (!wait_for_completion_timeout(&priv->wait_input,
					 msecs_to_jiffies(CMD_TIMEOUT_MS)))
		return -ETIMEDOUT;

	hid_dbg(priv->hdev,
		"cmd [%02x %02x %02x %02x %02x] resp [%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x]\n",
		len > 0 ? cmd[0] : 0, len > 1 ? cmd[1] : 0, len > 2 ? cmd[2] : 0,
		len > 3 ? cmd[3] : 0, len > 4 ? cmd[4] : 0,
		priv->resp[0], priv->resp[1], priv->resp[2], priv->resp[3],
		priv->resp[4], priv->resp[5], priv->resp[6], priv->resp[7],
		priv->resp[8], priv->resp[9], priv->resp[10], priv->resp[11]);

	return 0;
}

/*
 * CommanderCore endpoint operations.
 * All sensor/control communication uses a close-open-read/write-close
 * cycle on handle 0xfc with the appropriate endpoint number.
 */

static void csduo_close_endpoint(struct csduo_data *priv)
{
	static const u8 cmd[] = { 0x08, 0x05, 0x01, HANDLE_ID };
	int ret = csduo_send_recv(priv, cmd, sizeof(cmd));

	if (ret)
		hid_warn(priv->hdev, "close endpoint failed: %d\n", ret);
}

static int csduo_open_endpoint(struct csduo_data *priv, u8 endpoint)
{
	u8 cmd[] = { 0x08, 0x0d, HANDLE_ID, endpoint };

	return csduo_send_recv(priv, cmd, sizeof(cmd));
}

/*
 * Read from the currently open endpoint, retrying until the expected dtype
 * appears in the response or we exhaust retries.
 */
static int csduo_read_endpoint(struct csduo_data *priv, u8 expected_dtype,
			       int retries)
{
	static const u8 cmd[] = { 0x08, 0x08, HANDLE_ID };
	int i, ret;

	for (i = 0; i < retries; i++) {
		ret = csduo_send_recv(priv, cmd, sizeof(cmd));
		if (ret)
			return ret;
		if (priv->resp[RESP_DTYPE_OFF] == expected_dtype)
			return 0;
		msleep(POLL_SLEEP_MS);
	}

	return -EIO;
}

/*
 * Full close-open-read-close cycle for a sensor endpoint. On success the
 * read response is copied to resp_out (PKT_LEN bytes) before the trailing
 * close, whose reply overwrites priv->resp. (Current firmware happens to
 * echo the previous data buffer in close/open responses, but that is an
 * undocumented quirk — never parse priv->resp after the close.)
 */
static int csduo_read_sensor(struct csduo_data *priv, u8 endpoint,
			     u8 expected_dtype, u8 *resp_out)
{
	int ret;

	csduo_close_endpoint(priv);
	msleep(POLL_SLEEP_MS);

	ret = csduo_open_endpoint(priv, endpoint);
	if (ret)
		return ret;
	msleep(POLL_SLEEP_MS);

	ret = csduo_read_endpoint(priv, expected_dtype, POLL_RETRY_COUNT);
	if (!ret)
		memcpy(resp_out, priv->resp, PKT_LEN);
	msleep(POLL_SLEEP_MS);

	csduo_close_endpoint(priv);
	return ret;
}

/* Response parsing */

static bool csduo_parse_fans(struct csduo_data *priv, const u8 *resp)
{
	int i, base;
	u16 raw;

	if (resp[RESP_ERR_OFF] != 0x00 || resp[RESP_DTYPE_OFF] != DTYPE_FAN) {
		for (i = 0; i < NUM_FANS; i++)
			priv->fan_valid[i] = false;
		return false;
	}

	for (i = 0; i < NUM_FANS; i++) {
		if (i >= resp[RESP_COUNT_OFF]) {
			priv->fan_valid[i] = false;
			continue;
		}
		base = RESP_FAN_DATA_OFF + i * 2;
		raw = get_unaligned_le16(&resp[base]);
		priv->fan_cache[i] = raw;
		priv->fan_valid[i] = true;
	}
	return true;
}

static bool csduo_parse_temps(struct csduo_data *priv, const u8 *resp)
{
	int i, base;
	u16 raw;

	if (resp[RESP_ERR_OFF] != 0x00 || resp[RESP_DTYPE_OFF] != DTYPE_TEMP) {
		for (i = 0; i < NUM_TEMPS; i++)
			priv->temp_valid[i] = false;
		return false;
	}

	for (i = 0; i < NUM_TEMPS; i++) {
		if (i >= resp[RESP_COUNT_OFF]) {
			priv->temp_valid[i] = false;
			continue;
		}
		base = RESP_TEMP_DATA_OFF + i * 3;
		if (resp[base] != 0x00) {
			priv->temp_valid[i] = false;
			continue;
		}
		raw = get_unaligned_le16(&resp[base + 1]);
		priv->temp_cache[i] = (long)(s16)raw * 100;
		priv->temp_valid[i] = true;
	}
	return true;
}

/* Device initialization and polling */

static int csduo_enter_software_mode(struct csduo_data *priv)
{
	static const u8 cmd[] = { 0x08, 0x01, 0x03, 0x00, 0x02 };

	return csduo_send_recv(priv, cmd, sizeof(cmd));
}

/*
 * Read firmware version via the device's control cmd [02 13]. Response format
 * differs from sensor reads: resp[1] echoes the cmd byte (0x02), resp[3..4]
 * are the major/minor, and resp[5..6] are the build number (LE16). This call
 * does not require software mode and is safe to invoke at probe time.
 */
static int csduo_get_firmware_version(struct csduo_data *priv)
{
	static const u8 cmd[] = { 0x08, 0x02, 0x13 };
	int ret;

	ret = csduo_send_recv(priv, cmd, sizeof(cmd));
	if (ret)
		return ret;
	if (priv->resp[RESP_ERR_OFF] != 0x00)
		return -EIO;

	snprintf(priv->firmware, sizeof(priv->firmware), "%u.%u.%u",
		 priv->resp[3], priv->resp[4],
		 get_unaligned_le16(&priv->resp[5]));
	return 0;
}

/*
 * One-shot diagnostic: read EP 0x1a (fan status) and log each channel's
 * status byte. Observed values: 0x03 = fan with working tach signal,
 * 0x01 = fan present but no tach signal. Other values are reported as
 * "unknown". This explains a fan_input of 0 in dmesg without changing
 * any userspace surface.
 */
static void csduo_log_fan_status(struct csduo_data *priv)
{
	u8 resp[PKT_LEN];
	const char *desc;
	int ret, i;
	u8 count, status;

	ret = csduo_read_sensor(priv, EP_FAN_STATUS, DTYPE_FAN_STATUS, resp);
	if (ret) {
		hid_dbg(priv->hdev, "fan status read failed: %d\n", ret);
		return;
	}
	if (resp[RESP_ERR_OFF] != 0x00 ||
	    resp[RESP_DTYPE_OFF] != DTYPE_FAN_STATUS)
		return;

	count = resp[RESP_COUNT_OFF];
	for (i = 0; i < count && i < NUM_FANS; i++) {
		status = resp[RESP_FAN_DATA_OFF + i];
		switch (status) {
		case 0x03: desc = "tach signal present"; break;
		case 0x01: desc = "no tach signal"; break;
		default:   desc = "unknown"; break;
		}
		hid_info(priv->hdev, "fan%d: status 0x%02x (%s)\n",
			 i + 1, status, desc);
	}
}

/*
 * Poll the device for sensor data. Reads fan RPM and temperatures via
 * separate endpoint cycles. Must be called with priv->lock held.
 *
 * A parse failure (device answered, but with an error byte or wrong dtype)
 * returns -EIO like a transport failure so csduo_ensure_fresh treats both
 * as a degraded session and re-enters software mode.
 */
static int csduo_poll_cycle(struct csduo_data *priv)
{
	u8 resp[PKT_LEN];
	int ret;

	ret = csduo_read_sensor(priv, EP_SPEEDS, DTYPE_FAN, resp);
	if (ret)
		return ret;
	if (!csduo_parse_fans(priv, resp)) {
		priv->temp_updated = 0;
		return -EIO;
	}

	ret = csduo_read_sensor(priv, EP_TEMPS, DTYPE_TEMP, resp);
	if (ret)
		return ret;
	if (!csduo_parse_temps(priv, resp)) {
		priv->temp_updated = 0;
		return -EIO;
	}

	priv->temp_updated = jiffies;
	return 0;
}

static int csduo_write_fan_pwm(struct csduo_data *priv, int channel, long val);

static int csduo_init_device(struct csduo_data *priv)
{
	int ret, i;

	if (priv->init_last_fail &&
	    time_before(jiffies, priv->init_last_fail +
			msecs_to_jiffies(INIT_RETRY_DELAY_MS)))
		return -EAGAIN;

	ret = csduo_enter_software_mode(priv);
	if (ret)
		goto fail;
	msleep(INIT_SETTLE_MS);

	/* One-shot firmware read after software mode is active. */
	if (!priv->firmware[0]) {
		if (csduo_get_firmware_version(priv) == 0)
			hid_info(priv->hdev, "firmware %s\n", priv->firmware);
	}

	ret = csduo_poll_cycle(priv);
	if (ret)
		goto fail;

	if (!priv->fan_status_logged) {
		csduo_log_fan_status(priv);
		priv->fan_status_logged = true;
	}

	priv->init_last_fail = 0;
	priv->initialized = true;

	/*
	 * Re-entering software mode can reset the device's commanded duty
	 * cycle. Restore any channel userspace has written so a self-heal
	 * does not leave the fans at the firmware default. Best-effort: a
	 * failed restore must not fail the init that just succeeded.
	 */
	for (i = 0; i < NUM_FANS; i++) {
		if (!priv->pwm_written[i])
			continue;
		if (csduo_write_fan_pwm(priv, i, priv->pwm_cache[i]))
			hid_warn(priv->hdev,
				 "failed to restore pwm%d after re-init\n",
				 i + 1);
	}

	return 0;

fail:
	priv->init_last_fail = jiffies;
	return ret;
}

/* Ensure cache is fresh, polling if older than 1 second */
static int csduo_ensure_fresh(struct csduo_data *priv)
{
	int ret, err;

	if (time_before(jiffies, priv->temp_updated + HZ))
		return 0;

	ret = csduo_poll_cycle(priv);
	if (!ret)
		return 0;

	/*
	 * The device intermittently drops out of software mode (e.g. after a
	 * USB power-management event or an idle period). Its fan endpoint then
	 * answers with error byte 0x03 and the temp dtype instead of fan data,
	 * so the poll fails with -EIO and stays broken because we only ever
	 * initialize once. Re-enter software mode and retry — this recovers the
	 * session in place, equivalent to an rmmod/modprobe but with no USB
	 * replug. csduo_init_device keeps its own failure backoff, so a recovery
	 * that itself fails won't be retried on every single read.
	 */
	hid_dbg(priv->hdev, "poll failed (%d); re-initializing device\n", ret);
	priv->initialized = false;
	err = csduo_init_device(priv);
	if (!err)
		hid_info(priv->hdev,
			 "recovered from failed poll (%d) by re-entering software mode\n",
			 ret);
	return err;
}

static int csduo_read_temp(struct csduo_data *priv, int channel, long *val)
{
	int ret;

	ret = csduo_ensure_fresh(priv);
	if (ret)
		return ret;

	if (!priv->temp_valid[channel])
		return -ENODATA;

	*val = priv->temp_cache[channel];
	return 0;
}

static int csduo_read_fan(struct csduo_data *priv, int channel, long *val)
{
	int ret;

	ret = csduo_ensure_fresh(priv);
	if (ret)
		return ret;

	if (!priv->fan_valid[channel])
		return -ENODATA;

	*val = priv->fan_cache[channel];
	return 0;
}

/*
 * Fan speed control.
 * Uses CommanderCore per-channel write format via endpoint 0x18.
 * Each channel is independently addressable. The device latches the
 * commanded speed; no periodic resend is required.
 */

static int csduo_write_fan_pwm(struct csduo_data *priv, int channel, long val)
{
	u8 cmd[14];
	u8 duty_pct, write_err;
	int data_len, total_len;
	int ret, i;

	if (channel < 0 || channel >= NUM_FANS)
		return -EINVAL;

	val = clamp_val(val, 0, 255);

	/* Scale 0-255 (Linux hwmon) to 0-100 (device percent) */
	duty_pct = (u8)((val * 100UL) / 255);

	/*
	 * Per-channel data: [num_ch=1] [ch_id] [0x00] [duty%] [0x00]
	 * Total data bytes: 5
	 * Length field = data_len + 2 (for dtype bytes)
	 */
	data_len = 5;
	total_len = data_len + 2;

	csduo_close_endpoint(priv);
	msleep(POLL_SLEEP_MS);

	ret = csduo_open_endpoint(priv, EP_FAN_SPEED);
	if (ret)
		return ret;
	msleep(POLL_SLEEP_MS);

	/* Write command: [08 06 <handle> <len_lo> <len_hi> 00 00 <dtype> <data>] */
	cmd[0]  = 0x08;
	cmd[1]  = 0x06;
	cmd[2]  = HANDLE_ID;
	cmd[3]  = total_len & 0xff;
	cmd[4]  = (total_len >> 8) & 0xff;
	cmd[5]  = 0x00;
	cmd[6]  = 0x00;
	cmd[7]  = DTYPE_PWM;	/* 0x07 */
	cmd[8]  = 0x00;
	cmd[9]  = 0x01;		/* 1 channel */
	cmd[10] = (u8)channel;	/* channel ID */
	cmd[11] = 0x00;
	cmd[12] = duty_pct;
	cmd[13] = 0x00;

	/*
	 * The device occasionally NAKs a PWM write (resp[RESP_ERR_OFF] != 0).
	 * Retry briefly so a transient NAK doesn't bubble up as -EIO to
	 * userspace. Capture the write status before the trailing close,
	 * whose reply overwrites priv->resp.
	 */
	for (i = 0; i < POLL_RETRY_COUNT; i++) {
		ret = csduo_send_recv(priv, cmd, sizeof(cmd));
		if (ret == 0 && priv->resp[RESP_ERR_OFF] == 0x00)
			break;
		msleep(POLL_SLEEP_MS);
	}
	write_err = priv->resp[RESP_ERR_OFF];
	msleep(POLL_SLEEP_MS);

	csduo_close_endpoint(priv);

	if (ret)
		return ret;
	if (write_err != 0x00)
		return -EIO;

	priv->pwm_cache[channel] = val;
	priv->pwm_written[channel] = true;
	return 0;
}

/* hwmon read/write callbacks */

static int csduo_read(struct device *dev, enum hwmon_sensor_types type,
		      u32 attr, int channel, long *val)
{
	struct csduo_data *priv = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&priv->lock);

	if (!priv->initialized) {
		ret = csduo_init_device(priv);
		if (ret)
			goto out_unlock;
	}

	switch (type) {
	case hwmon_temp:
		ret = csduo_read_temp(priv, channel, val);
		break;
	case hwmon_fan:
		ret = csduo_read_fan(priv, channel, val);
		break;
	case hwmon_pwm:
		/* Return cached last-written value; device has no readback */
		*val = priv->pwm_cache[channel];
		ret = 0;
		break;
	default:
		ret = -EOPNOTSUPP;
	}

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csduo_write(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long val)
{
	struct csduo_data *priv = dev_get_drvdata(dev);
	int ret = -EOPNOTSUPP;

	mutex_lock(&priv->lock);
	if (!priv->initialized) {
		ret = csduo_init_device(priv);
		if (ret)
			goto out_unlock;
	}

	if (type == hwmon_pwm && attr == hwmon_pwm_input && channel < NUM_FANS)
		ret = csduo_write_fan_pwm(priv, channel, val);

out_unlock:
	mutex_unlock(&priv->lock);
	return ret;
}

static int csduo_read_string(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		if (channel < NUM_TEMPS)
			*str = csduo_temp_labels[channel];
		else
			return -EOPNOTSUPP;
		break;
	case hwmon_fan:
		if (channel < NUM_FANS)
			*str = csduo_fan_labels[channel];
		else
			return -EOPNOTSUPP;
		break;
	default:
		return -EOPNOTSUPP;
	}
	return 0;
}

static const struct hwmon_ops csduo_hwmon_ops = {
	.is_visible = csduo_is_visible,
	.read = csduo_read,
	.write = csduo_write,
	.read_string = csduo_read_string,
};

static const struct hwmon_chip_info csduo_chip_info = {
	.ops = &csduo_hwmon_ops,
	.info = csduo_info,
};

static ssize_t firmware_version_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct csduo_data *priv = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n",
			  priv->firmware[0] ? priv->firmware : "unknown");
}
static DEVICE_ATTR_RO(firmware_version);

static struct attribute *csduo_attrs[] = {
	&dev_attr_firmware_version.attr,
	NULL,
};
static const struct attribute_group csduo_group = {
	.attrs = csduo_attrs,
};
static const struct attribute_group *csduo_groups[] = {
	&csduo_group,
	NULL,
};

/* HID driver callbacks */

static int csduo_probe(struct hid_device *hdev,
		       const struct hid_device_id *id)
{
	struct usb_interface *usbif;
	struct csduo_data *priv;
	int ret;

	if (!hid_is_usb(hdev))
		return -ENODEV;

	usbif = to_usb_interface(hdev->dev.parent);
	if (usbif->cur_altsetting->desc.bInterfaceNumber != 0)
		return -ENODEV;

	priv = devm_kzalloc(&hdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->cmd_buffer = devm_kzalloc(&hdev->dev, OUT_BUF_LEN, GFP_KERNEL);
	if (!priv->cmd_buffer)
		return -ENOMEM;

	priv->hdev = hdev;
	hid_set_drvdata(hdev, priv);
	mutex_init(&priv->lock);
	init_completion(&priv->wait_input);

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "hid_hw_open failed: %d\n", ret);
		goto err_stop;
	}

	priv->hwmon_dev = hwmon_device_register_with_info(
		&hdev->dev, "corsaircmdrduo", priv,
		&csduo_chip_info, csduo_groups);
	if (IS_ERR(priv->hwmon_dev)) {
		ret = PTR_ERR(priv->hwmon_dev);
		hid_err(hdev, "hwmon register failed: %d\n", ret);
		goto err_close;
	}

	hid_info(hdev, "Corsair Commander Duo initialized\n");

	return 0;

err_close:
	hid_hw_close(hdev);
err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void csduo_remove(struct hid_device *hdev)
{
	struct csduo_data *priv = hid_get_drvdata(hdev);
	static const u8 cmd[] = { 0x08, 0x01, 0x03, 0x00, 0x01 };

	hwmon_device_unregister(priv->hwmon_dev);

	mutex_lock(&priv->lock);
	if (priv->initialized)
		csduo_send_recv(priv, cmd, sizeof(cmd));
	mutex_unlock(&priv->lock);

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
	mutex_destroy(&priv->lock);
}

static int csduo_raw_event(struct hid_device *hdev,
			   struct hid_report *report, u8 *data, int size)
{
	struct csduo_data *priv = hid_get_drvdata(hdev);
	int n = min_t(int, size, PKT_LEN);

	/*
	 * Responses echo the command opcode in byte 1. Drop reports that do
	 * not match the in-flight command so a late reply to a timed-out
	 * command cannot satisfy the next caller's wait and shift every
	 * subsequent exchange off by one.
	 */
	if (size < 2 || data[1] != priv->expect_cmd)
		return 0;

	memset(priv->resp, 0, PKT_LEN);
	memcpy(priv->resp, data, n);
	complete(&priv->wait_input);

	return 0;
}

static const struct hid_device_id csduo_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR,
			 USB_PRODUCT_ID_CMDR_DUO) },
	{ }
};
MODULE_DEVICE_TABLE(hid, csduo_devices);

static struct hid_driver csduo_driver = {
	.name = "corsair-cduo",
	.id_table = csduo_devices,
	.probe = csduo_probe,
	.remove = csduo_remove,
	.raw_event = csduo_raw_event,
};
module_hid_driver(csduo_driver);

MODULE_AUTHOR("David Mistretta");
MODULE_DESCRIPTION("Corsair Commander Duo hwmon driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
