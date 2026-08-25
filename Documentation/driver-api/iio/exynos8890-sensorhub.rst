.. SPDX-License-Identifier: GPL-2.0-only

=========================================
Exynos8890 herolte sensorhub bring-up plan
=========================================

Status
======

herolte's motion/environmental sensors (accelerometer, gyroscope,
magnetometer, barometer) are not individually bus-attached. They are managed
by a companion MCU speaking Samsung's "SSP" (Sensor Sub Processor) RPC
protocol, bridged through the same Broadcom BCM4773 chip that also does GNSS.
Broadcom calls this bridging layer "BBD": one physical SPI link, one
byte-stuffed link-layer framing ("TransportLayer", TL), carrying both GNSS
traffic and SSP RPC records multiplexed by RPC id.

``drivers/gnss/bcm4773.c`` already owns the SPI transport and GNSS function.
This document adds a TransportLayer TX encoder and a small exported RPC API
to that driver, and introduces a new, separate SSP protocol-core driver on
top of it. Both pieces are new and have compiled but **not been validated on
real hardware** — treat first boot as an unvalidated experiment, the same
caveat that applied to the modem rewrite
(:doc:`../wwan/exynos8890-modem`).

Contract authorities
=====================

* Vendor reference driver: ``Cronos_8890/drivers/sensorhub/brcm/`` (SSP core,
  BBD bridge) and ``Cronos_8890/drivers/sensorhub/brcm/bbdpl/`` (BBD/TL
  framing, SPI transaction shape).
* A local reverse-engineering note (not in-tree) cross-checking the vendor
  source against protocol traces; used to fill gaps where the vendor's TX
  encoder is not present in the available source tree (see below).
* Existing herolte DT wiring for the BCM4773 SPI transport
  (``arch/arm64/boot/dts/exynos/exynos8890-herolte.dts``, the
  "BCM4773 GNSS transport bring-up" node group) — reused unchanged for the
  physical bus/GPIO description.

One honest gap: the vendor's real TransportLayer TX encoder
(``transport_layer_c.c``) is not present in the reference tree on this
machine — only the RX decoder (``bbdpl/bbd_rpc_lh.c``) is, and that RX
decoder has been verified byte-exact against the vendor's own embedded
self-test frame. The TX encoder implemented here is derived by symmetry with
that verified RX decoder (same escape table, same CRC scope, same flag/detail
byte layout, mirrored), not transcribed from a TX source file. Treat it as
believed-correct-but-hardware-unverified until the first real TX/RX round
trip succeeds.

Subsystem boundary
===================

``drivers/gnss/bcm4773.c``
  Sole owner of the physical SPI bus and the four handshake GPIOs
  (``enable``, ``host-request``, ``mcu-request``, ``mcu-response``). This is
  a hard architectural constraint, not a preference: the vendor BBD bridge is
  a singleton with file-static, non-reentrant parser state and one shared SSI
  transaction path — two independent drivers cannot each claim the SPI
  device. bcm4773.c therefore owns:

  * SSI-over-SPI half-duplex transactions and the ``mcu-request``/
    ``mcu-response`` hello/bye handshake (already implemented, unchanged by
    this work).
  * TransportLayer RX parsing: escape/SOP/EOP state machine, CRC-8
    validation, flag-detail decoding, RPC sub-record demux by id
    (already implemented; the payload-length accounting bug found during
    review is fixed).
  * TransportLayer TX encoding: the new counterpart to the RX parser,
    producing one escaped, CRC'd, SOP/EOP-bounded frame from a caller-
    supplied RPC id + payload.
  * RPC-id demux ownership: ids in the ``IRpcA``..``IRpcL`` range (GNSS/
    location-engine RPCs, opaque to this driver beyond a couple of
    diagnostic ones such as GetVersion) stay internal; id ``0x21``
    (``IRpcSensorResponse_Data``, MCU→AP sensor/SSP payload) is handed to a
    registered consumer instead of just being counted; id ``0x20``
    (``IRpcSensorRequest_Data``, AP→MCU) is what outbound SSP bytes get
    wrapped in.
  * A small exported API so exactly one sibling driver can attach as the
    "sensor" RPC consumer, mirroring the ``exynos8890_cpctl_get()``/
    ``device_link_add()`` phandle-lookup pattern already used by the modem
    driver split (``drivers/soc/samsung/exynos8890-cpctl.c``):

    - ``bcm4773_get(struct device_node *np)`` / ``bcm4773_put()`` — phandle
      lookup via ``bus_find_device_by_of_node(&spi_bus_type, ...)``,
      ``EPROBE_DEFER`` if the SPI device hasn't bound yet, a
      ``device_link_add()`` for consumer-lifetime safety.
    - ``bcm4773_register_sensor_ops()`` / ``bcm4773_unregister_sensor_ops()``
      — the sibling driver supplies an RX callback invoked with decoded,
      length-validated SSP payload bytes (the existing 2-byte little-endian
      size prefix on RPC id ``0x21`` is already stripped here).
    - ``bcm4773_sensor_send()`` — send raw SSP bytes; wrapped as one
      ``IRpcSensorRequest_Data`` (``0x20``) record inside one TL frame with
      ``Flags = 0`` (no ``FLAG_RELIABLE_PACKET`` — the vendor's own ARQ state
      is dead/unimplemented code even on the reference driver, so there is
      nothing to interoperate with; unreliable delivery is the correct
      starting point, not a shortcut).

