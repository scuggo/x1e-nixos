// SPDX-License-Identifier: GPL-2.0
/*
 * HID over SPI (HIDSPI v3) transport driver for QSPI touchpads.
 *
 * Based on Microsoft's spi-hid v2 driver.
 * Copyright (c) 2020 Microsoft Corporation
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include "spi-hid-core.h"

static struct hid_ll_driver spi_hid_ll_driver;
static int spi_hid_process_input_report(struct spi_hid *shid,
					 const u8 *body, int body_len);

/* QSPI transfer helpers */

static void qspi_fill_cmd(u8 *buf, u8 opcode, u32 addr)
{
	buf[0] = opcode;
	buf[1] = (addr >> 16) & 0xFF;
	buf[2] = (addr >> 8) & 0xFF;
	buf[3] = addr & 0xFF;
}

static int qspi_read_sync(struct spi_hid *shid, u32 addr, void *buf, int len)
{
	struct spi_transfer xfer = {};
	u8 *tx, *rx;
	int ret;

	tx = kzalloc(len, GFP_KERNEL);
	rx = kzalloc(len, GFP_KERNEL);
	if (!tx || !rx) {
		kfree(tx);
		kfree(rx);
		return -ENOMEM;
	}

	qspi_fill_cmd(tx, SPI_HID_QSPI_READ_OPCODE, addr);

	xfer.tx_buf = tx;
	xfer.rx_buf = rx;
	xfer.len = len;
	xfer.tx_nbits = 4;
	xfer.rx_nbits = 4;

	ret = spi_sync_transfer(shid->spi, &xfer, 1);
	if (ret == 0)
		memcpy(buf, rx, len);

	kfree(tx);
	kfree(rx);
	return ret;
}

static int qspi_write_sync(struct spi_hid *shid, u32 addr,
			    const void *data, int len)
{
	struct device *dev = &shid->spi->dev;
	struct spi_transfer xfer = {};
	int total = SPI_HID_QSPI_CMD_LEN + len;
	u8 *tx;
	int ret;

	tx = kzalloc(total, GFP_KERNEL);
	if (!tx)
		return -ENOMEM;

	qspi_fill_cmd(tx, SPI_HID_QSPI_WRITE_OPCODE, addr);
	if (data && len > 0)
		memcpy(tx + SPI_HID_QSPI_CMD_LEN, data, len);

	xfer.tx_buf = tx;
	xfer.rx_buf = NULL;
	xfer.len = total;
	xfer.tx_nbits = 4;
	xfer.rx_nbits = 4;

	ret = spi_sync_transfer(shid->spi, &xfer, 1);

	kfree(tx);
	return ret;
}

static void spi_hid_parse_input_header(const u8 *buf,
					struct spi_hid_input_header *hdr)
{
	u32 raw = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);

	hdr->sync_const = (raw >> 24) & 0xFF;
	hdr->version = raw & 0x0F;
	hdr->body_len = ((raw >> 8) & 0x3FFF) * 4;
	hdr->last_frag = (raw >> 22) & 1;
}

static void spi_hid_parse_body_header(const u8 *buf,
				       struct spi_hid_body_header *bhdr)
{
	bhdr->report_type = buf[0];
	bhdr->content_length = buf[1] | (buf[2] << 8);
	bhdr->content_id = buf[3];
}

static int spi_hid_validate_header(struct spi_hid *shid,
				    struct spi_hid_input_header *hdr)
{
	struct device *dev = &shid->spi->dev;

	if (hdr->sync_const != SPI_HID_INPUT_HEADER_SYNC_BYTE) {
		dev_err(dev, "Bad sync: 0x%02x\n", hdr->sync_const);
		return -EINVAL;
	}

	if (hdr->version != SPI_HID_INPUT_HEADER_VERSION) {
		dev_err(dev, "Bad version: %d\n", hdr->version);
		return -EINVAL;
	}

	if (hdr->body_len == 0)
		return 0;

	if (shid->desc.max_input_length != 0 &&
	    hdr->body_len > shid->desc.max_input_length) {
		dev_err(dev, "Body %u > max %u\n",
			hdr->body_len, shid->desc.max_input_length);
		return -EMSGSIZE;
	}

	return 0;
}

