// SPDX-License-Identifier: GPL-2.0-only
/*
 * Broadcom BCM4773 GNSS/SSP SPI transport
 *
 * The BCM4773 used by Exynos8890 Galaxy S7 devices multiplexes two logical
 * functions over one physical SPI connection: GNSS and a Samsung Sensor
 * Platform gateway. This initial driver preserves the vendor raw-BBD userspace
 * ABI on the GNSS side while establishing a per-device TransportLayer
 * encoder/parser shared by both functions.
 *
 * Protocol layering:
 *   SSI framing over SPI ↔ Broadcom TransportLayer escaping/CRC ↔ RPC records
 *
 * The SSI transaction format is based on Samsung's GPL-licensed bcm_gps_spi.
 * TransportLayer RX framing is derived from Broadcom's GPL
 * transport_layer_c.c and bbd_rpc_lh.c. TransportLayer TX framing (frame
 * builder, escaping, CRC scope/order, SeqId handling) and the VersionResponse
 * wire layout are [LHD-RE]: disassembled directly from
 * TransportLayer::BuildAndSendPacket()/SendPacket() and
 * RpcGlobalResponseDecoder::ProcessRpc() in the userspace `lhd` binary's
 * embedded .gnu_debugdata symbol table — no TX-side source is present in any
 * vendor tree available for this port.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gnss.h>
#include <linux/gnss/bcm4773.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

/* SSI framing constants — [VENDOR] bcm_gps_spi.c */
#define BCM4773_SSI_READ_HD		0x20
#define BCM4773_SSI_WRITE_HD		0x00

/* Samsung limited PIO reads to 254 payload bytes — avoid old DMA corner case */
#define BCM4773_MAX_PAYLOAD		254
#define BCM4773_HELLO_RETRIES		100
#define BCM4773_MAX_DRAIN_FRAMES	128

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
 * StreamDecoder::GetU32() in the uploaded lhd binary's embedded
 * .gnu_debugdata symbol table (functions at file offset 0x28438 and
 * 0x2c344 respectively). ProcessRpc() dispatches rpc id 3 to a callback
 * taking exactly 3 unsigned ints, each produced by one GetU32() call in
 * sequence (little-endian, 4 bytes each, no padding) — i.e. VersionResponse
 * is `u32 AsicVersion, u32 RomVersion, u32 PatchLevel` back to back, 12
 * bytes total; that argument order matches the vendor's own printed status
 * string order "Asic 0x%x Rom %u Patch %u". Id 4 (StateResponse) dispatches
 * to a single-unsigned-int callback, i.e. one raw `u32 State`, 4 bytes.
 */
#define BCM4773_VERSION_RESPONSE_LEN	12
#define BCM4773_STATE_RESPONSE_LEN	4

/*
 * TransportLayer TX limits. The payload cap is a deliberately small bound
 * for the diagnostic/handshake traffic phase 1 sends (see
 * Documentation/driver-api/iio/exynos8890-sensorhub.rst); it keeps the
 * worst-case escaped frame well under BCM4773_MAX_PAYLOAD so a single SSI
 * transaction always suffices and bcm4773_ssi_write() never needs to loop.
 */
#define TL_MAX_TX_PAYLOAD		64
#define TL_MAX_TX_BODY			(1 /* SeqId */ + 1 /* PayloadSize */ + \
					 1 /* Flags */ + 10 /* flag detail bytes */ + \
					 TL_MAX_TX_PAYLOAD + 1 /* CRC */)
#define TL_MAX_TX_FRAME			(2 * TL_MAX_TX_BODY + 4)

/* TransportLayer sequence window — [VENDOR] bbd_rpc_lh.c */
#define TL_MAX_INCOMING_SEQS		150
#define TL_RELIABLE_RETRY_MS		1000
#define TL_RELIABLE_MAX_RETRY		10

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

	/* Statistics — [VENDOR] bbd_rpc_lh.c */
	u32 sensor_responses;

	u8 rx_buf[TL_MAX_INCOMING + TL_MAX_HEADER_SIZE];
};

