// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * NCI based driver for Samsung S3FWRN5 NFC chip
 *
 * Copyright (C) 2015 Samsung Electronics
 * Robert Baldyga <r.baldyga@samsung.com>
 */

#include <linux/completion.h>
#include <linux/firmware.h>
#include <crypto/sha1.h>

#include "s3fwrn5.h"
#include "firmware.h"

struct s3fwrn5_fw_version {
	__u8 major;
	__u8 build1;
	__u8 build2;
	__u8 target;
};

static int s3fwrn5_fw_send_msg(struct s3fwrn5_fw_info *fw_info,
	struct sk_buff *msg, struct sk_buff **rsp)
{
	struct s3fwrn5_info *info =
		container_of(fw_info, struct s3fwrn5_info, fw_info);
	struct s3fwrn5_fw_header *hdr;
	struct sk_buff *reply;
	unsigned long flags;
	long ret;

	spin_lock_irqsave(&fw_info->rsp_lock, flags);
	if (fw_info->pending) {
		spin_unlock_irqrestore(&fw_info->rsp_lock, flags);
		return -EBUSY;
	}
	reinit_completion(&fw_info->completion);
	fw_info->pending = true;
	spin_unlock_irqrestore(&fw_info->rsp_lock, flags);

	ret = s3fwrn5_write(info, msg);
	if (ret < 0)
		goto out;

	ret = wait_for_completion_interruptible_timeout(
		&fw_info->completion, msecs_to_jiffies(1000));
	if (!ret)
		ret = -ENXIO;

out:
	/* Retire the receive slot even if RX raced a failed write or wait. */
	spin_lock_irqsave(&fw_info->rsp_lock, flags);
	fw_info->pending = false;
	reply = fw_info->rsp;
	fw_info->rsp = NULL;
	spin_unlock_irqrestore(&fw_info->rsp_lock, flags);
	if (ret < 0) {
		kfree_skb(reply);
		return ret;
	}
	if (!reply)
		return -EINVAL;

	if (reply->len < S3FWRN5_FW_HDR_SIZE)
		goto bad_reply;
	hdr = (struct s3fwrn5_fw_header *)reply->data;
	/* Bit 7 carries alternating parity, not the message type. */
	if ((hdr->type & 0x7f) != S3FWRN5_FW_MSG_RSP)
		goto bad_reply;
	if (le16_to_cpu(hdr->len) != reply->len - S3FWRN5_FW_HDR_SIZE)
		goto bad_reply;

	*rsp = reply;
	return 0;

bad_reply:
	kfree_skb(reply);
	return -EPROTO;
}

static int s3fwrn5_fw_prep_msg(struct s3fwrn5_fw_info *fw_info,
	struct sk_buff **msg, u8 type, u8 code, const void *data, u16 len)
{
	struct s3fwrn5_fw_header hdr;
	struct sk_buff *skb;

	hdr.type = type | fw_info->parity;
	hdr.code = code;
	hdr.len = len;

	skb = alloc_skb(S3FWRN5_FW_HDR_SIZE + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	fw_info->parity ^= 0x80;
	skb_put_data(skb, &hdr, S3FWRN5_FW_HDR_SIZE);
	if (len)
		skb_put_data(skb, data, len);

	*msg = skb;

	return 0;
}

static int s3fwrn5_fw_get_bootinfo(struct s3fwrn5_fw_info *fw_info,
	struct s3fwrn5_fw_cmd_get_bootinfo_rsp *bootinfo)
{
	struct sk_buff *msg, *rsp = NULL;
	struct s3fwrn5_fw_header *hdr;
	int ret;

	/* Send GET_BOOTINFO command */

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_CMD,
		S3FWRN5_FW_CMD_GET_BOOTINFO, NULL, 0);
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS) {
		ret = -EINVAL;
		goto out;
	}

	if (rsp->len < S3FWRN5_FW_HDR_SIZE + 10) {
		ret = -EPROTO;
		goto out;
	}
	memset(bootinfo, 0, sizeof(*bootinfo));
	memcpy(bootinfo, rsp->data + S3FWRN5_FW_HDR_SIZE, 10);

