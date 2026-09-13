// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM4773 GNSS/SSP SPI transport
 *
 * The BCM4773 used by Exynos8890 Galaxy S7 devices multiplexes two logical
 * functions over one physical SPI connection: GNSS and a Samsung Sensor
 * Platform gateway. During identification the kernel is the only TX owner.
 * The GNSS endpoint exposes raw RX only; userspace and sensor TX are blocked
 * until a coordinated firmware-ready/reliable transport session exists.
 *
 * Protocol layering:
 *   SSI framing over SPI ↔ Broadcom TransportLayer escaping/CRC ↔ RPC records
 *
 * The SSI transaction format is based on Samsung's GPL-licensed bcm_gps_spi.
 * TransportLayer RX framing is derived from Broadcom's GPL
 * transport_layer_c.c and bbd_rpc_lh.c. TransportLayer TX framing (frame
 * builder, escaping, CRC scope/order, SeqId handling) and the VersionResponse
 * wire layout were recovered from lhd TransportLayer::BuildAndSendPacket(),
 * SendPacket() and RpcGlobalResponseDecoder::ProcessRpc(), using embedded
 * .gnu_debugdata symbols to identify the corresponding executable code.
 */

#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/gnss.h>
#include <linux/gnss/bcm4773.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/seq_file.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

/* SSI framing constants — [VENDOR] bcm_gps_spi.c */
#define BCM4773_SSI_READ_HD		0x20
#define BCM4773_SSI_WRITE_HD		0x00

/* Samsung limited PIO reads to 254 payload bytes — avoid old DMA corner case */
#define BCM4773_MAX_PAYLOAD		254
#define BCM4773_HELLO_RETRIES		100
#define BCM4773_MAX_DRAIN_FRAMES	128
#define BCM4773_SYNC_WAIT_MS		100
#define BCM4773_SYNC_ATTEMPTS		6
#define BCM4773_TRACE_RECORDS		16
#define BCM4773_TRACE_BYTES		32

/* Cached transfer provenance; not a firmware/service readiness state. */
#define BCM4773_STAGE_AUTOBAUD		1
#define BCM4773_STAGE_PREAMBLE		2
#define BCM4773_STAGE_SYNC		3
#define BCM4773_STAGE_SYNCED		4
#define BCM4773_STAGE_FAILED		5

/* TransportLayer protocol constants — [VENDOR] / [BRCM-GPL] */
#define TL_ESCAPE			0xB0
#define TL_SOP				0x00
#define TL_EOP				0x01
#define TL_ESC_ESC			0x03
#define TL_ESC_XON			0x04
#define TL_ESC_XOFF			0x05

/* TransportLayer receive limits — [VENDOR] bbd_rpc_lh.c */
#define TL_MAX_INCOMING			2048
#define TL_MAX_HEADER_SIZE		14

/* TransportLayer flags — [VENDOR] transport_layer_c.c */
#define TL_FLAG_PACKET_ACK		BIT(0)
#define TL_FLAG_RELIABLE_PACKET	BIT(1)
#define TL_FLAG_RELIABLE_ACK		BIT(2)
#define TL_FLAG_RELIABLE_NACK		BIT(3)
#define TL_FLAG_MSG_LOST		BIT(4)
#define TL_FLAG_MSG_GARBAGE		BIT(5)
#define TL_FLAG_SIZE_EXTENDED		BIT(6)
#define TL_FLAG_EXTENDED		BIT(7)
#define TL_FLAG_INTERNAL_PACKET	BIT(8)
#define TL_FLAG_IGNORE_SEQID		BIT(9)
#define TL_FLAG_KNOWN			(TL_FLAG_PACKET_ACK | \
					 TL_FLAG_RELIABLE_PACKET | \
					 TL_FLAG_RELIABLE_ACK | \
					 TL_FLAG_RELIABLE_NACK | \
					 TL_FLAG_MSG_LOST | TL_FLAG_MSG_GARBAGE | \
					 TL_FLAG_SIZE_EXTENDED | TL_FLAG_EXTENDED | \
					 TL_FLAG_INTERNAL_PACKET | \
					 TL_FLAG_IGNORE_SEQID)

/*
 * RPC ids — [VENDOR] bbdpl/bbd_rpc_lh.c RPC_DEFINITION enum. IRpcSensorRequest
 * (AP->MCU) and IRpcSensorResponse (MCU->AP) are distinct, adjacent ids, not
 * one id used both ways.
 */
#define BCM4773_RPC_SENSOR_REQUEST	0x20
#define BCM4773_RPC_SENSOR_RESPONSE	0x21
#define BCM4773_RPC_GET_VERSION_REQ	0x00
#define BCM4773_RPC_SLEEP_CYCLES_REQ	0x02
#define BCM4773_RPC_VERSION_RESPONSE	0x03
#define BCM4773_RPC_STATE_RESPONSE	0x04

/*
 * VersionResponse (0x03) / StateResponse (0x04) payload layouts — [LHD-RE]
 * disassembled directly from RpcGlobalResponseDecoder::ProcessRpc() and
 * StreamDecoder::GetU32() in lhd (functions identified using its embedded
 * .gnu_debugdata symbols). ProcessRpc() dispatches rpc id 3 to a callback
 * taking exactly 3 unsigned ints, each produced by one GetU32() call in
 * sequence (little-endian, 4 bytes each, no padding) — i.e. VersionResponse
 * is `u32 AsicVersion, u32 RomVersion, u32 PatchLevel` back to back, 12
 * bytes total; that argument order matches lhd's printed status
 * string order "Asic 0x%x Rom %u Patch %u". Id 4 (StateResponse) dispatches
 * to a single-unsigned-int callback, i.e. one raw `u32 State`, 4 bytes.
 */
#define BCM4773_VERSION_RESPONSE_LEN	12
#define BCM4773_STATE_RESPONSE_LEN	4

/*
 * TransportLayer TX limits. The payload cap is a deliberately small bound
 * for internal synchronization traffic (see
 * Documentation/driver-api/iio/exynos8890-sensorhub.rst); it keeps the
 * worst-case escaped frame well under BCM4773_MAX_PAYLOAD so a single SSI
 * transaction always suffices and bcm4773_ssi_write() never needs to loop.
 */
#define TL_MAX_TX_PAYLOAD		64
#define TL_MAX_TX_BODY			(1 /* SeqId */ + 1 /* PayloadSize */ + \
					 1 /* Flags */ + 10 /* flag detail bytes */ + \
					 TL_MAX_TX_PAYLOAD + 1 /* CRC */)
#define TL_MAX_TX_FRAME			(2 * TL_MAX_TX_BODY + 4)

/* Parser states */
#define TL_STATE_WAIT_ESC_SOP		0
#define TL_STATE_WAIT_SOP		1
#define TL_STATE_MSG_COMPLETE		2
#define TL_STATE_WAIT_EOP		3