static void spi_hid_parse_dev_desc(struct spi_hid_device_desc_raw *raw,
				    struct spi_hid_device_descriptor *desc)
{
	desc->hid_version = le16_to_cpu(raw->bcdVersion);
	desc->report_descriptor_length = le16_to_cpu(raw->wReportDescLength);
	desc->max_input_length = le16_to_cpu(raw->wMaxInputLength);
	desc->max_output_length = le16_to_cpu(raw->wMaxOutputLength);
	desc->max_fragment_length = le16_to_cpu(raw->wMaxFragmentLength);
	desc->vendor_id = le16_to_cpu(raw->wVendorID);
	desc->product_id = le16_to_cpu(raw->wProductID);
	desc->version_id = le16_to_cpu(raw->wVersionID);
	desc->flags = le16_to_cpu(raw->wFlags);
}

static int spi_hid_send_output(struct spi_hid *shid,
			       u8 report_type, u8 content_id,
			       const u8 *content, int content_len)
{
	u8 body[SPI_HID_BODY_HEADER_LEN + 512];
	int body_len = SPI_HID_BODY_HEADER_LEN + content_len;

	if (body_len > sizeof(body)) {
		dev_err(&shid->spi->dev, "Output too large: %d\n", body_len);
		return -E2BIG;
	}

	body[0] = report_type;
	body[1] = content_len & 0xFF;
	body[2] = (content_len >> 8) & 0xFF;
	body[3] = content_id;
	if (content && content_len > 0)
		memcpy(body + SPI_HID_BODY_HEADER_LEN, content, content_len);

	return qspi_write_sync(shid, SPI_HID_OUTPUT_ADDR, body, body_len);
}

/* Caller must hold shid->lock. */
static int spi_hid_sync_request_ms(struct spi_hid *shid,
				    u8 report_type, u8 content_id,
				    const u8 *content, int content_len,
				    unsigned int timeout_ms)
{
	struct device *dev = &shid->spi->dev;
	unsigned long timeout;
	int ret;

	reinit_completion(&shid->output_done);

	ret = spi_hid_send_output(shid, report_type, content_id,
				  content, content_len);
	if (ret) {
		dev_err(dev, "Failed to send output type %d: %d\n",
			report_type, ret);
		return ret;
	}

	timeout = wait_for_completion_timeout(&shid->output_done,
				msecs_to_jiffies(timeout_ms));
	if (timeout == 0) {
		dev_err(dev, "Response timeout for type %d (%ums)\n",
			report_type, timeout_ms);
		return -ETIMEDOUT;
	}

	return 0;
}

static int spi_hid_sync_request(struct spi_hid *shid,
				u8 report_type, u8 content_id,
				const u8 *content, int content_len)
{
	return spi_hid_sync_request_ms(shid, report_type, content_id,
				       content, content_len,
				       SPI_HID_RESPONSE_TIMEOUT_MS);
}

static int spi_hid_power_down(struct spi_hid *shid)
{
	int ret;

	if (!shid->powered)
		return 0;

	if (shid->pinctrl_sleep)
		pinctrl_select_state(shid->pinctrl, shid->pinctrl_sleep);

	if (shid->supply) {
		ret = regulator_disable(shid->supply);
		if (ret) {
			dev_err(&shid->spi->dev, "regulator disable failed\n");
			return ret;
		}
	}

	shid->powered = false;
	return 0;
}

static int spi_hid_power_up(struct spi_hid *shid)
{
	int ret;

	if (shid->powered)
		return 0;

	shid->powered = true;

	if (shid->supply) {
		ret = regulator_enable(shid->supply);
		if (ret) {
			shid->powered = false;
			return ret;
		}
	}

	usleep_range(5000, 6000);
	return 0;
}

static struct hid_device *spi_hid_disconnect_hid(struct spi_hid *shid)
{
	struct hid_device *hid = shid->hid;

	shid->hid = NULL;
	return hid;
}

static void spi_hid_stop_hid(struct spi_hid *shid)
{
	struct hid_device *hid;

	hid = spi_hid_disconnect_hid(shid);
	if (hid) {
		cancel_work_sync(&shid->create_device_work);
		cancel_work_sync(&shid->refresh_device_work);
		hid_destroy_device(hid);
	}
}