out:
	kfree_skb(rsp);
	return ret;
}

static int s3fwrn5_fw_enter_update_mode(struct s3fwrn5_fw_info *fw_info,
	const void *hash_data, u16 hash_size,
	const void *sig_data, u16 sig_size)
{
	struct s3fwrn5_fw_cmd_enter_updatemode args;
	struct sk_buff *msg, *rsp = NULL;
	struct s3fwrn5_fw_header *hdr;
	int ret;

	/* Send ENTER_UPDATE_MODE command */

	args.hashcode_size = hash_size;
	args.signature_size = sig_size;

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_CMD,
		S3FWRN5_FW_CMD_ENTER_UPDATE_MODE, &args, sizeof(args));
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS) {
		ret = -EPROTO;
		goto out;
	}

	kfree_skb(rsp);

	/* Send hashcode data */

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_DATA, 0,
		hash_data, hash_size);
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS) {
		ret = -EPROTO;
		goto out;
	}

	kfree_skb(rsp);

	/* Send signature data */

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_DATA, 0,
		sig_data, sig_size);
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS)
		ret = -EPROTO;

out:
	kfree_skb(rsp);
	return ret;
}

static int s3fwrn5_fw_update_sector(struct s3fwrn5_fw_info *fw_info,
	u32 base_addr, const void *data)
{
	struct s3fwrn5_fw_cmd_update_sector args;
	struct sk_buff *msg, *rsp = NULL;
	struct s3fwrn5_fw_header *hdr;
	int ret, i;

	/* Send UPDATE_SECTOR command */

	args.base_address = base_addr;

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_CMD,
		S3FWRN5_FW_CMD_UPDATE_SECTOR, &args, sizeof(args));
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS) {
		ret = -EPROTO;
		goto err;
	}

	kfree_skb(rsp);

	/* Send data split into 256-byte packets */

	for (i = 0; i < 16; ++i) {
		ret = s3fwrn5_fw_prep_msg(fw_info, &msg,
			S3FWRN5_FW_MSG_DATA, 0, data+256*i, 256);
		if (ret < 0)
			break;

		ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
		kfree_skb(msg);
		if (ret < 0)
			break;

		hdr = (struct s3fwrn5_fw_header *) rsp->data;
		if (hdr->code != S3FWRN5_FW_RET_SUCCESS) {
			ret = -EPROTO;
			goto err;
		}

		kfree_skb(rsp);
	}

	return ret;

err:
	kfree_skb(rsp);
	return ret;
}

static int s3fwrn5_fw_complete_update_mode(struct s3fwrn5_fw_info *fw_info)
{
	struct sk_buff *msg, *rsp = NULL;
	struct s3fwrn5_fw_header *hdr;
	int ret;

	/* Send COMPLETE_UPDATE_MODE command */

	ret = s3fwrn5_fw_prep_msg(fw_info, &msg, S3FWRN5_FW_MSG_CMD,
		S3FWRN5_FW_CMD_COMPLETE_UPDATE_MODE, NULL, 0);
	if (ret < 0)
		return ret;

	ret = s3fwrn5_fw_send_msg(fw_info, msg, &rsp);
	kfree_skb(msg);
	if (ret < 0)
		return ret;

	hdr = (struct s3fwrn5_fw_header *) rsp->data;
	if (hdr->code != S3FWRN5_FW_RET_SUCCESS)
		ret = -EPROTO;

	kfree_skb(rsp);

	return ret;
}

/*
 * Firmware header structure:
 *
 * 0x00 - 0x0B : Date and time string (w/o NUL termination)
 * 0x10 - 0x13 : Firmware version
 * 0x14 - 0x17 : Signature address
 * 0x18 - 0x1B : Signature size
 * 0x1C - 0x1F : Firmware image address
 * 0x20 - 0x23 : Firmware sectors count
 * 0x24 - 0x27 : Custom signature address
 * 0x28 - 0x2B : Custom signature size
 */

#define S3FWRN5_FW_IMAGE_HEADER_SIZE 44