/* TransportLayer stream parser state for one BCM4773 SPI device. */
struct tl_parser {
	unsigned int state;
	unsigned int rx_len;
	u8 last_rx_seqid;
	u8 tx_seqid; /* our own outgoing seqId counter, wraps at u8 */

	/* Sequence tracking — [VENDOR] transport_layer_c.c */
	u32 valid_frames;
	u32 malformed_frames;
	u16 packets_received;
	u16 remote_packet_lost;
	u16 local_packet_lost;
	u16 remote_garbage_detected;
	u8 expected_seqid;
	bool sync_pending;
	bool sync_valid;
	u8 sync_token;
	u8 sync_tx_seqid;
	u8 sync_peer[7]; /* token + six sequence fields, not ASIC identity */
	u32 sync_requests;
	u32 sync_responses;
	u32 sync_rejected;
	u32 internal_frames;
	u32 sequence_gaps;

	/* Statistics — [VENDOR] bbd_rpc_lh.c */
	u32 sensor_responses;

	u8 rx_buf[TL_MAX_INCOMING + TL_MAX_HEADER_SIZE];
};

/* Cached prefixes of existing SPI transfers, including MISO during writes.
 * Failed transfers have no valid RX bytes. Protected by io_lock.
 */
struct bcm4773_xfer_record {
	u32 stage;
	unsigned long at_jiffies;
	u32 len;
	u32 captured;
	int ret;
	u8 tx[BCM4773_TRACE_BYTES];
	u8 rx[BCM4773_TRACE_BYTES];
};

/*
 * struct bcm4773 — multi-function BCM4773 device context.
 * @spi: physical SPI device handle (single connection)
 * @enable: GPIO — power/enabled control for GNSS front-end
 * @host_req: GPIO — BCM4773 → AP interrupt (data pending)
 * @mcu_req: GPIO — AP → BCM4773 wake/request handshake
 * @mcu_resp: GPIO — BCM4773 → AP ready response handshake
 * @irq: IRQ number derived from host_req GPIO
 * @irq_enabled: whether the IRQ thread is active (controlled by link users)
 * @gdev: Linux GNSS core device handle for GNSS function
 * @parser: TransportLayer framing/escaping/CRC parser state machine
 * @io_lock: serializes drain, TX, and probe/remove operations
 * @link_lock: serializes @link_refcount against concurrent get/put
 * @link_refcount: probe and registered sensor consumers holding the link;
 *		   GNSS cdev open/close are passive and do not hold power
 * @sensor_ops: registered consumer callbacks for IRpcSensorResponse_Data
 *		RPC records, or NULL if no consumer has attached
 * @sensor_priv: opaque context passed back to @sensor_ops->recv()
 */
struct bcm4773 {
	struct spi_device	*spi;

	struct gpio_desc	*enable;
	struct gpio_desc	*host_req;
	struct gpio_desc	*mcu_req;
	struct gpio_desc	*mcu_resp;
	int			irq;
	bool			irq_enabled;

	struct gnss_device	*gdev; /* GNSS function */

	struct tl_parser	parser; /* TransportLayer parser — shared by both functions */

	struct mutex		io_lock; /* serializes SPI access */

	struct mutex		link_lock;
	unsigned int		link_refcount;

	const struct bcm4773_sensor_ops *sensor_ops;
	void			*sensor_priv;
	u32 version_responses;
	u32 asic_version;
	u32 rom_version;
	u32 patch_level;
	struct dentry *debugfs;
	u32 spi_transfers;
	u32 spi_errors;
	u32 trace_next;
	u32 trace_count;
	struct bcm4773_xfer_record trace[BCM4773_TRACE_RECORDS];
	u32 hello_attempts;
	u32 hello_timeouts;
	u32 irq_count;
	int last_mcu_resp;
	int last_host_req;
	int sync_result;
	int preamble_result;
	u32 stage;
	int version_result;
};

/*
 * Broadcom BBD CRC-8 lookup table (identical to vendor's GlUtlCrc::ucCrcTable).
 * [VENDOR] / [BRCM-GPL] crc8bits_c.c — do NOT replace with generic kernel CRC
 * until byte-for-byte equivalence is verified.
 */

static const u8 crc_table[256] = {
	0x00, 0x4d, 0x9a, 0xd7, 0x79, 0x34, 0xe3, 0xae, 0xf2, 0xbf, 0x68, 0x25,
	0x8b, 0xc6, 0x11, 0x5c, 0xa9, 0xe4, 0x33, 0x7e, 0xd0, 0x9d, 0x4a, 0x07,
	0x5b, 0x16, 0xc1, 0x8c, 0x22, 0x6f, 0xb8, 0xf5, 0x1f, 0x52, 0x85, 0xc8,
	0x66, 0x2b, 0xfc, 0xb1, 0xed, 0xa0, 0x77, 0x3a, 0x94, 0xd9, 0x0e, 0x43,
	0xb6, 0xfb, 0x2c, 0x61, 0xcf, 0x82, 0x55, 0x18, 0x44, 0x09, 0xde, 0x93,
	0x3d, 0x70, 0xa7, 0xea, 0x3e, 0x73, 0xa4, 0xe9, 0x47, 0x0a, 0xdd, 0x90,
	0xcc, 0x81, 0x56, 0x1b, 0xb5, 0xf8, 0x2f, 0x62, 0x97, 0xda, 0x0d, 0x40,
	0xee, 0xa3, 0x74, 0x39, 0x65, 0x28, 0xff, 0xb2, 0x1c, 0x51, 0x86, 0xcb,
	0x21, 0x6c, 0xbb, 0xf6, 0x58, 0x15, 0xc2, 0x8f, 0xd3, 0x9e, 0x49, 0x04,
	0xaa, 0xe7, 0x30, 0x7d, 0x88, 0xc5, 0x12, 0x5f, 0xf1, 0xbc, 0x6b, 0x26,
	0x7a, 0x37, 0xe0, 0xad, 0x03, 0x4e, 0x99, 0xd4, 0x7c, 0x31, 0xe6, 0xab,
	0x05, 0x48, 0x9f, 0xd2, 0x8e, 0xc3, 0x14, 0x59, 0xf7, 0xba, 0x6d, 0x20,
	0xd5, 0x98, 0x4f, 0x02, 0xac, 0xe1, 0x36, 0x7b, 0x27, 0x6a, 0xbd, 0xf0,
	0x5e, 0x13, 0xc4, 0x89, 0x63, 0x2e, 0xf9, 0xb4, 0x1a, 0x57, 0x80, 0xcd,
	0x91, 0xdc, 0x0b, 0x46, 0xe8, 0xa5, 0x72, 0x3f, 0xca, 0x87, 0x50, 0x1d,
	0xb3, 0xfe, 0x29, 0x64, 0x38, 0x75, 0xa2, 0xef, 0x41, 0x0c, 0xdb, 0x96,
	0x42, 0x0f, 0xd8, 0x95, 0x3b, 0x76, 0xa1, 0xec, 0xb0, 0xfd, 0x2a, 0x67,
	0xc9, 0x84, 0x53, 0x1e, 0xeb, 0xa6, 0x71, 0x3c, 0x92, 0xdf, 0x08, 0x45,
	0x19, 0x54, 0x83, 0xce, 0x60, 0x2d, 0xfa, 0xb7, 0x5d, 0x10, 0xc7, 0x8a,
	0x24, 0x69, 0xbe, 0xf3, 0xaf, 0xe2, 0x35, 0x78, 0xd6, 0x9b, 0x4c, 0x01,
	0xf4, 0xb9, 0x6e, 0x23, 0x8d, 0xc0, 0x17, 0x5a, 0x06, 0x4b, 0x9c, 0xd1,
	0x7f, 0x32, 0xe5, 0xa8
};