static int spi_hid_error_handler(struct spi_hid *shid)
{
	struct device *dev = &shid->spi->dev;
	int ret;

	if (shid->power_state == SPI_HID_POWER_MODE_OFF)
		return 0;

	dev_err(dev, "Error handler (attempt %d)\n", shid->attempts);

	if (shid->attempts++ >= SPI_HID_MAX_RESET_ATTEMPTS) {
		dev_err(dev, "Unresponsive device, aborting\n");
		spi_hid_stop_hid(shid);
		spi_hid_power_down(shid);
		return -ESHUTDOWN;
	}

	shid->ready = false;

	ret = pinctrl_select_state(shid->pinctrl, shid->pinctrl_reset);
	if (ret) {
		dev_err(dev, "Reset assert failed\n");
		return ret;
	}
	shid->power_state = SPI_HID_POWER_MODE_OFF;

	msleep(SPI_HID_RESET_ASSERT_MS);

	shid->power_state = SPI_HID_POWER_MODE_ACTIVE;
	ret = pinctrl_select_state(shid->pinctrl, shid->pinctrl_active);
	if (ret) {
		dev_err(dev, "Reset deassert failed\n");
		return ret;
	}

	msleep(SPI_HID_POST_DIR_DELAY_MS);

	return 0;
}

static int spi_hid_input_report_handler(struct spi_hid *shid,
					 const u8 *body, int body_len)
{
	struct device *dev = &shid->spi->dev;
	struct spi_hid_body_header bhdr;
	int ret;

	if (!shid->ready || !shid->hid)
		return 0;

	spi_hid_parse_body_header(body, &bhdr);

	ret = hid_input_report(shid->hid, HID_INPUT_REPORT,
			       (u8 *)(body + 3),
			       bhdr.content_length + 1, 1);

	if (ret == -ENODEV || ret == -EBUSY)
		return 0;

	return ret;
}

static int spi_hid_process_input_report(struct spi_hid *shid,
					 const u8 *body, int body_len)
{
	struct device *dev = &shid->spi->dev;
	struct spi_hid_body_header bhdr;
	struct spi_hid_device_desc_raw *raw;

	if (body_len < SPI_HID_BODY_HEADER_LEN) {
		dev_err(dev, "Body too short: %d\n", body_len);
		return -EINVAL;
	}

	spi_hid_parse_body_header(body, &bhdr);

	switch (bhdr.report_type) {
	case SPI_HID_REPORT_TYPE_DATA:
		return spi_hid_input_report_handler(shid, body, body_len);

	case SPI_HID_REPORT_TYPE_RESET_RESP:
		shid->resp_type = SPI_HID_REPORT_TYPE_RESET_RESP;
		shid->resp_len = 0;
		if (!completion_done(&shid->output_done)) {
			complete(&shid->output_done);
		} else {
			if (!shid->ready)
				schedule_work(&shid->reset_work);
			else
				schedule_work(&shid->refresh_device_work);
		}
		return 0;

	case SPI_HID_REPORT_TYPE_DEVICE_DESC:
		shid->attempts = 0;
		if (body_len >= SPI_HID_BODY_HEADER_LEN +
				sizeof(struct spi_hid_device_desc_raw)) {
			raw = (struct spi_hid_device_desc_raw *)
				(body + SPI_HID_BODY_HEADER_LEN);
			spi_hid_parse_dev_desc(raw, &shid->desc);
		}
		if (shid->resp_buf && body_len <= SPI_HID_MAX_INPUT_LEN) {
			memcpy(shid->resp_buf, body, body_len);
			shid->resp_len = body_len;
			shid->resp_type = bhdr.report_type;
		}
		if (!completion_done(&shid->output_done))
			complete(&shid->output_done);
		else if (!shid->hid)
			schedule_work(&shid->create_device_work);
		else
			schedule_work(&shid->refresh_device_work);
		return 0;

	case SPI_HID_REPORT_TYPE_COMMAND_RESP:
	case SPI_HID_REPORT_TYPE_GET_FEATURE_RESP:
	case SPI_HID_REPORT_TYPE_REPORT_DESC:
	case SPI_HID_REPORT_TYPE_SET_FEATURE_RESP:
	case SPI_HID_REPORT_TYPE_OUTPUT_REPORT_RESP:
	case SPI_HID_REPORT_TYPE_GET_INPUT_RESP:
		if (shid->resp_buf && body_len <= SPI_HID_MAX_INPUT_LEN) {
			memcpy(shid->resp_buf, body, body_len);
			shid->resp_len = body_len;
			shid->resp_type = bhdr.report_type;
		}
		if (!completion_done(&shid->output_done))
			complete(&shid->output_done);
		return 0;

	default:
		dev_err(dev, "Unknown report type: 0x%x (body_len=%d raw: %*ph)\n",
			bhdr.report_type, body_len,
			min(body_len, 16), body);
		return -EINVAL;
	}
}