/*
 * struct bcm4773 — multi-function BCM4773 device context.
 * @spi: physical SPI device handle (single connection)
 * @enable: GPIO — power/enabled control for GNSS front-end
 * @host_req: GPIO — BCM4773 → AP interrupt (data pending)
 * @mcu_req: GPIO — AP → BCM4773 wake/request handshake
 * @mcu_resp: GPIO — BCM4773 → AP ready response handshake
 * @irq: IRQ number derived from host_req GPIO
 * @irq_enabled: whether the IRQ thread is active (controlled by GNSS open)
 * @gdev: Linux GNSS core device handle for GNSS function
 * @parser: TransportLayer framing/escaping/CRC parser state machine
 * @io_lock: serializes drain, TX, and probe/remove operations
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

	const struct bcm4773_sensor_ops *sensor_ops;
	void			*sensor_priv;
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
	tl->expected_seqid = 0x01; /* start expecting seqId=1 */
	tl->tx_seqid = 0x01; /* match the same starting convention for TX */
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

static bool tl_parse_rpc_payload(struct tl_parser *tl, const u8 *data,
				 size_t len)
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

			/*
			 * This is the phase-1 "does the wire protocol work at
			 * all" proof: a correctly decoded VersionResponse can
			 * only happen if GPIO handshake, SSI framing,
			 * TransportLayer TX encode/escape/CRC, and
			 * TransportLayer RX decode/escape/CRC all round-tripped
			 * correctly against real hardware.
			 */
			dev_info(&bcm->spi->dev,
				"BCM4773 ASIC=0x%08x ROM=0x%08x patch=%u\n",
				asic, rom, patch);
		} else if (id == BCM4773_RPC_STATE_RESPONSE &&
			   payload_len == BCM4773_STATE_RESPONSE_LEN) {
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);

			dev_info(&bcm->spi->dev, "BCM4773 state=0x%08x\n",
				get_unaligned_le32(data));
		} else {
			/*
			 * Any other RPC (GNSS/location-engine or internal
			 * diagnostic). Not parsed further here — phase 1 only
			 * needs to prove a round trip happened at all.
			 */
			struct bcm4773 *bcm = container_of(tl, struct bcm4773, parser);

			dev_dbg(&bcm->spi->dev,
				"RPC id=0x%x len=%u: %*ph\n", id, payload_len,
				(int)min_t(u16, payload_len, 32), data);
		}

		data += payload_len;
		len -= payload_len;
	}

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

	if (!(flags & TL_FLAG_IGNORE_SEQID)) {
		u8 expected = tl->expected_seqid;
		u32 gap = (seqid - expected) & 0xFF;

		if (gap > 0 && !(flags & TL_FLAG_INTERNAL_PACKET)) {
			tl->local_packet_lost += gap;
			tl->remote_packet_lost += gap;
		}
	}

	if (!(flags & TL_FLAG_MSG_GARBAGE))
		tl->packets_received++;

	if (payload_len != len)
		return false;

	if (!(flags & TL_FLAG_INTERNAL_PACKET)) {
		if (!tl_parse_rpc_payload(tl, data, len))
			return false;
	}

	tl->last_rx_seqid = seqid;
	tl->expected_seqid = (seqid + 1) & 0xFF;
	tl->valid_frames++;
	return true;
}

/*
 * Consume the stream framing only. The legacy /dev/gnssX ABI still receives
 * unmodified SSI payloads, so lhd retains ownership of TransportLayer TX and
 * reliability until the in-kernel RPC/firmware state machine exists.
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
 * tl_build_rpc_record() — encode one RPC id+payload record.
 *
 * Mirrors tl_parse_rpc_payload()'s decode exactly: a 1-byte id/length,
 * extended to 2 bytes (big-endian, high bit of the first byte set) when the
 * value exceeds 0xff.
 */