/* CRC helpers — match Broadcom's table-based implementation exactly */

static u8 crc_calc_many(u8 *state, const u8 *data, unsigned int len)
{
	while (len--) {
		*state = crc_table[*state ^ (*data++)];
	}
	return *state;
}

static void tl_parser_init(struct tl_parser *tl)
{
	memset(tl, 0, sizeof(*tl));
	tl->state = TL_STATE_WAIT_ESC_SOP;
	tl->last_rx_seqid = 0xFF;
	tl->expected_seqid = 0x00; /* (last_rx_seqid + 1) modulo 256 */
	/* The first internal synchronization request uses packet SeqId zero. */
}

/*
 * tl_parse_bytes() — feed raw SSI bytes into the TransportLayer parser.
 *
 * Packet boundaries may span multiple SSI reads, including an escape pair.
 */
static void tl_reset(struct tl_parser *tl)
{
	tl->state = TL_STATE_WAIT_ESC_SOP;
	tl->rx_len = 0;
}

static bool tl_walk_rpc_payload(struct tl_parser *tl, const u8 *data,
				size_t len, bool dispatch)
{
	while (len) {
		u16 id, payload_len;
		u8 first;

		first = *data++;
		len--;
		id = first;
		if (first & BIT(7)) {
			if (!len)
				return false;
			id = (first & ~BIT(7)) << 8;
			id |= *data++;
			len--;
		}

		if (!len)
			return false;
		first = *data++;
		len--;
		payload_len = first;
		if (first & BIT(7)) {
			if (!len)
				return false;
			payload_len = (first & ~BIT(7)) << 8;
			payload_len |= *data++;
			len--;
		}

		if (payload_len > len)
			return false;
		if (id == BCM4773_RPC_VERSION_RESPONSE &&
		    payload_len != BCM4773_VERSION_RESPONSE_LEN)
			return false;
		if (id == BCM4773_RPC_STATE_RESPONSE &&
		    payload_len != BCM4773_STATE_RESPONSE_LEN)
			return false;
		if (id == BCM4773_RPC_SENSOR_RESPONSE &&
		    (payload_len < sizeof(__le16) ||
		     ((u16)data[0] | ((u16)data[1] << 8)) !=
		     payload_len - sizeof(__le16)))
			return false;
		if (!dispatch)
			goto next_record;
		if (id == BCM4773_RPC_SENSOR_RESPONSE) {
			u16 ssp_len;
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);

			if (payload_len < sizeof(__le16))
				return false;
			ssp_len = (u16)data[0] | ((u16)data[1] << 8);
			if (ssp_len != payload_len - sizeof(__le16))
				return false;
			tl->sensor_responses++;
			if (bcm->sensor_ops)
				bcm->sensor_ops->recv(bcm->sensor_priv,
						      data + sizeof(__le16),
						      ssp_len);
		} else if (id == BCM4773_RPC_VERSION_RESPONSE &&
			   payload_len == BCM4773_VERSION_RESPONSE_LEN) {
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);
			u32 asic = get_unaligned_le32(&data[0]);
			u32 rom = get_unaligned_le32(&data[4]);
			u32 patch = get_unaligned_le32(&data[8]);

			bcm->asic_version = asic;
			bcm->rom_version = rom;
			bcm->patch_level = patch;
			bcm->version_responses++;

			/*
			 * Validated received identity, not proof of correlation
			 * with our request, a GPIO round trip, remote sync,
			 * reliable delivery, or firmware/GNSS readiness.
			 */
			dev_info_ratelimited(&bcm->spi->dev,
				"BCM4773 ASIC=0x%08x ROM=0x%08x patch=%u\n",
				asic, rom, patch);
		} else if (id == BCM4773_RPC_STATE_RESPONSE &&
			   payload_len == BCM4773_STATE_RESPONSE_LEN) {
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);

			dev_info_ratelimited(&bcm->spi->dev,
				"BCM4773 state=0x%08x\n",
				get_unaligned_le32(data));
		} else {
			/*
			 * Any other RPC (GNSS/location-engine or internal
			 * diagnostic). Not parsed further here; no readiness
			 * or request/response correlation is inferred.
			 */
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);

			dev_dbg_ratelimited(&bcm->spi->dev,
				"RPC id=0x%x len=%u: %*ph\n", id, payload_len,
				(int)min_t(u16, payload_len, 32), data);
		}

next_record:
		data += payload_len;
		len -= payload_len;
	}

	return true;
}

/* Validate the complete RPC batch before publishing identity or callbacks. */
static bool tl_parse_rpc_payload(struct tl_parser *tl, const u8 *data, size_t len)
{
	if (!tl_walk_rpc_payload(tl, data, len, false))
		return false;
	return tl_walk_rpc_payload(tl, data, len, true);
}

/*
 * [LHD-RE] OnInternalPacket, VA 0x2ba28..0x2bcec: opcode 1 followed
 * by seven u8 fields; accept only while syncing and with the current token.
 * Peer RxSeq must identify our actual transmitted request; peer TxSeq must
 * match this frame's SeqId. Retain the remaining reliability fields without
 * creating a reliable sender or treating this as firmware/ASIC identity.
 */
static bool tl_handle_internal_sync(struct tl_parser *tl, const u8 *data,
				    size_t len, u8 seqid)
{
	tl->internal_frames++;
	if (!len || data[0] != 1)
		return true;
	if (len != 8)
		return false;
	if (!tl->sync_pending || data[1] != tl->sync_token ||
	    data[2] != tl->sync_tx_seqid || data[5] != seqid) {
		tl->sync_rejected++;
		return true;
	}
	memcpy(tl->sync_peer, data + 1, sizeof(tl->sync_peer));
	/* Stock deliberately skips one packet SeqId after the peer's RxSeq. */
	tl->tx_seqid = data[2] + 2;
	tl->sync_valid = true;
	tl->sync_pending = false;
	tl->sync_responses++;
	return true;
}