static irqreturn_t spi_hid_irq_thread(int irq, void *_shid)
{
	struct spi_hid *shid = _shid;
	struct device *dev = &shid->spi->dev;
	struct spi_hid_input_header hdr;
	struct spi_transfer xfer = {};
	int ret;

	if (!shid->powered)
		return IRQ_HANDLED;

	xfer.tx_buf = shid->irq_hdr_tx;
	xfer.rx_buf = shid->irq_hdr_rx;
	xfer.len = SPI_HID_INPUT_HEADER_LEN;
	xfer.tx_nbits = 4;
	xfer.rx_nbits = 4;

	ret = spi_sync_transfer(shid->spi, &xfer, 1);
	if (ret) {
		shid->bus_error_count++;
		shid->bus_last_error = ret;
		if (shid->bus_error_count <= 5)
			dev_err(dev, "Header read failed: %d\n", ret);
		if (shid->bus_error_count == 100) {
			dev_err(dev, "Too many read errors, disabling IRQ\n");
			disable_irq_nosync(shid->spi->irq);
		}
		return IRQ_HANDLED;
	}

	spi_hid_parse_input_header(shid->irq_hdr_rx, &hdr);

	ret = spi_hid_validate_header(shid, &hdr);
	if (ret) {
		shid->bus_error_count++;
		shid->bus_last_error = ret;
		if (shid->bus_error_count <= 5 ||
		    shid->bus_error_count % 1000 == 0)
			dev_err(dev, "Invalid header (%u errors): %*ph\n",
				shid->bus_error_count,
				SPI_HID_INPUT_HEADER_LEN, shid->irq_hdr_rx);
		if (shid->bus_error_count == 100) {
			dev_err(dev, "Too many errors, disabling IRQ\n");
			disable_irq_nosync(shid->spi->irq);
		}
		return IRQ_HANDLED;
	}
	shid->bus_error_count = 0;

	if (hdr.body_len == 0)
		return IRQ_HANDLED;

	if (hdr.body_len > SPI_HID_MAX_INPUT_LEN) {
		dev_err(dev, "Body too large: %u\n", hdr.body_len);
		return IRQ_HANDLED;
	}

	memset(&xfer, 0, sizeof(xfer));
	xfer.tx_buf = shid->irq_bdy_tx;
	xfer.rx_buf = shid->irq_bdy_rx;
	xfer.len = hdr.body_len;
	xfer.tx_nbits = 4;
	xfer.rx_nbits = 4;

	ret = spi_sync_transfer(shid->spi, &xfer, 1);
	if (ret) {
		dev_err(dev, "Body read failed: %d\n", ret);
		shid->bus_error_count++;
		shid->bus_last_error = ret;
		return IRQ_HANDLED;
	}

	spi_hid_process_input_report(shid, shid->irq_bdy_rx, hdr.body_len);

	return IRQ_HANDLED;
}

static int spi_hid_create_device(struct spi_hid *shid);