``drivers/iio/common/exynos8890-ssp/`` (new)
  Owns the Samsung SSP application-layer protocol: the 9-byte
  ``{cmd, length, options, data}`` command header, matching a response to
  its request, and (in a later phase) decoding unsolicited sensor-data
  reports and presenting them as IIO devices. It knows nothing about SPI,
  GPIOs, or TL framing — it only calls ``bcm4773_sensor_send()`` and receives
  bytes via the registered callback. This mirrors the
  cpctl/sipc-core boundary from the modem work: one driver owns the
  transport and hardware handshake, a separate driver owns the higher-level
  protocol riding on top of it.

Why the RPC-id split, not a shared id
--------------------------------------

The vendor enum (``bbdpl/bbd_rpc_lh.c``) defines ``IRpcSensorRequest_Data``
and ``IRpcSensorResponse_Data`` as two distinct, adjacent ids (``0x20`` and
``0x21``), not one id used bidirectionally. Getting this backwards would make
the first probe silently hang waiting for a response that the MCU would
never associate with a request it never recognized as addressed to it.

Implementation phases
======================

1. TransportLayer TX + an in-driver diagnostic round trip
------------------------------------------------------------

* Implement ``tl_build_frame()``: escape the whole body (SeqId through CRC
  inclusive) byte-for-byte against the same three-value escape table as the
  RX decoder, compute CRC-8 over ``PayloadSize..Payload`` (excluding SeqId
  and the CRC byte itself, matching the RX side), nibble-swap, and bound the
  output to the documented worst case (``2 * body_len + 4``).
* Implement ``bcm4773_rpc_send()`` (fire-and-forget, internal) and use it to
  send a GetVersion request (``BCM4773_RPC_GET_VERSION_REQ``, already
  defined) once at GNSS-open time, purely as a TX/RX round-trip diagnostic.
  Log whatever comes back — do not assume a specific response struct layout
  beyond a minimum length sanity check, since the exact GetVersion response
  fields are not independently verified from source in this tree. This
  step's job is to prove the TX encoder is symmetric with the already
  hardware-plausible RX decoder, not to parse firmware version semantics.

2. Passive SSP transport registration
--------------------------------------

* New driver, DT-instantiated via a phandle to the ``bcm4773`` node.
* Register as the BCM4773 sensor transport consumer without sending WHOAMI or
  any state-changing sensor command from kernel probe.
* Match the stock lifecycle: userspace first downloads the runtime patch,
  observes ``ESW:READY``, and only then asks the MCU whether it is alive.
* No sensor enumeration, IIO channels, unsolicited-report handling, or
  userspace request ABI exists yet.

3. Sensor enumeration and IIO presentation (future work, not in this pass)
-------------------------------------------------------------------------

* ``get_sensor_scanning_info`` (``0xF4``) to discover which sensors are
  actually populated.
* Per-sensor IIO drivers consuming a registration API from the SSP core,
  the same shape as ``exynos8890-sipc-wwan.c`` consuming
  ``exynos8890-sipc-core.c``.
* Unsolicited ``HUB2AP_WRITE`` report dispatch by first-payload-byte
  ``MSG2AP_INST_*`` tag.

Required tests
================

* Phase 1: confirm a GetVersion response frame is received at all (proves
  TX encode, SPI transaction shape, and RX decode are mutually consistent on
  real hardware) — log the raw bytes for manual inspection, no assertion on
  content.
* Phase 2: confirm probe registers the SSP transport consumer without emitting
  WHOAMI, ADD_SENSOR, or REMOVE_SENSOR traffic.
* A later userspace daemon must not issue WHOAMI until the runtime patch is
  complete and ``ESW:READY`` has been observed. A timeout or mismatch remains
  diagnostic and must not affect kernel driver registration.
* Do not proceed to phase 3 (individual sensor drivers, unsolicited report
  parsing, or state-changing sensor-enable commands) until this userspace
  readiness sequence passes reliably on real hardware.