static bool tl_handle_frame(struct device *dev, struct tl_parser *tl)
{
	const u8 *data;
	unsigned int len;
	u16 flags;
	u16 payload_len;
	u8 seqid;
	unsigned int bit;
	u8 crc = 0;

	if (tl->rx_len < 4)
		return false;

	crc_calc_many(&crc, &tl->rx_buf[1], tl->rx_len - 2);
	crc = ((crc & 0x0f) << 4) | ((crc & 0xf0) >> 4);
	if (crc != tl->rx_buf[tl->rx_len - 1]) {
		dev_warn_ratelimited(dev, "TransportLayer CRC mismatch\n");
		return false;
	}

	seqid = tl->rx_buf[0];
	data = &tl->rx_buf[1];
	len = tl->rx_len - 2; /* exclude SeqId and CRC bytes */
	payload_len = *data++;
	flags = *data++;
	len -= 2; /* payload_len and flags bytes just consumed */

	for (bit = 0; bit < 16; bit++) {
		u16 flag = BIT(bit);
		u8 detail;

		if (!(flags & flag))
			continue;
		if (!len || !(flag & TL_FLAG_KNOWN))
			return false;
		detail = *data++; /* consume one detail byte per set flag */
		len--;
		if (flag == TL_FLAG_SIZE_EXTENDED)
			payload_len |= (u16)detail << 8;
		else if (flag == TL_FLAG_EXTENDED)
			flags |= (u16)detail << 8;
	}

	if (payload_len != len)
		return false;

	if (flags & TL_FLAG_INTERNAL_PACKET) {
		if (!tl_handle_internal_sync(tl, data, len, seqid))
			return false;
	} else if (!tl_parse_rpc_payload(tl, data, len)) {
		return false;
	}

	if (!(flags & TL_FLAG_IGNORE_SEQID) && !tl->sync_pending) {
		u8 expected = tl->expected_seqid;
		u32 gap = (seqid - expected) & 0xFF;

		if (gap > 0 && !(flags & TL_FLAG_INTERNAL_PACKET)) {
			tl->local_packet_lost += gap;
			tl->sequence_gaps++;
		}
	}

	if (!(flags & TL_FLAG_MSG_GARBAGE))
		tl->packets_received++;

	tl->last_rx_seqid = seqid;
	tl->expected_seqid = (seqid + 1) & 0xFF;
	tl->valid_frames++;
	return true;
}

/*
 * The read-only /dev/gnssX stream receives unmodified SSI payloads.
 * Only the kernel diagnostic owns TX; this is not a stock lhd BBD ABI.
 */
static void tl_parse_bytes(struct device *dev, struct tl_parser *tl,
			   const u8 *raw, size_t len)
{
	const u8 *p = raw;

	while (p < raw + len) {
		u8 byte = *p++;

		switch (tl->state) {
		case TL_STATE_WAIT_ESC_SOP:
			if (byte == TL_ESCAPE)
				tl->state = TL_STATE_WAIT_SOP;
			break;

		case TL_STATE_WAIT_SOP:
			if (byte == TL_SOP) {
				tl->rx_len = 0;
				tl->state = TL_STATE_MSG_COMPLETE;
			} else if (byte != TL_ESCAPE) {
				tl_reset(tl);
			}
			break;

		case TL_STATE_MSG_COMPLETE:
			if (byte == TL_ESCAPE) {
				tl->state = TL_STATE_WAIT_EOP;
			} else if (tl->rx_len == sizeof(tl->rx_buf)) {
				tl_reset(tl);
			} else {
				tl->rx_buf[tl->rx_len++] = byte;
			}
			break;

		case TL_STATE_WAIT_EOP:
			if (byte == TL_EOP) {
				if (!tl_handle_frame(dev, tl))
					tl->malformed_frames++;
				tl_reset(tl);
			} else if (byte == TL_ESC_ESC || byte == TL_ESC_XON ||
				   byte == TL_ESC_XOFF) {
				if (tl->rx_len == sizeof(tl->rx_buf)) {
					tl_reset(tl);
					break;
				}
				tl->rx_buf[tl->rx_len++] = byte == TL_ESC_ESC ? TL_ESCAPE :
					byte == TL_ESC_XON ? 0x11 : 0x13;
				tl->state = TL_STATE_MSG_COMPLETE;
			} else if (byte == TL_SOP) {
				tl->rx_len = 0;
				tl->state = TL_STATE_MSG_COMPLETE;
			} else if (byte != TL_ESCAPE) {
				tl_reset(tl);
			} else {
				tl->state = TL_STATE_WAIT_SOP;
			}
			break;

		default:
			tl->state = TL_STATE_WAIT_ESC_SOP;
			break;
		}
	}

}

/* ========================== TransportLayer TX encoding ==================== */

/*
 * Encode a bounded TL payload: SeqId, size, flags/details, payload, CRC.
 * Framing follows lhd TransportLayer::BuildAndSendPacket().
 * CRC excludes SeqId, includes unescaped size/flags/details/payload and is
 * nibble-swapped. Body and CRC escape B0/11/13; SOP/EOP remain literal pairs.
 * Only internal sync uses this builder. Its INTERNAL/IGNORE_SEQID details
 * are zero. Packet/reliable ACK piggybacking is not implemented: do not use
 * this as a generic RPC/reliable sender. Returns escaped frame length.
 */
static int tl_build_frame(struct tl_parser *tl, u16 extra_flags,
			  const u8 *rpc, size_t rpc_len, u8 *out)
{
	u8 body[TL_MAX_TX_BODY];
	unsigned int pos = 0;
	unsigned int bit;
	unsigned int escaped;
	unsigned int i;
	u16 flags = extra_flags;
	u8 crc = 0;

	if (rpc_len > TL_MAX_TX_PAYLOAD)
		return -EMSGSIZE;
	if (rpc_len > 0xff)
		flags |= TL_FLAG_SIZE_EXTENDED;
	if (flags > 0xff)
		flags |= TL_FLAG_EXTENDED;

	body[pos++] = tl->tx_seqid;
	body[pos++] = rpc_len & 0xff;
	body[pos++] = flags & 0xff;

	for (bit = 0; bit < 16; bit++) {
		u16 flag = BIT(bit);

		if (!(flags & flag))
			continue;
		if (flag == TL_FLAG_SIZE_EXTENDED)
			body[pos++] = (rpc_len >> 8) & 0xff;
		else if (flag == TL_FLAG_EXTENDED)
			body[pos++] = (flags >> 8) & 0xff;
		else
			body[pos++] = 0; /* no other flags carry real data here */
	}

	memcpy(&body[pos], rpc, rpc_len);
	pos += rpc_len;

	crc_calc_many(&crc, &body[1], pos - 1);
	crc = ((crc & 0x0f) << 4) | ((crc & 0xf0) >> 4);
	body[pos++] = crc;

	escaped = 0;
	out[escaped++] = TL_ESCAPE;
	out[escaped++] = TL_SOP;

	for (i = 0; i < pos; i++) {
		u8 b = body[i];

		if (escaped + 2 > TL_MAX_TX_FRAME)
			return -EMSGSIZE;

		if (b == TL_ESCAPE) {
			out[escaped++] = TL_ESCAPE;
			out[escaped++] = TL_ESC_ESC;
		} else if (b == 0x11) {
			out[escaped++] = TL_ESCAPE;
			out[escaped++] = TL_ESC_XON;
		} else if (b == 0x13) {
			out[escaped++] = TL_ESCAPE;
			out[escaped++] = TL_ESC_XOFF;
		} else {
			out[escaped++] = b;
		}
	}

	if (escaped + 2 > TL_MAX_TX_FRAME)
		return -EMSGSIZE;
	out[escaped++] = TL_ESCAPE;
	out[escaped++] = TL_EOP;

	tl->tx_seqid++; /* wraps naturally at u8 */

	return escaped;
}