static void spi_hid_reset_work(struct work_struct *work)
{
	struct spi_hid *shid = container_of(work, struct spi_hid, reset_work);
	struct device *dev = &shid->spi->dev;
	int ret, attempt;

	if (shid->ready)
		shid->dir_count++;

	flush_work(&shid->create_device_work);

	if (shid->power_state == SPI_HID_POWER_MODE_OFF)
		return;

	flush_work(&shid->refresh_device_work);

	mutex_lock(&shid->lock);

	for (attempt = 0; attempt < SPI_HID_MAX_RESET_ATTEMPTS; attempt++) {
		int dd_try, rd_try, rd_len;

		if (attempt > 0) {
			pinctrl_select_state(shid->pinctrl, shid->pinctrl_reset);
			msleep(SPI_HID_RESET_ASSERT_MS);
			pinctrl_select_state(shid->pinctrl, shid->pinctrl_active);
			msleep(2000);
		}

		for (dd_try = 0; dd_try < SPI_HID_MAX_INIT_RETRIES; dd_try++) {
			ret = spi_hid_sync_request(shid,
						   SPI_HID_OUT_DEVICE_DESC,
						   0, NULL, 0);
			if (ret) {
				dev_err(dev, "GET_DD failed: %d\n", ret);
				break;
			}
			if (shid->resp_type == SPI_HID_REPORT_TYPE_DEVICE_DESC)
				break;
			if (shid->resp_type == SPI_HID_REPORT_TYPE_RESET_RESP) {
				msleep(SPI_HID_POST_DIR_DELAY_MS);
				continue;
			}
			dev_err(dev, "GET_DD: unexpected type %d\n",
				shid->resp_type);
			msleep(SPI_HID_POST_DIR_DELAY_MS);
		}
		if (dd_try >= SPI_HID_MAX_INIT_RETRIES || ret) {
			dev_err(dev, "GET_DD failed, restarting\n");
			continue;
		}

		for (rd_try = 0; rd_try < SPI_HID_MAX_INIT_RETRIES; rd_try++) {
			ret = spi_hid_sync_request(shid,
						   SPI_HID_OUT_REPORT_DESC,
						   0, NULL, 0);
			if (ret) {
				dev_err(dev, "GET_RD failed: %d\n", ret);
				break;
			}
			if (shid->resp_type == SPI_HID_REPORT_TYPE_REPORT_DESC)
				break;
			if (shid->resp_type == SPI_HID_REPORT_TYPE_RESET_RESP) {
				msleep(SPI_HID_POST_DIR_DELAY_MS);
				continue;
			}
			dev_err(dev, "GET_RD: unexpected type %d\n",
				shid->resp_type);
			msleep(SPI_HID_POST_DIR_DELAY_MS);
		}
		if (rd_try >= SPI_HID_MAX_INIT_RETRIES || ret) {
			dev_err(dev, "GET_RD failed, restarting\n");
			continue;
		}

		rd_len = shid->resp_len - SPI_HID_BODY_HEADER_LEN;
		if (rd_len <= 0 || rd_len > 2048) {
			dev_err(dev, "GET_RD: bad length %d\n", rd_len);
			continue;
		}
		memcpy(shid->rd_buf,
		       shid->resp_buf + SPI_HID_BODY_HEADER_LEN, rd_len);
		shid->rd_len = rd_len;

		goto success;
	}

	dev_err(dev, "Init failed after %d reset cycles\n", attempt);
	mutex_unlock(&shid->lock);
	spi_hid_error_handler(shid);
	return;

success:
	mutex_unlock(&shid->lock);

	if (!shid->hid)
		spi_hid_create_device(shid);
}

static int spi_hid_create_device(struct spi_hid *shid)
{
	struct hid_device *hid;
	struct device *dev = &shid->spi->dev;
	int ret;

	hid = hid_allocate_device();
	if (IS_ERR(hid)) {
		dev_err(dev, "Failed to allocate hid device: %ld\n",
			PTR_ERR(hid));
		return PTR_ERR(hid);
	}

	hid->driver_data = shid->spi;
	hid->ll_driver = &spi_hid_ll_driver;
	hid->dev.parent = &shid->spi->dev;
	hid->bus = BUS_SPI;
	hid->version = shid->desc.hid_version;
	hid->vendor = shid->desc.vendor_id;
	hid->product = shid->desc.product_id;

	snprintf(hid->name, sizeof(hid->name), "spi %04hX:%04hX",
		 hid->vendor, hid->product);
	strscpy(hid->phys, dev_name(&shid->spi->dev), sizeof(hid->phys));

	shid->hid = hid;

	ret = hid_add_device(hid);
	if (ret) {
		dev_err(dev, "Failed to add hid device: %d\n", ret);
		hid = spi_hid_disconnect_hid(shid);
		if (hid)
			hid_destroy_device(hid);
		return ret;
	}

	return 0;
}

static void spi_hid_create_device_work(struct work_struct *work)
{
	struct spi_hid *shid =
		container_of(work, struct spi_hid, create_device_work);
	struct device *dev = &shid->spi->dev;
	int ret;

	if (shid->desc.hid_version != SPI_HID_SUPPORTED_VERSION) {
		dev_err(dev, "Unsupported version 0x%04x (expected 0x%04x)\n",
			shid->desc.hid_version, SPI_HID_SUPPORTED_VERSION);
		spi_hid_error_handler(shid);
		return;
	}

	ret = spi_hid_create_device(shid);
	if (ret) {
		dev_err(dev, "Failed to create HID device: %d\n", ret);
		return;
	}

	shid->attempts = 0;
}

static void spi_hid_refresh_device_work(struct work_struct *work)
{
	struct spi_hid *shid =
		container_of(work, struct spi_hid, refresh_device_work);

	shid->ready = true;
}

static int spi_hid_ll_start(struct hid_device *hid)
{
	struct spi_device *spi = hid->driver_data;
	struct spi_hid *shid = spi_get_drvdata(spi);

	if (shid->desc.max_input_length < HID_MIN_BUFFER_SIZE) {
		dev_err(&spi->dev, "max_input_length %d < HID_MIN_BUFFER_SIZE\n",
			shid->desc.max_input_length);
		return -EINVAL;
	}

	return 0;
}