int s3fwrn5_fw_request_firmware(struct s3fwrn5_fw_info *fw_info)
{
	struct s3fwrn5_fw_image *fw = &fw_info->fw;
	u32 sig_off;
	u32 image_off;
	u32 custom_sig_off;
	int ret;

	ret = request_firmware(&fw->fw, fw_info->fw_name,
		&fw_info->ndev->nfc_dev->dev);
	if (ret < 0)
		return ret;

	if (fw->fw->size < S3FWRN5_FW_IMAGE_HEADER_SIZE) {
		release_firmware(fw->fw);
		return -EINVAL;
	}

	memcpy(fw->date, fw->fw->data + 0x00, 12);
	fw->date[12] = '\0';

	memcpy(&fw->version, fw->fw->data + 0x10, 4);

	memcpy(&sig_off, fw->fw->data + 0x14, 4);
	memcpy(&fw->sig_size, fw->fw->data + 0x18, 4);

	memcpy(&image_off, fw->fw->data + 0x1C, 4);
	memcpy(&fw->image_sectors, fw->fw->data + 0x20, 4);

	memcpy(&custom_sig_off, fw->fw->data + 0x24, 4);
	memcpy(&fw->custom_sig_size, fw->fw->data + 0x28, 4);

	/* Validate offsets before forming pointers; wire lengths are u16. */
	if (sig_off > fw->fw->size ||
	    fw->sig_size > fw->fw->size - sig_off ||
	    fw->sig_size > U16_MAX ||
	    custom_sig_off > fw->fw->size ||
	    fw->custom_sig_size > fw->fw->size - custom_sig_off ||
	    fw->custom_sig_size > U16_MAX || image_off >= fw->fw->size) {
		release_firmware(fw->fw);
		return -EINVAL;
	}

	fw->sig = fw->fw->data + sig_off;
	fw->image = fw->fw->data + image_off;
	fw->custom_sig = fw->fw->data + custom_sig_off;

	return 0;
}

static void s3fwrn5_fw_release_firmware(struct s3fwrn5_fw_info *fw_info)
{
	release_firmware(fw_info->fw.fw);
}

static int s3fwrn5_fw_get_base_addr(
	struct s3fwrn5_fw_cmd_get_bootinfo_rsp *bootinfo, u32 *base_addr)
{
	int i;
	static const struct {
		u8 version[4];
		u32 base_addr;
	} match[] = {
		{{0x05, 0x00, 0x00, 0x00}, 0x00005000},
		{{0x05, 0x00, 0x00, 0x01}, 0x00003000},
		{{0x05, 0x00, 0x00, 0x02}, 0x00003000},
		{{0x05, 0x00, 0x00, 0x03}, 0x00003000},
		{{0x05, 0x00, 0x00, 0x05}, 0x00003000}
	};

	for (i = 0; i < ARRAY_SIZE(match); ++i)
		if (bootinfo->hw_version[0] == match[i].version[0] &&
			bootinfo->hw_version[1] == match[i].version[1] &&
			bootinfo->hw_version[3] == match[i].version[3]) {
			*base_addr = match[i].base_addr;
			return 0;
		}

	return -EINVAL;
}

static inline bool
s3fwrn5_fw_is_custom(const struct s3fwrn5_fw_cmd_get_bootinfo_rsp *bootinfo)
{
	return !!bootinfo->hw_version[2];
}

int s3fwrn5_fw_setup(struct s3fwrn5_fw_info *fw_info)
{
	struct device *dev = &fw_info->ndev->nfc_dev->dev;
	struct s3fwrn5_fw_cmd_get_bootinfo_rsp bootinfo;
	int ret;

	/* Get bootloader info */

	ret = s3fwrn5_fw_get_bootinfo(fw_info, &bootinfo);
	if (ret < 0) {
		dev_err(dev, "Failed to get bootinfo, ret=%02x\n", ret);
		goto err;
	}

	/* Match hardware version to obtain firmware base address */

	ret = s3fwrn5_fw_get_base_addr(&bootinfo, &fw_info->base_addr);
	if (ret < 0) {
		dev_err(dev, "Unknown hardware version\n");
		goto err;
	}

	fw_info->sector_size = bootinfo.sector_size;

	fw_info->sig_size = s3fwrn5_fw_is_custom(&bootinfo) ?
		fw_info->fw.custom_sig_size : fw_info->fw.sig_size;
	fw_info->sig = s3fwrn5_fw_is_custom(&bootinfo) ?
		fw_info->fw.custom_sig : fw_info->fw.sig;

	return 0;

err:
	s3fwrn5_fw_release_firmware(fw_info);
	return ret;
}