/* ========================== SSI SPI transport ============================= */

static int bcm4773_spi_xfer(struct bcm4773 *bcm, const void *tx, void *rx,
		    size_t len)
{
	struct spi_transfer xfer = {
		.tx_buf = tx,
		.rx_buf = rx,
		.len = len,
		.bits_per_word = 8,
	};
	struct bcm4773_xfer_record *record = &bcm->trace[bcm->trace_next];
	int ret;

	ret = spi_sync_transfer(bcm->spi, &xfer, 1);
	bcm->spi_transfers++;
	if (ret)
		bcm->spi_errors++;
	memset(record, 0, sizeof(*record));
	record->stage = bcm->stage;
	record->at_jiffies = jiffies;
	record->len = len;
	record->captured = min_t(size_t, len, BCM4773_TRACE_BYTES);
	record->ret = ret;
	memcpy(record->tx, tx, record->captured);
	if (!ret)
		memcpy(record->rx, rx, record->captured);
	bcm->trace_next = (bcm->trace_next + 1) % BCM4773_TRACE_RECORDS;
	if (bcm->trace_count < BCM4773_TRACE_RECORDS)
		bcm->trace_count++;
	return ret;
}

/*
 * bcm4773_hello() — wake BCM4773 MCU via GPIO handshake. [VENDOR]
 *
 * mcu_req = 1 → wait for mcu_resp == 1 (periodic reset attempts during wait)
 */
static int bcm4773_hello(struct bcm4773 *bcm)
{
	unsigned int count;
	unsigned int retries = 0;

	bcm->hello_attempts++;

	gpiod_set_value_cansleep(bcm->mcu_req, 1);

	for (count = 0; count < BCM4773_HELLO_RETRIES; count++) {
		bcm->last_mcu_resp = gpiod_get_value_cansleep(bcm->mcu_resp);
		if (bcm->last_mcu_resp < 0) {
			gpiod_set_value_cansleep(bcm->mcu_req, 0);
			return bcm->last_mcu_resp;
		}
		if (bcm->last_mcu_resp)
			return 0;

		usleep_range(1000, 1500);

		if (count && !(count % 20) && retries++ < 3) {
			gpiod_set_value_cansleep(bcm->mcu_req, 0);
			usleep_range(1000, 1500);
			gpiod_set_value_cansleep(bcm->mcu_req, 1);
			usleep_range(1000, 1500);
		}
	}

	gpiod_set_value_cansleep(bcm->mcu_req, 0);
	bcm->hello_timeouts++;
	return -ETIMEDOUT;
}

/*
 * bcm4773_bye() — sleep BCM4773 MCU. [VENDOR]
 */
static void bcm4773_bye(struct bcm4773 *bcm)
{
	gpiod_set_value_cansleep(bcm->mcu_req, 0);
}

/*
 * bcm4773_ssi_read() — two-stage half-duplex SSI read. [VENDOR] bcm_gps_spi.c
 */
static int bcm4773_ssi_read(struct bcm4773 *bcm, u8 *payload, size_t *len)
{
	u8 tx[BCM4773_MAX_PAYLOAD + 2] = { 0 };
	u8 rx[BCM4773_MAX_PAYLOAD + 2] = { 0 };
	size_t count;
	int ret;

	tx[0] = BCM4773_SSI_READ_HD;

	/* First transaction: get SSI status and pending payload length */
	ret = bcm4773_spi_xfer(bcm, tx, rx, 2);
	if (ret)
		return ret;

	if (rx[0])
		return -EIO;

	count = rx[1] ? rx[1] : BCM4773_MAX_PAYLOAD;
	count = min_t(size_t, count, BCM4773_MAX_PAYLOAD);

	memset(tx, 0, count + 2);
	memset(rx, 0, count + 2);
	tx[0] = BCM4773_SSI_READ_HD;

	/* Second transaction: get status, length and payload */
	ret = bcm4773_spi_xfer(bcm, tx, rx, count + 2);
	if (ret)
		return ret;

	if (rx[0])
		return -EIO;

	if (rx[1] < count)
		count = rx[1];

	memcpy(payload, &rx[2], count);
	*len = count;

	return 0;
}

/*
 * bcm4773_ssi_write() — half-duplex SSI write. [VENDOR] bcm_gps_spi.c
 */
static int bcm4773_ssi_write(struct bcm4773 *bcm, const u8 *payload, size_t len)
{
	u8 tx[BCM4773_MAX_PAYLOAD + 1] = { 0 };
	u8 rx[BCM4773_MAX_PAYLOAD + 1] = { 0 };

	if (len > BCM4773_MAX_PAYLOAD)
		return -EMSGSIZE;

	tx[0] = BCM4773_SSI_WRITE_HD;
	memcpy(&tx[1], payload, len);

	return bcm4773_spi_xfer(bcm, tx, rx, len + 1);
}

/* ========================== Drain loop (RX path) ========================= */

/*
 * bcm4773_drain_locked() — drain all pending data from BCM4773. [VENDOR]
 *
 * Stock services RX before TX. Validate TransportLayer stream framing while
 * forwarding the unmodified SSI payload to the read-only GNSS endpoint.
 */