static void spi_hid_ll_stop(struct hid_device *hid)
{
	hid->claimed = 0;
}

static int spi_hid_ll_open(struct hid_device *hid)
{
	return 0;
}

static void spi_hid_ll_close(struct hid_device *hid)
{
}

static int spi_hid_ll_power(struct hid_device *hid, int level)
{
	struct spi_device *spi = hid->driver_data;
	struct spi_hid *shid = spi_get_drvdata(spi);

	if (!shid->hid)
		return -ENODEV;

	return 0;
}

static int spi_hid_ll_parse(struct hid_device *hid)
{
	struct spi_device *spi = hid->driver_data;
	struct spi_hid *shid = spi_get_drvdata(spi);
	struct device *dev = &spi->dev;
	int ret;

	if (!shid->rd_buf || shid->rd_len <= 0) {
		dev_err(dev, "No report descriptor available\n");
		return -ENODATA;
	}

	ret = hid_parse_report(hid, shid->rd_buf, shid->rd_len);
	if (ret)
		dev_err(dev, "hid_parse_report failed: %d\n", ret);
	else
		shid->ready = true;

	return ret;
}

static int spi_hid_ll_raw_request(struct hid_device *hid,
				  unsigned char reportnum, __u8 *buf,
				  size_t len, unsigned char rtype, int reqtype)
{
	struct spi_device *spi = hid->driver_data;
	struct spi_hid *shid = spi_get_drvdata(spi);
	struct device *dev = &spi->dev;

	int ret, copy_len;

	if (reqtype == HID_REQ_SET_REPORT && rtype == HID_FEATURE_REPORT) {
		mutex_lock(&shid->lock);
		ret = spi_hid_sync_request(shid, SPI_HID_OUT_SET_FEATURE,
					   buf[0], buf + 1, len - 1);
		mutex_unlock(&shid->lock);
		if (ret)
			return ret;
		if (shid->resp_type == SPI_HID_REPORT_TYPE_RESET_RESP)
			return len;
		if (shid->resp_type != SPI_HID_REPORT_TYPE_SET_FEATURE_RESP) {
			dev_err(dev, "SET_FEATURE got resp type %d\n",
				shid->resp_type);
			return -EIO;
		}
		return len;
	}

	if (reqtype == HID_REQ_GET_REPORT && rtype == HID_FEATURE_REPORT) {
		mutex_lock(&shid->lock);
		ret = spi_hid_sync_request(shid, SPI_HID_OUT_GET_FEATURE,
					   reportnum, NULL, 0);
		mutex_unlock(&shid->lock);
		if (ret) {
			dev_err(dev, "GET_FEATURE 0x%02x failed: %d\n",
				reportnum, ret);
			return ret;
		}

		if (shid->resp_type == SPI_HID_REPORT_TYPE_RESET_RESP) {
			memset(buf, 0, len);
			buf[0] = reportnum;
			return len;
		}

		if (shid->resp_type != SPI_HID_REPORT_TYPE_GET_FEATURE_RESP) {
			dev_err(dev, "GET_FEATURE got resp type %d\n",
				shid->resp_type);
			return -EIO;
		}

		copy_len = shid->resp_len - SPI_HID_BODY_HEADER_LEN;
		if (copy_len <= 0) {
			dev_err(dev, "GET_FEATURE 0x%02x: empty response\n",
				reportnum);
			return -EIO;
		}
		if (copy_len > (int)len - 1)
			copy_len = (int)len - 1;

		buf[0] = reportnum;
		memcpy(buf + 1, shid->resp_buf + SPI_HID_BODY_HEADER_LEN,
		       copy_len);
		return 1 + copy_len;
	}

	if (reqtype == HID_REQ_SET_REPORT) {
		mutex_lock(&shid->lock);
		ret = spi_hid_send_output(shid, SPI_HID_OUT_OUTPUT_REPORT,
					  buf[0], buf + 1, len - 1);
		mutex_unlock(&shid->lock);
		return ret ? ret : len;
	}

	dev_err(dev, "Unsupported request: reqtype=%d rtype=%d\n",
		reqtype, rtype);
	return -EIO;
}

static int spi_hid_ll_output_report(struct hid_device *hid,
				    __u8 *buf, size_t len)
{
	struct spi_device *spi = hid->driver_data;
	struct spi_hid *shid = spi_get_drvdata(spi);
	struct device *dev = &spi->dev;
	int ret;

	mutex_lock(&shid->lock);
	if (!shid->ready) {
		dev_err(dev, "output_report called in unready state\n");
		ret = -ENODEV;
		goto out;
	}

	ret = spi_hid_send_output(shid, SPI_HID_OUT_OUTPUT_REPORT,
				  buf[0], &buf[1], len - 1);
	if (ret == 0)
		ret = len;

out:
	mutex_unlock(&shid->lock);
	return ret;
}