static int tl_build_rpc_record(u16 rpc_id, const u8 *payload, size_t len,
			       u8 *out, size_t out_max)
{
	size_t pos = 0;

	if (rpc_id > 0x7fff || len > 0x7fff)
		return -EINVAL;

	if (rpc_id > 0xff) {
		if (out_max < pos + 2)
			return -EMSGSIZE;
		out[pos++] = 0x80 | (rpc_id >> 8);
		out[pos++] = rpc_id & 0xff;
	} else {
		if (out_max < pos + 1)
			return -EMSGSIZE;
		out[pos++] = rpc_id & 0xff;
	}

	if (len > 0xff) {
		if (out_max < pos + 2)
			return -EMSGSIZE;
		out[pos++] = 0x80 | (len >> 8);
		out[pos++] = len & 0xff;
	} else {
		if (out_max < pos + 1)
			return -EMSGSIZE;
		out[pos++] = len & 0xff;
	}

	if (out_max < pos + len)
		return -EMSGSIZE;
	if (len)
		memcpy(&out[pos], payload, len);
	pos += len;

	return pos;
}

/*
 * tl_build_frame() — encode one TransportLayer frame for TX.
 *
 * @tl: parser state, source of the next outgoing seqId
 * @rpc: a fully-formed RPC record (as produced by tl_build_rpc_record()) —
 *       this function wraps it in exactly one TL frame, it does not split
 *       across frames
 * @rpc_len: length of @rpc
 * @out: output buffer, must be at least TL_MAX_TX_FRAME bytes
 *
 * Layout mirrors tl_handle_frame()'s decode exactly: SeqId, PayloadSize,
 * Flags, then one detail byte per set flag bit in bit0->bit15 order (only
 * TL_FLAG_SIZE_EXTENDED's detail byte carries a real value here; this
 * driver never sets any other flag), Payload, CRC-8 — computed over
 * PayloadSize..Payload (SeqId and the CRC byte itself excluded) and
 * nibble-swapped. The whole body (SeqId..CRC inclusive) is then escaped
 * byte-for-byte and bounded by literal, unescaped SOP/EOP marker pairs.
 *
 * [LHD-RE] confirmed byte-for-byte against TransportLayer::BuildAndSendPacket()
 * (lhd .gnu_debugdata offset 0x2a5b4): SOP is a fixed halfword store
 * (`0xB0 0x00`) before the escape loop, not routed through it; SeqId is
 * escape-checked but written before the first Crc8Bits::Update() call; CRC
 * accumulates PayloadSize, Flags, each flag-detail byte, then the raw
 * (unescaped) Payload via a bulk Update(buf,len) call, in that order; the
 * final CRC byte is itself escape-checked before EOP; EOP is a fixed,
 * unescaped `0xB0 0x01` pair. The one confirmed gap here: real firmware also
 * auto-ORs FLAG_PACKET_ACK/FLAG_MSG_LOST/FLAG_MSG_GARBAGE into Flags based on
 * internal RX accounting state before encoding — this driver does not, which
 * is a no-op on the freshly-reset parser state this phase-1 probe runs
 * against, but will need implementing before general reliable/ACK traffic.
 *
 * Returns the number of bytes written to @out, or a negative errno.
 */