static int bcm4773_drain_locked(struct bcm4773 *bcm)
{
	unsigned int frames = 0;
	int ret = 0;

	for (frames = 0; frames < BCM4773_MAX_DRAIN_FRAMES; frames++) {
		u8 payload[BCM4773_MAX_PAYLOAD];
		size_t len;

		bcm->last_host_req = gpiod_get_value_cansleep(bcm->host_req);
		if (bcm->last_host_req < 0)
			return bcm->last_host_req;
		if (!bcm->last_host_req)
			break;

		ret = bcm4773_ssi_read(bcm, payload, &len);
		if (ret)
			break;

		if (!len)
			continue;

		/* Preserve raw RX bytes while validating stream framing. */
		tl_parse_bytes(&bcm->spi->dev, &bcm->parser, payload, len);
		if (gnss_insert_raw(bcm->gdev, payload, len) != len)
			dev_warn_ratelimited(&bcm->spi->dev,
					     "GNSS FIFO overflow\n");
	}

	if (frames == BCM4773_MAX_DRAIN_FRAMES) {
		bcm->last_host_req = gpiod_get_value_cansleep(bcm->host_req);
		if (bcm->last_host_req < 0)
			return bcm->last_host_req;
		if (bcm->last_host_req)
			dev_warn_ratelimited(&bcm->spi->dev,
				"HOST_REQ stayed asserted after %u frames\n",
				frames);
	}

	return ret;
}

/* ========================== Bounded startup sync ========================= */

/*
 * [LHD-RE] AutoBaud (0x12220) writes 16 raw 0x80 bytes to ttyBCM;
 * UpdateDownloadState (0x2d3c0) writes 36 then delays 100 ms. StartRemoteSync
 * and Tick (0x2a378/0x2a524) send tokens 1..6 at 100 ms intervals.
 * These are SSI payloads, not TL RPCs. No MCU reset or firmware upload.
 * Polling budgets do not bound synchronous SPI/lock/scheduler latency.
 */
static void bcm4773_tl_sync(struct bcm4773 *bcm)
{
	u8 preamble[36];
	u8 sync_payload[2] = { 0, 0 };
	u8 frame[TL_MAX_TX_FRAME];
	unsigned long timeout;
	unsigned int attempt;
	int frame_len;
	int ret;

	mutex_lock(&bcm->io_lock);
	bcm->parser.sync_pending = false;
	bcm->parser.sync_valid = false;
	bcm->stage = BCM4773_STAGE_AUTOBAUD;
	ret = bcm4773_hello(bcm);
	if (ret)
		goto out;
	ret = bcm4773_drain_locked(bcm);
	if (ret)
		goto out_bye;

	memset(preamble, 0x80, sizeof(preamble));
	/* Selected stock lhd.conf: LheAutoBaudDelayMS=10. */
	usleep_range(10000, 11000);
	ret = bcm4773_ssi_write(bcm, preamble, 16);
	if (ret)
		goto out_bye;
	usleep_range(10000, 11000);
	bcm->stage = BCM4773_STAGE_PREAMBLE;
	ret = bcm4773_ssi_write(bcm, preamble, sizeof(preamble));
	if (ret)
		goto out_bye;
	msleep(100);
	bcm->preamble_result = 0;
	bcm->stage = BCM4773_STAGE_SYNC;

	for (attempt = 1; attempt <= BCM4773_SYNC_ATTEMPTS; attempt++) {
		/* Drain stale traffic before arming this attempt's token/SeqId. */
		ret = bcm4773_drain_locked(bcm);
		if (ret)
			goto out_bye;
		bcm->parser.sync_token = attempt;
		bcm->parser.sync_tx_seqid = bcm->parser.tx_seqid;
		sync_payload[1] = attempt;
		frame_len = tl_build_frame(&bcm->parser,
				TL_FLAG_INTERNAL_PACKET | TL_FLAG_IGNORE_SEQID,
				sync_payload, sizeof(sync_payload), frame);
		if (frame_len < 0) {
			ret = frame_len;
			goto out_bye;
		}
		ret = bcm4773_ssi_write(bcm, frame, frame_len);
		if (ret)
			goto out_bye;
		bcm->parser.sync_requests++;
		bcm->parser.sync_pending = true;
		timeout = jiffies + msecs_to_jiffies(BCM4773_SYNC_WAIT_MS);
		do {
			ret = bcm4773_drain_locked(bcm);
			if (ret)
				goto out_bye;
			if (bcm->parser.sync_valid)
				goto out_bye;
			usleep_range(1000, 2000);
		} while (time_before(jiffies, timeout));
		bcm->parser.sync_pending = false;
	}
	ret = -ETIMEDOUT;
out_bye:
	bcm4773_bye(bcm);
out:
	if (bcm->stage <= BCM4773_STAGE_PREAMBLE)
		bcm->preamble_result = ret;
	bcm->parser.sync_pending = false;
	bcm->sync_result = ret;
	bcm->stage = ret ? BCM4773_STAGE_FAILED : BCM4773_STAGE_SYNCED;
	mutex_unlock(&bcm->io_lock);

	if (ret)
		dev_dbg(&bcm->spi->dev, "TransportLayer sync failed: %d\n", ret);
}

/*
 * bcm4773_link_up() / bcm4773_link_down() — power the GNSS front-end and
 * arm/disarm HOST_REQ for the probe and registered sensor consumers.
 */
static void bcm4773_link_up(struct bcm4773 *bcm)
{
	mutex_lock(&bcm->io_lock);

	gpiod_set_value_cansleep(bcm->enable, 1);

	bcm->irq_enabled = true;
	enable_irq(bcm->irq);

	mutex_unlock(&bcm->io_lock);
}

static void bcm4773_link_down(struct bcm4773 *bcm)
{
	if (bcm->irq_enabled) {
		bcm->irq_enabled = false;
		disable_irq(bcm->irq);
	}

	mutex_lock(&bcm->io_lock);
	bcm4773_bye(bcm);
	gpiod_set_value_cansleep(bcm->enable, 0);
	mutex_unlock(&bcm->io_lock);
}

/*
 * bcm4773_link_get() / bcm4773_link_put() — reference-counted wrappers
 * around bcm4773_link_up()/down().
 *
 * Probe and sensor registration hold independent references. Releasing
 * either reference must not disable the link while the other still owns it.
 */
static void bcm4773_link_get(struct bcm4773 *bcm)
{
	mutex_lock(&bcm->link_lock);
	if (bcm->link_refcount++ == 0)
		bcm4773_link_up(bcm);
	mutex_unlock(&bcm->link_lock);
}

static void bcm4773_link_put(struct bcm4773 *bcm)
{
	mutex_lock(&bcm->link_lock);
	if (!WARN_ON(bcm->link_refcount == 0) && --bcm->link_refcount == 0)
		bcm4773_link_down(bcm);
	mutex_unlock(&bcm->link_lock);
}

/* ========================== Exported sensor RPC API ======================= */

/**
 * bcm4773_get() - look up the BCM4773 transport referenced by @consumer's
 *		  "samsung,transport" DT phandle
 * @consumer: the requesting device; its of_node must carry the phandle
 *
 * Returns a pointer usable with the rest of this API, ERR_PTR(-EPROBE_DEFER)
 * if the BCM4773 SPI device hasn't bound yet, or another ERR_PTR() on
 * failure. The caller must release the reference with bcm4773_put().
 */