static struct hid_ll_driver spi_hid_ll_driver = {
	.start = spi_hid_ll_start,
	.stop = spi_hid_ll_stop,
	.open = spi_hid_ll_open,
	.close = spi_hid_ll_close,
	.power = spi_hid_ll_power,
	.parse = spi_hid_ll_parse,
	.output_report = spi_hid_ll_output_report,
	.raw_request = spi_hid_ll_raw_request,
};

static ssize_t ready_show(struct device *dev,
			  struct device_attribute *attr, char *buf)
{
	struct spi_hid *shid = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", shid->ready ? "ready" : "not ready");
}
static DEVICE_ATTR_RO(ready);

static ssize_t bus_error_count_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct spi_hid *shid = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d (%d)\n",
			  shid->bus_error_count, shid->bus_last_error);
}
static DEVICE_ATTR_RO(bus_error_count);

static ssize_t device_initiated_reset_count_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct spi_hid *shid = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", shid->dir_count);
}
static DEVICE_ATTR_RO(device_initiated_reset_count);

static const struct attribute *const spi_hid_attributes[] = {
	&dev_attr_ready.attr,
	&dev_attr_bus_error_count.attr,
	&dev_attr_device_initiated_reset_count.attr,
	NULL
};

static int spi_hid_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct spi_hid *shid;
	unsigned long irqflags;
	int ret;

	if (spi->irq <= 0) {
		dev_err(dev, "Missing IRQ\n");
		return spi->irq ?: -EINVAL;
	}

	shid = devm_kzalloc(dev, sizeof(*shid), GFP_KERNEL);
	if (!shid)
		return -ENOMEM;

	shid->spi = spi;
	shid->power_state = SPI_HID_POWER_MODE_ACTIVE;
	spi_set_drvdata(spi, shid);

	spi->mode = SPI_MODE_0 | SPI_TX_QUAD | SPI_RX_QUAD;
	spi->max_speed_hz = 20000000;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret) {
		dev_err(dev, "SPI setup failed: %d\n", ret);
		return ret;
	}

	shid->resp_buf = kzalloc(SPI_HID_MAX_INPUT_LEN, GFP_KERNEL);
	shid->rd_buf = kzalloc(2048, GFP_KERNEL);
	shid->irq_hdr_tx = kzalloc(SPI_HID_INPUT_HEADER_LEN, GFP_KERNEL);
	shid->irq_hdr_rx = kzalloc(SPI_HID_INPUT_HEADER_LEN, GFP_KERNEL);
	shid->irq_bdy_tx = kzalloc(SPI_HID_MAX_INPUT_LEN, GFP_KERNEL);
	shid->irq_bdy_rx = kzalloc(SPI_HID_MAX_INPUT_LEN, GFP_KERNEL);
	if (!shid->resp_buf || !shid->rd_buf ||
	    !shid->irq_hdr_tx || !shid->irq_hdr_rx ||
	    !shid->irq_bdy_tx || !shid->irq_bdy_rx) {
		ret = -ENOMEM;
		goto err_free;
	}

	qspi_fill_cmd(shid->irq_hdr_tx, SPI_HID_QSPI_READ_OPCODE,
		       SPI_HID_INPUT_HDR_ADDR);
	qspi_fill_cmd(shid->irq_bdy_tx, SPI_HID_QSPI_READ_OPCODE,
		       SPI_HID_INPUT_BDY_ADDR);

	shid->desc.max_input_length = SPI_HID_MAX_INPUT_LEN;

	ret = sysfs_create_files(&dev->kobj, spi_hid_attributes);
	if (ret) {
		dev_err(dev, "sysfs_create_files failed\n");
		goto err_free;
	}

	mutex_init(&shid->lock);
	init_completion(&shid->output_done);
	complete(&shid->output_done);

	INIT_WORK(&shid->reset_work, spi_hid_reset_work);
	INIT_WORK(&shid->create_device_work, spi_hid_create_device_work);
	INIT_WORK(&shid->refresh_device_work, spi_hid_refresh_device_work);

	shid->supply = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(shid->supply)) {
		if (PTR_ERR(shid->supply) == -EPROBE_DEFER) {
			ret = -EPROBE_DEFER;
			goto err_sysfs;
		}
		shid->supply = NULL;
	}

	shid->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(shid->pinctrl)) {
		dev_err(dev, "pinctrl_get failed: %ld\n",
			PTR_ERR(shid->pinctrl));
		ret = PTR_ERR(shid->pinctrl);
		goto err_sysfs;
	}

	shid->pinctrl_reset = pinctrl_lookup_state(shid->pinctrl, "reset");
	if (IS_ERR(shid->pinctrl_reset)) {
		dev_err(dev, "pinctrl 'reset' not found: %ld\n",
			PTR_ERR(shid->pinctrl_reset));
		ret = PTR_ERR(shid->pinctrl_reset);
		goto err_sysfs;
	}

	shid->pinctrl_active = pinctrl_lookup_state(shid->pinctrl, "active");
	if (IS_ERR(shid->pinctrl_active)) {
		dev_err(dev, "pinctrl 'active' not found: %ld\n",
			PTR_ERR(shid->pinctrl_active));
		ret = PTR_ERR(shid->pinctrl_active);
		goto err_sysfs;
	}

	shid->pinctrl_sleep = pinctrl_lookup_state(shid->pinctrl, "sleep");
	if (IS_ERR(shid->pinctrl_sleep))
		shid->pinctrl_sleep = shid->pinctrl_reset;

	irqflags = irq_get_trigger_type(spi->irq) | IRQF_ONESHOT;
	ret = request_threaded_irq(spi->irq, NULL, spi_hid_irq_thread,
				   irqflags, dev_name(&spi->dev), shid);
	if (ret) {
		dev_err(dev, "request_threaded_irq failed: %d\n", ret);
		goto err_sysfs;
	}
	disable_irq(spi->irq);
	shid->irq_enabled = false;

	pm_runtime_enable(dev->parent);
	ret = pm_runtime_get_sync(dev->parent);
	if (ret < 0) {
		dev_warn(dev, "SPI ctrl PM get failed: %d\n", ret);
		pm_runtime_put_noidle(dev->parent);
	}

	/*
	 * Bring-up: hold the device in reset (power off, reset asserted)
	 * for a few ms, then deassert reset and enable the regulator. The
	 * 2 s settle gives the touchpad firmware time to boot before we
	 * arm the IRQ and start talking to it.
	 */
	pinctrl_select_state(shid->pinctrl, shid->pinctrl_reset);
	msleep(SPI_HID_RESET_ASSERT_MS);
	pinctrl_select_state(shid->pinctrl, shid->pinctrl_active);
	spi_hid_power_up(shid);
	msleep(2000);

	pm_runtime_put(dev->parent);

	enable_irq(spi->irq);
	shid->irq_enabled = true;

	return 0;