static int tl_build_frame(struct tl_parser *tl, const u8 *rpc, size_t rpc_len,
			  u8 *out)
{
	u8 body[TL_MAX_TX_BODY];
	unsigned int pos = 0;
	unsigned int bit;
	unsigned int escaped;
	unsigned int i;
	u16 flags = 0;
	u8 crc = 0;

	if (rpc_len > TL_MAX_TX_PAYLOAD)
		return -EMSGSIZE;
	if (rpc_len > 0xff)
		flags |= TL_FLAG_SIZE_EXTENDED;

	body[pos++] = tl->tx_seqid;
	body[pos++] = rpc_len & 0xff;
	body[pos++] = flags & 0xff;

	for (bit = 0; bit < 16; bit++) {
		u16 flag = BIT(bit);

		if (!(flags & flag))
			continue;
		if (flag == TL_FLAG_SIZE_EXTENDED)
			body[pos++] = (rpc_len >> 8) & 0xff;
		else
			body[pos++] = 0; /* no other flags used by this driver */
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

	return spi_sync_transfer(bcm->spi, &xfer, 1);
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

	gpiod_set_value_cansleep(bcm->mcu_req, 1);

	for (count = 0; count < BCM4773_HELLO_RETRIES; count++) {
		if (gpiod_get_value_cansleep(bcm->mcu_resp))
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
 * forwarding the unmodified SSI payload to the existing raw-BBD endpoint.
 */

static int bcm4773_drain_locked(struct bcm4773 *bcm)
{
	unsigned int frames = 0;
	int ret = 0;

	for (frames = 0; frames < BCM4773_MAX_DRAIN_FRAMES; frames++) {
		u8 payload[BCM4773_MAX_PAYLOAD];
		size_t len;

		if (!gpiod_get_value_cansleep(bcm->host_req))
			break;

		ret = bcm4773_ssi_read(bcm, payload, &len);
		if (ret)
			break;

		if (!len)
			continue;

		/* Preserve the existing raw BBD ABI while validating stream framing. */
		tl_parse_bytes(&bcm->spi->dev, &bcm->parser, payload, len);
		if (gnss_insert_raw(bcm->gdev, payload, len) != len)
			dev_warn_ratelimited(&bcm->spi->dev,
					     "GNSS FIFO overflow\n");
	}

	if (frames == BCM4773_MAX_DRAIN_FRAMES &&
	    gpiod_get_value_cansleep(bcm->host_req))
		dev_warn_ratelimited(&bcm->spi->dev,
			     "HOST_REQ stayed asserted after %u frames\n",
			     frames);

	return ret;
}

/* ========================== RPC send (TX path) ============================ */

/*
 * bcm4773_rpc_send() — encode and transmit one RPC record as one TL frame.
 *
 * Services pending RX first, matching bcm4773_gnss_write_raw()'s existing
 * ordering and the vendor transport's own "RX before TX" servicing rule.
 * Sent unreliably (Flags=0): the vendor's own TL-level ARQ/retry state is
 * dead code even in the reference driver, so there is no remote peer
 * behavior to interoperate with, and a first bring-up probe does not need
 * delivery guarantees beyond "did anything come back at all."
 */
static int bcm4773_rpc_send(struct bcm4773 *bcm, u16 rpc_id,
			    const u8 *payload, size_t len)
{
	u8 rpc_buf[TL_MAX_TX_PAYLOAD];
	u8 frame[TL_MAX_TX_FRAME];
	int rpc_len, frame_len;
	int ret;

	rpc_len = tl_build_rpc_record(rpc_id, payload, len, rpc_buf,
				      sizeof(rpc_buf));
	if (rpc_len < 0)
		return rpc_len;

	mutex_lock(&bcm->io_lock);

	frame_len = tl_build_frame(&bcm->parser, rpc_buf, rpc_len, frame);
	if (frame_len < 0) {
		ret = frame_len;
		goto out;
	}

	ret = bcm4773_hello(bcm);
	if (ret)
		goto out;

	ret = bcm4773_drain_locked(bcm);
	if (ret)
		goto out_bye;

	ret = bcm4773_ssi_write(bcm, frame, frame_len);

out_bye:
	bcm4773_bye(bcm);
out:
	mutex_unlock(&bcm->io_lock);

	return ret;
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
}
EXPORT_SYMBOL_GPL(bcm4773_unregister_sensor_ops);

/*
 * bcm4773_sensor_send() — send raw SSP bytes to the sensor-hub MCU.
 *
 * Wrapped as one IRpcSensorRequest_Data (0x20) RPC record with a leading
 * 2-byte little-endian length prefix, mirroring the 2-byte length prefix
 * this driver already requires and validates on the IRpcSensorResponse_Data
 * (0x21) receive side. The vendor source available for this port does not
 * independently confirm the same prefix applies on the request side — it is
 * inferred from the Request/Response RPC pair sharing one "Data" method
 * name in the vendor's own RPC definition list, which is exactly what
 * phase 1 exists to validate against real hardware. See
 * Documentation/driver-api/iio/exynos8890-sensorhub.rst.
 */
int bcm4773_sensor_send(struct bcm4773 *bcm, const void *data, size_t len)
{
	u8 payload[TL_MAX_TX_PAYLOAD];
	__le16 sz;

	if (!bcm || (!data && len))
		return -EINVAL;
	if (len + sizeof(sz) > sizeof(payload))
		return -EMSGSIZE;

	sz = cpu_to_le16(len);
	memcpy(payload, &sz, sizeof(sz));
	memcpy(payload + sizeof(sz), data, len);

	return bcm4773_rpc_send(bcm, BCM4773_RPC_SENSOR_REQUEST, payload,
				len + sizeof(sz));
}
EXPORT_SYMBOL_GPL(bcm4773_sensor_send);

/* ========================== IRQ thread (RX path) ========================= */

static irqreturn_t bcm4773_irq_thread(int irq, void *data)
{
	struct bcm4773 *bcm = data;
	int ret;

	mutex_lock(&bcm->io_lock);

	ret = bcm4773_hello(bcm);
	if (!ret)
		ret = bcm4773_drain_locked(bcm);

	bcm4773_bye(bcm);
	mutex_unlock(&bcm->io_lock);

	if (ret)
		dev_err_ratelimited(&bcm->spi->dev,
			    "receive failed: %d\n", ret);

	return IRQ_HANDLED;
}

/* ========================== GNSS core operations ========================= */

static int bcm4773_gnss_open(struct gnss_device *gdev)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);

	mutex_lock(&bcm->io_lock);

	gpiod_set_value_cansleep(bcm->enable, 1);

	bcm->irq_enabled = true;
	enable_irq(bcm->irq);

	mutex_unlock(&bcm->io_lock);

	/*
	 * Phase 1 diagnostic round trip (see
	 * Documentation/driver-api/iio/exynos8890-sensorhub.rst): send a
	 * GetVersion request and let whatever comes back be logged by
	 * tl_parse_rpc_payload()'s diagnostic path. This only proves the TX
	 * encoder and RX decoder are mutually consistent on real hardware —
	 * it must never be allowed to fail GNSS open.
	 */
	{
		int ret = bcm4773_rpc_send(bcm, BCM4773_RPC_GET_VERSION_REQ,
					   NULL, 0);
		if (ret)
			dev_warn(&bcm->spi->dev,
				"GetVersion probe failed: %d%s\n", ret,
				ret == -ETIMEDOUT ?
				" (GPIO handshake timeout — mcu_resp never asserted)" :
				"");
	}

	return 0;
}

static void bcm4773_gnss_close(struct gnss_device *gdev)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);

	if (bcm->irq_enabled) {
		bcm->irq_enabled = false;
		disable_irq(bcm->irq);
	}

	mutex_lock(&bcm->io_lock);
	bcm4773_bye(bcm);
	gpiod_set_value_cansleep(bcm->enable, 0);
	mutex_unlock(&bcm->io_lock);
}