bool s3fwrn5_fw_check_version(const struct s3fwrn5_fw_info *fw_info, u32 version)
{
	struct s3fwrn5_fw_version *new = (void *) &fw_info->fw.version;
	struct s3fwrn5_fw_version *old = (void *) &version;

	if (new->major != old->major)
		return new->major > old->major;
	if (new->build1 != old->build1)
		return new->build1 > old->build1;

	return new->build2 > old->build2;
}

int s3fwrn5_fw_download(struct s3fwrn5_fw_info *fw_info)
{
	struct device *dev = &fw_info->ndev->nfc_dev->dev;
	struct s3fwrn5_fw_image *fw = &fw_info->fw;
	u8 hash_data[SHA1_DIGEST_SIZE];
	u32 image_size, off;
	int ret;

	/* update_sector() sends exactly sixteen 256-byte data packets. */
	if (fw_info->sector_size != 4096 || !fw->image_sectors ||
	    fw->image_sectors > U32_MAX / fw_info->sector_size ||
	    fw_info->sig_size > U16_MAX)
		return -EINVAL;

	image_size = fw_info->sector_size * fw->image_sectors;
	if (image_size > fw->fw->size -
	    ((const u8 *)fw->image - fw->fw->data) ||
	    image_size > U32_MAX - fw_info->base_addr)
		return -EINVAL;

	/* Compute SHA of firmware data */
	sha1(fw->image, image_size, hash_data);

	/* Firmware update process */

	dev_info(dev, "Firmware update: %s\n", fw_info->fw_name);

	ret = s3fwrn5_fw_enter_update_mode(fw_info, hash_data,
		SHA1_DIGEST_SIZE, fw_info->sig, fw_info->sig_size);
	if (ret < 0) {
		dev_err(dev, "Unable to enter update mode\n");
		return ret;
	}

	for (off = 0; off < image_size; off += fw_info->sector_size) {
		ret = s3fwrn5_fw_update_sector(fw_info,
			fw_info->base_addr + off, fw->image + off);
		if (ret < 0) {
			dev_err(dev, "Firmware update error (code=%d)\n", ret);
			return ret;
		}
	}

	ret = s3fwrn5_fw_complete_update_mode(fw_info);
	if (ret < 0) {
		dev_err(dev, "Unable to complete update mode\n");
		return ret;
	}

	dev_info(dev, "Firmware update: success\n");

	return ret;
}

void s3fwrn5_fw_init(struct s3fwrn5_fw_info *fw_info, const char *fw_name)
{
	fw_info->parity = 0x00;
	spin_lock_init(&fw_info->rsp_lock);
	fw_info->pending = false;
	fw_info->rsp = NULL;
	fw_info->fw.fw = NULL;
	strscpy(fw_info->fw_name, fw_name);
	init_completion(&fw_info->completion);
}

void s3fwrn5_fw_cleanup(struct s3fwrn5_fw_info *fw_info)
{
	s3fwrn5_fw_release_firmware(fw_info);
}

int s3fwrn5_fw_recv_frame(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct s3fwrn5_info *info = nci_get_drvdata(ndev);
	struct s3fwrn5_fw_info *fw_info = &info->fw_info;
	unsigned long flags;

	spin_lock_irqsave(&fw_info->rsp_lock, flags);
	if (!fw_info->pending || fw_info->rsp) {
		spin_unlock_irqrestore(&fw_info->rsp_lock, flags);
		kfree_skb(skb);
		return -EINVAL;
	}

	fw_info->rsp = skb;

	/* Publish completion before the sender can retire or reuse the slot. */
	complete(&fw_info->completion);
	spin_unlock_irqrestore(&fw_info->rsp_lock, flags);

	return 0;
}
