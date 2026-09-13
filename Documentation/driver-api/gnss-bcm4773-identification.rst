.. SPDX-License-Identifier: GPL-2.0-only

BCM4773 SPI transport and synchronization
=======================================

Hardware and protocol
---------------------

BCM4773 multiplexes GNSS and Samsung SSP sensor traffic over a single SPI
connection. SSI half-duplex transactions carry an escaped Broadcom
TransportLayer stream, which in turn contains RPC records. The GNSS device
name does not imply a UART or an NMEA stream.

The driver owns the enable, host-request, mcu-request and mcu-response GPIOs.
It asserts mcu-request and waits for mcu-response before transferring data.
Host-request indicates pending receive data and supplies the receive IRQ.
SSI reads use separate status/length and payload transactions with command
``0x20``. Writes use command ``0x00``; their received SPI bytes are retained
for diagnostics but are not input to the stream parser. Payloads are limited
to 254 bytes per transaction.

Transport ownership
-------------------

The kernel is the only transmitter. Probe holds the link and receive IRQ
through one bounded synchronization sequence:

* Send 16 raw ``0x80`` bytes, with 10 ms delays before and after the write.
* Send 36 raw ``0x80`` bytes and wait 100 ms.
* Send internal synchronization requests with tokens 1 through 6, allowing
  a 100 ms receive-polling window for each request. Drain stale traffic
  before arming each token and stop on a transport error.

Preambles are SSI payloads, not RPC records. Polling limits do not bound
synchronous SPI, GPIO, lock or scheduler latency. Synchronization failure is
reported diagnostically and does not prevent driver registration.

Opening or closing ``/dev/gnssN`` does not transmit, reset or change power.
The stream returns unmodified SSI receive payloads, not decoded navigation
sentences. Nonempty writes and ``bcm4773_sensor_send()`` return
``-EOPNOTSUPP``. Probe and registered sensor consumers hold independent link
references; opening the GNSS stream does not hold one. With no remaining
link reference, receive interrupts and the enable GPIO are disabled.

The transport has no userspace ownership handoff or BBD patch/control
interface. A userspace firmware service must not compete with the kernel's
TransportLayer sequence owner.

Receive validation
------------------

The parser retains state across SSI boundaries, including split escape
pairs. It validates the CRC, flag details and complete payload length before
publishing a frame. The CRC covers unescaped size, flags, details and
payload, excludes the packet sequence byte, and is nibble-swapped on the
wire. A complete RPC batch is validated before callbacks or cached identity
fields are updated.

An internal synchronization response contains opcode 1 followed by seven
bytes: token, peer last RX packet, peer last packet ACK, peer last reliable
RX, peer next TX packet, peer next reliable TX and peer last reliable ACK.
Acceptance requires a pending request, its matching token, a peer last RX
sequence equal to the transmitted request sequence, and a peer next TX
sequence equal to the response frame sequence. The next local TX sequence
is peer last RX plus two, modulo 256. These packet checks are deliberately
conservative; rejected responses do not establish synchronization.

RPC ``0x21`` delivers sensor-response bytes to one registered consumer,
after validating and removing the two-byte little-endian size prefix.
VersionResponse RPC ``0x03`` contains three little-endian 32-bit values:
ASIC version, ROM version and patch level. An unsolicited response may be
cached, but does not establish request correlation or firmware readiness.

Cached diagnostics
------------------

With ``CONFIG_DEBUG_FS``, ``<debugfs>/gnssN/stats`` is a read-only, mode-0400
view of cached state. Reading it does not sample GPIOs, transfer SPI data,
wake the MCU or consume the GNSS FIFO. A busy transport returns ``-EBUSY``.

``stage`` is 0 before startup, 1 during autobaud, 2 during the download
preamble, 3 during synchronization, 4 after successful synchronization and
5 on failure. ``sync_rx_result=0`` denotes a successful parsed exchange,
not merely a successful SSI drain. ``sync_valid``, token and sequence fields
retain the accepted response; a subsequent transfer error can still make
the overall sequence fail. Counters separate malformed frames and rejected
synchronization responses.

Sixteen transfer records retain stage, completion jiffies, length, result
and up to 32 transmitted and received bytes. Later traffic can evict startup
records. Received bytes from failed transfers are invalid. Counters wrap
and reads do not clear them.

Missing functionality
---------------------

GetVersion transmission is withheld even after synchronization. A reliable
sender needs packet ACK accounting, reliable ACK/NACK handling, retries and
receive duplicate suppression before it can own application requests.
``version_result=-EAGAIN`` means synchronization failed;
``-EOPNOTSUPP`` means the reliable sender is unavailable after successful
synchronization. Neither result is an identity timeout.

Firmware compatibility checks and loading, hub readiness, inner GNSS
configuration, sensor enumeration and navigation output are not implemented.
Successful synchronization alone establishes none of these capabilities.
See :doc:`iio/exynos8890-sensorhub` for the SSP consumer boundary.