struct bcm4773 *bcm4773_get(struct device *consumer)
{
	struct device_node *node;
	struct device *dev;
	struct bcm4773 *bcm;

	if (!consumer || !consumer->of_node)
		return ERR_PTR(-EINVAL);

	node = of_parse_phandle(consumer->of_node, "samsung,transport", 0);
	if (!node)
		return ERR_PTR(-ENODEV);

	dev = bus_find_device_by_of_node(&spi_bus_type, node);
	of_node_put(node);
	if (!dev)
		return ERR_PTR(-EPROBE_DEFER);

	bcm = spi_get_drvdata(to_spi_device(dev));
	if (!bcm) {
		put_device(dev);
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (!device_link_add(consumer, dev, DL_FLAG_AUTOREMOVE_CONSUMER)) {
		put_device(dev);
		return ERR_PTR(-ENOMEM);
	}

	return bcm;
}
EXPORT_SYMBOL_GPL(bcm4773_get);

void bcm4773_put(struct bcm4773 *bcm)
{
	if (bcm)
		put_device(&bcm->spi->dev);
}
EXPORT_SYMBOL_GPL(bcm4773_put);

int bcm4773_register_sensor_ops(struct bcm4773 *bcm,
				const struct bcm4773_sensor_ops *ops,
				void *priv)
{
	if (!bcm || !ops || !ops->recv)
		return -EINVAL;

	mutex_lock(&bcm->io_lock);
	if (bcm->sensor_ops) {
		mutex_unlock(&bcm->io_lock);
		return -EBUSY;
	}
	bcm->sensor_ops = ops;
	bcm->sensor_priv = priv;
	mutex_unlock(&bcm->io_lock);

	/*
	 * The sensor-hub MCU shares the same physical link as GNSS: keep it
	 * powered for as long as this consumer is registered, independently
	 * of the diagnostic probe's temporary reference (see
	 * bcm4773_link_get()).
	 */
	bcm4773_link_get(bcm);

	return 0;
}
EXPORT_SYMBOL_GPL(bcm4773_register_sensor_ops);

void bcm4773_unregister_sensor_ops(struct bcm4773 *bcm)
{
	if (!bcm)
		return;

	mutex_lock(&bcm->io_lock);
	bcm->sensor_ops = NULL;
	bcm->sensor_priv = NULL;
	mutex_unlock(&bcm->io_lock);

	bcm4773_link_put(bcm);
}
EXPORT_SYMBOL_GPL(bcm4773_unregister_sensor_ops);

/*
 * The sensor API remains present for source compatibility, but transmission
 * is blocked until a coordinated firmware-ready/reliable session exists.
 * Registration and receive callbacks remain available to the SSP consumer.
 */
int bcm4773_sensor_send(struct bcm4773 *bcm, const void *data, size_t len)
{
	/* No firmware-ready/reliable session exists yet. Keep one TX owner. */
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(bcm4773_sensor_send);

/* ========================== IRQ thread (RX path) ========================= */

static irqreturn_t bcm4773_irq_thread(int irq, void *data)
{
	struct bcm4773 *bcm = data;
	int ret;

	mutex_lock(&bcm->io_lock);
	bcm->irq_count++;

	ret = bcm4773_hello(bcm);
	if (!ret)
		ret = bcm4773_drain_locked(bcm);

	bcm4773_bye(bcm);
	mutex_unlock(&bcm->io_lock);

	/*
	 * This records IRQ service only. Polling can drain HOST_REQ before
	 * this thread runs, and logging is rate limited. An absent message
	 * does not prove HOST_REQ never asserted or the chip never answered.
	 */
	dev_info_ratelimited(&bcm->spi->dev, "HOST_REQ IRQ serviced: %d\n",
			     ret);

	if (ret)
		dev_err_ratelimited(&bcm->spi->dev,
			    "receive failed: %d\n", ret);

	return IRQ_HANDLED;
}

/* ========================== GNSS core operations ========================= */

/*
 * Run bounded sync once at bind with a held link reference. Failure remains
 * diagnostic, not a reason to fail driver probe. GetVersion is withheld until
 * its reliable transport/ACK owner is implemented, including after sync.
 */
static void bcm4773_version_probe(struct bcm4773 *bcm)
{
	int ret;

	bcm4773_tl_sync(bcm);
	/* Never send the old flags=0 query, even after validated sync.
	 * Reliable GetVersion/ACK ownership is the next implementation gate.
	 */
	ret = bcm->sync_result ? -EAGAIN : -EOPNOTSUPP;
	mutex_lock(&bcm->io_lock);
	bcm->version_result = ret;
	mutex_unlock(&bcm->io_lock);
	if (ret)
		dev_warn(&bcm->spi->dev,
			 "GetVersion withheld: %d (sync=%d; reliable sender not implemented)\n",
			 ret, bcm->sync_result);
}

static int bcm4773_gnss_open(struct gnss_device *gdev)
{
	/* RX observation only: opening must not restart the kernel session. */
	return 0;
}

static void bcm4773_gnss_close(struct gnss_device *gdev)
{
}

static int bcm4773_gnss_write_raw(struct gnss_device *gdev,
				  const unsigned char *buf, size_t count)
{
	/* Raw userspace TL sequences cannot coexist with kernel-owned TX. */
	return -EOPNOTSUPP;
}

static const struct gnss_operations bcm4773_gnss_ops = {
	.open = bcm4773_gnss_open,
	.close = bcm4773_gnss_close,
	.write_raw = bcm4773_gnss_write_raw,
};

/* No GPIO reads, wake, SPI, FIFO consumption or writes from debugfs.
 * A busy transport returns EBUSY rather than waiting behind synchronous SPI.
 */
static int bcm4773_stats_show(struct seq_file *s, void *unused)
{
	struct bcm4773 *bcm = s->private;
	u32 i;

	if (!mutex_trylock(&bcm->io_lock))
		return -EBUSY;
	seq_puts(s, "owner=kernel-identification raw_tx=disabled sensor_tx=disabled\n");
	seq_printf(s, "stage=%u preamble_result=%d version_tx=withheld\n",
		   bcm->stage, bcm->preamble_result);
	seq_printf(s, "spi_transfers=%u spi_errors=%u irq_count=%u\n",
		   bcm->spi_transfers, bcm->spi_errors, bcm->irq_count);
	seq_printf(s, "hello_attempts=%u hello_timeouts=%u cached_mcu_resp=%d cached_host_req=%d\n",
		   bcm->hello_attempts, bcm->hello_timeouts,
		   bcm->last_mcu_resp, bcm->last_host_req);
	seq_printf(s, "sync_rx_result=%d version_result=%d\n",
		   bcm->sync_result, bcm->version_result);
	seq_printf(s, "sync_valid=%u sync_requests=%u sync_responses=%u sync_rejected=%u token=%u request_seq=%u\n",
		   bcm->parser.sync_valid, bcm->parser.sync_requests,
		   bcm->parser.sync_responses, bcm->parser.sync_rejected,
		   bcm->parser.sync_token, bcm->parser.sync_tx_seqid);
	if (bcm->parser.sync_valid)
		seq_printf(s, "sync_peer_token_sequences=%*ph\n",
			   (int)sizeof(bcm->parser.sync_peer), bcm->parser.sync_peer);
	seq_printf(s, "tl_internal=%u sequence_gaps=%u last_rx_seq=%u next_tx_seq=%u\n",
		   bcm->parser.internal_frames, bcm->parser.sequence_gaps,
		   bcm->parser.last_rx_seqid, bcm->parser.tx_seqid);
	seq_printf(s, "tl_valid=%u tl_malformed=%u version_responses=%u\n",
		   bcm->parser.valid_frames, bcm->parser.malformed_frames,
		   bcm->version_responses);
	if (bcm->version_responses)
		seq_printf(s, "unsolicited_asic=0x%08x rom=0x%08x patch=%u\n",
			   bcm->asic_version, bcm->rom_version, bcm->patch_level);
	seq_puts(s, "spi_prefixes_oldest_first (RX invalid when ret != 0):\n");
	for (i = 0; i < bcm->trace_count; i++) {
		u32 slot = (bcm->trace_next + BCM4773_TRACE_RECORDS -
			    bcm->trace_count + i) % BCM4773_TRACE_RECORDS;
		struct bcm4773_xfer_record *r = &bcm->trace[slot];

		seq_printf(s, "stage=%u completed_jiffies=%lu len=%u ret=%d captured=%u tx=%*ph rx=%*ph\n",
			   r->stage, r->at_jiffies, r->len, r->ret, r->captured,
			   (int)r->captured, r->tx, (int)r->captured, r->rx);
	}
	mutex_unlock(&bcm->io_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(bcm4773_stats);

/* ========================== Probe/remove ================================= */

static int bcm4773_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct bcm4773 *bcm;
	struct gnss_device *gdev;
	int ret;

	bcm = devm_kzalloc(dev, sizeof(*bcm), GFP_KERNEL);
	if (!bcm)
		return -ENOMEM;

	bcm->spi = spi;
	bcm->last_mcu_resp = -ENODATA;
	bcm->last_host_req = -ENODATA;
	bcm->sync_result = -ENODATA;
	bcm->preamble_result = -ENODATA;
	bcm->version_result = -ENODATA;
	mutex_init(&bcm->io_lock);
	mutex_init(&bcm->link_lock);
	tl_parser_init(&bcm->parser);

	bcm->enable = devm_gpiod_get(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(bcm->enable))
		return dev_err_probe(dev, PTR_ERR(bcm->enable),
			     "failed to get GPS enable GPIO\n");

	bcm->host_req = devm_gpiod_get(dev, "host-request", GPIOD_IN);
	if (IS_ERR(bcm->host_req))
		return dev_err_probe(dev, PTR_ERR(bcm->host_req),
			     "failed to get HOST_REQ GPIO\n");

	bcm->mcu_req = devm_gpiod_get(dev, "mcu-request", GPIOD_OUT_LOW);
	if (IS_ERR(bcm->mcu_req))
		return dev_err_probe(dev, PTR_ERR(bcm->mcu_req),
			     "failed to get MCU_REQ GPIO\n");

	bcm->mcu_resp = devm_gpiod_get(dev, "mcu-response", GPIOD_IN);
	if (IS_ERR(bcm->mcu_resp))
		return dev_err_probe(dev, PTR_ERR(bcm->mcu_resp),
			     "failed to get MCU_RESP GPIO\n");

	bcm->irq = gpiod_to_irq(bcm->host_req);
	if (bcm->irq < 0)
		return dev_err_probe(dev, bcm->irq,
			     "failed to map HOST_REQ IRQ\n");

	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "failed to setup SPI\n");

	ret = devm_request_threaded_irq(dev, bcm->irq, NULL,
				bcm4773_irq_thread,
				IRQF_TRIGGER_HIGH | IRQF_ONESHOT |
					IRQF_NO_AUTOEN,
				dev_name(dev), bcm);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request HOST_REQ IRQ\n");

	bcm->irq_enabled = false;

	/* GNSS function */
	gdev = gnss_allocate_device(dev);
	if (!gdev)
		return -ENOMEM;

	bcm->gdev = gdev;
	gdev->ops = &bcm4773_gnss_ops;
	gdev->type = GNSS_TYPE_BCM4773;
	gnss_set_drvdata(gdev, bcm);
	spi_set_drvdata(spi, bcm);

	ret = gnss_register_device(gdev);
	if (ret) {
		gnss_put_device(gdev);
		return ret;
	}

	dev_info(dev, "BCM4773 kernel identification transport registered as %s\n",
		 dev_name(&gdev->dev));

	/*
	 * One kernel-owned identification attempt per bind, not per cdev open.
	 * Keep the link reference through the entire bounded sync sequence.
	 * No GetVersion TX until a reliable sender is separately implemented.
	 * No firmware-ready, GNSS-navigation or SSP-readiness claim is made.
	 */
	bcm4773_link_get(bcm);
	bcm4773_version_probe(bcm);
	bcm4773_link_put(bcm);

	bcm->debugfs = debugfs_create_dir(dev_name(&gdev->dev), NULL);
	debugfs_create_file("stats", 0400, bcm->debugfs, bcm,
			    &bcm4773_stats_fops);

	return 0;
}

static void bcm4773_remove(struct spi_device *spi)
{
	struct bcm4773 *bcm = spi_get_drvdata(spi);

	debugfs_remove_recursive(bcm->debugfs);
	/* Stop the IRQ thread before freeing its GNSS RX destination. */
	bcm4773_link_down(bcm);
	gnss_deregister_device(bcm->gdev);
	gnss_put_device(bcm->gdev);
}

static const struct of_device_id bcm4773_of_match[] = {
	{ .compatible = "brcm,bcm4773" },
	{ }
};
MODULE_DEVICE_TABLE(of, bcm4773_of_match);

static const struct spi_device_id bcm4773_spi_ids[] = {
	{ "bcm4773" },
	{ }
};
MODULE_DEVICE_TABLE(spi, bcm4773_spi_ids);

static struct spi_driver bcm4773_driver = {
	.probe = bcm4773_probe,
	.remove = bcm4773_remove,
	.id_table = bcm4773_spi_ids,
	.driver = {
		.name = "gnss-bcm4773",
		.of_match_table = bcm4773_of_match,
	},
};
module_spi_driver(bcm4773_driver);

MODULE_DESCRIPTION("Broadcom BCM4773 GNSS/SSP SPI transport");
MODULE_LICENSE("GPL");