static int bcm4773_gnss_write_raw(struct gnss_device *gdev,
				  const unsigned char *buf, size_t count)
{
	struct bcm4773 *bcm = gnss_get_drvdata(gdev);
	size_t len = min_t(size_t, count, BCM4773_MAX_PAYLOAD);
	int ret;

	mutex_lock(&bcm->io_lock);

	ret = bcm4773_hello(bcm);
	if (ret)
		goto out;

	/* Stock transport services pending RX before sending new data. */
	ret = bcm4773_drain_locked(bcm);
	if (ret)
		goto out_bye;

	ret = bcm4773_ssi_write(bcm, buf, len);
	if (!ret)
		ret = len;

out_bye:
	bcm4773_bye(bcm);
out:
	mutex_unlock(&bcm->io_lock);

	return ret;
}

static const struct gnss_operations bcm4773_gnss_ops = {
	.open = bcm4773_gnss_open,
	.close = bcm4773_gnss_close,
	.write_raw = bcm4773_gnss_write_raw,
};

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
	mutex_init(&bcm->io_lock);
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

	dev_info(dev, "BCM4773 raw BBD transport registered as %s\n",
		 dev_name(&gdev->dev));

	return 0;
}

static void bcm4773_remove(struct spi_device *spi)
{
	struct bcm4773 *bcm = spi_get_drvdata(spi);

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