err_sysfs:
	sysfs_remove_files(&dev->kobj, spi_hid_attributes);
err_free:
	kfree(shid->resp_buf);
	kfree(shid->rd_buf);
	kfree(shid->irq_hdr_tx);
	kfree(shid->irq_hdr_rx);
	kfree(shid->irq_bdy_tx);
	kfree(shid->irq_bdy_rx);
	return ret;
}

static void spi_hid_remove(struct spi_device *spi)
{
	struct spi_hid *shid = spi_get_drvdata(spi);
	struct device *dev = &spi->dev;

	spi_hid_power_down(shid);
	free_irq(spi->irq, shid);
	shid->irq_enabled = false;
	sysfs_remove_files(&dev->kobj, spi_hid_attributes);
	spi_hid_stop_hid(shid);

	kfree(shid->resp_buf);
	kfree(shid->rd_buf);
	kfree(shid->irq_hdr_tx);
	kfree(shid->irq_hdr_rx);
	kfree(shid->irq_bdy_tx);
	kfree(shid->irq_bdy_rx);
}

static const struct of_device_id spi_hid_of_match[] = {
	{ .compatible = "hid-over-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, spi_hid_of_match);

static const struct spi_device_id spi_hid_id_table[] = {
	{ "hid-over-spi", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, spi_hid_id_table);

static struct spi_driver spi_hid_driver = {
	.driver = {
		.name	= "spi_hid",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(spi_hid_of_match),
	},
	.probe		= spi_hid_probe,
	.remove		= spi_hid_remove,
	.id_table	= spi_hid_id_table,
};

module_spi_driver(spi_hid_driver);

MODULE_DESCRIPTION("HID over SPI (HIDSPI v3) QSPI transport driver");
MODULE_LICENSE("GPL");
