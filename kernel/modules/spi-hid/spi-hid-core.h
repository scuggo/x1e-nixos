/* SPDX-License-Identifier: GPL-2.0 */
/*
 * HID over SPI (HIDSPI v3) transport driver for QSPI touchpads.
 *
 * Based on Microsoft's spi-hid v2 driver.
 * Copyright (c) 2020 Microsoft Corporation
 */

#ifndef SPI_HID_CORE_H
#define SPI_HID_CORE_H

#include <linux/kernel.h>
#include <linux/completion.h>
#include <linux/spi/spi.h>
#include <linux/types.h>

#define SPI_HID_INPUT_HEADER_SYNC_BYTE		0x5A
#define SPI_HID_INPUT_HEADER_VERSION		0x03

#define SPI_HID_QSPI_READ_OPCODE		0xEB
#define SPI_HID_QSPI_WRITE_OPCODE		0xE2
#define SPI_HID_QSPI_CMD_LEN			4

#define SPI_HID_INPUT_HDR_ADDR			0x1000
#define SPI_HID_INPUT_BDY_ADDR			0x1004
#define SPI_HID_OUTPUT_ADDR			0x2000

#define SPI_HID_SUPPORTED_VERSION		0x0300

#define SPI_HID_BODY_HEADER_LEN		4

/* Input report types (device -> host) */
#define SPI_HID_REPORT_TYPE_DATA		0x01
#define SPI_HID_REPORT_TYPE_RESET_RESP		0x03
#define SPI_HID_REPORT_TYPE_COMMAND_RESP	0x04
#define SPI_HID_REPORT_TYPE_GET_FEATURE_RESP	0x05
#define SPI_HID_REPORT_TYPE_DEVICE_DESC		0x07
#define SPI_HID_REPORT_TYPE_REPORT_DESC		0x08
#define SPI_HID_REPORT_TYPE_SET_FEATURE_RESP	0x09
#define SPI_HID_REPORT_TYPE_OUTPUT_REPORT_RESP	0x0A
#define SPI_HID_REPORT_TYPE_GET_INPUT_RESP	0x0B

/* Output report types (host -> device) */
#define SPI_HID_OUT_DEVICE_DESC			0x01
#define SPI_HID_OUT_REPORT_DESC			0x02
#define SPI_HID_OUT_SET_FEATURE			0x03
#define SPI_HID_OUT_GET_FEATURE			0x04
#define SPI_HID_OUT_OUTPUT_REPORT		0x05
#define SPI_HID_OUT_GET_INPUT_REPORT		0x06
#define SPI_HID_OUT_COMMAND_CONTENT		0x07

#define SPI_HID_POWER_MODE_ACTIVE		0x01
#define SPI_HID_POWER_MODE_SLEEP		0x02
#define SPI_HID_POWER_MODE_OFF			0x03

#define SPI_HID_RESET_ASSERT_MS		300
#define SPI_HID_POST_DIR_DELAY_MS		25
#define SPI_HID_RESPONSE_TIMEOUT_MS		2000
#define SPI_HID_MAX_RESET_ATTEMPTS		3
#define SPI_HID_MAX_INIT_RETRIES		10

#define SPI_HID_INPUT_HEADER_LEN		4
#define SPI_HID_MAX_INPUT_LEN			SZ_8K

struct spi_hid_device_desc_raw {
	__le16 wDeviceDescLength;
	__le16 bcdVersion;
	__le16 wReportDescLength;
	__le16 wMaxInputLength;
	__le16 wMaxOutputLength;
	__le16 wMaxFragmentLength;
	__le16 wVendorID;
	__le16 wProductID;
	__le16 wVersionID;
	__le16 wFlags;
	__u8 reserved[4];
} __packed;

/* Parsed device descriptor */
struct spi_hid_device_descriptor {
	u16 hid_version;
	u16 report_descriptor_length;
	u16 max_input_length;
	u16 max_output_length;
	u16 max_fragment_length;
	u16 vendor_id;
	u16 product_id;
	u16 version_id;
	u16 flags;
};

struct spi_hid_input_header {
	u8 version;
	u16 body_len;
	u8 last_frag;
	u8 sync_const;
};

struct spi_hid_body_header {
	u8 report_type;
	u16 content_length;
	u8 content_id;
};

struct spi_hid {
	struct spi_device	*spi;
	struct hid_device	*hid;

	struct spi_hid_device_descriptor desc;

	u8 *resp_buf;
	int resp_len;
	u8 resp_type;

	u8 power_state;
	u8 attempts;

	bool ready;
	bool irq_enabled;

	struct regulator *supply;
	struct pinctrl *pinctrl;
	struct pinctrl_state *pinctrl_reset;
	struct pinctrl_state *pinctrl_active;
	struct pinctrl_state *pinctrl_sleep;
	struct work_struct reset_work;
	struct work_struct create_device_work;
	struct work_struct refresh_device_work;

	struct mutex lock;
	struct completion output_done;

	u32 bus_error_count;
	int bus_last_error;

	u32 dir_count;
	u32 powered;

	u8 *rd_buf;
	int rd_len;

	u8 *irq_hdr_tx;
	u8 *irq_hdr_rx;
	u8 *irq_bdy_tx;
	u8 *irq_bdy_rx;
};

#endif
