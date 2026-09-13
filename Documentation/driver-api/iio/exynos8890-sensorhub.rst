.. SPDX-License-Identifier: GPL-2.0-only

Exynos8890 SSP sensor transport
==============================

The Samsung Sensor Sub Processor (SSP) protocol provides access to sensors
managed by the BCM4773 location hub. SSP and GNSS share one SPI connection
and one Broadcom TransportLayer session. They cannot have independent SPI
or packet-sequence owners.

Driver interfaces
-----------------

``bcm4773`` owns the SPI/SSI transport, handshake GPIOs, receive IRQ,
escaping and CRC validation. The platform driver ``exynos8890-ssp`` obtains
that supplier through its ``samsung,transport`` phandle and registers as the
single sensor RPC consumer. A managed device link orders consumer removal
before the transport resources are released. Each successful
``bcm4773_get()`` must be paired with ``bcm4773_put()``.

``bcm4773_register_sensor_ops()`` installs a receive callback and holds a
link reference. Registration can enable the link and receive IRQ but sends
no WHOAMI or sensor command. ``bcm4773_unregister_sensor_ops()`` removes the
callback under the transport lock before releasing that reference.

RPC ``0x20`` identifies sensor requests; ``0x21`` identifies sensor
responses. They are distinct directions, not one bidirectional RPC ID.
The transport validates the entire incoming RPC batch and strips the
sensor response's two-byte little-endian length prefix before invoking the
callback. Callbacks run with the transport I/O lock held and must not block
or call back into the transport API.

Readiness and limitations
------------------------

The SSP consumer only registers the transport. Its receive callback discards
sensor payloads; it does not decode readiness, samples or request responses.
No sensor enumeration, IIO channels, buffers, batching or userspace request
ABI is provided.

A runtime hub patch and a firmware-ready indication are prerequisites for
SSP commands. The kernel has neither a patch-loading interface nor a
coordinated firmware-service ownership handoff. Starting a userspace daemon
does not implement those interfaces or make the discard-only callback a
sensor driver.

``bcm4773_sensor_send()`` returns ``-EOPNOTSUPP`` until reliable request and
acknowledgement handling and firmware readiness have a single owner.
Transport synchronization alone does not establish these prerequisites.
The GNSS transport's startup sequence and cached diagnostic interface are
described in :doc:`../gnss-bcm4773-identification`.
