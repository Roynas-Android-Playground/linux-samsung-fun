.. SPDX-License-Identifier: GPL-2.0-only

=====================================
Exynos8890 SS310AP modem rewrite plan
=====================================

Status
======

The accompanying sources implement the control, shared-memory, SIPC5, SBD,
WWAN/netdev, and legacy cbd paths described below. They compile and link as a
mutually exclusive replacement for ``modem_v1``, and a full behavioral audit
against the active (non-``#ifdef``-gated-off) ``modem_v1`` code paths has been
completed and fed back into fixes here. ``herolte_defconfig`` now builds this
stack in and disables ``modem_v1``
(``CONFIG_SEC_SIPC_MODEM_IF``/``CONFIG_MCU_IPC``), and
``exynos8890-herolte.dts`` now enables the ``modem-control@20c7800``/``sipc``/
``cbd`` node group and disables the legacy ``modem``/``shmem`` nodes. **This
has not yet been validated on real hardware** — the code compiles, links, and
the audit resolved several would-be-fatal bugs (a boot-time SBD
initialization failure, a missing SIPC5 header on ordinary TX/RX frames, a
``misc_register()`` device-name collision), but none of that substitutes for
an actual boot. Treat the first flash as an unvalidated experiment: keep a
known-good ``modem_v1`` build available to revert to (flip the DT node
``status`` values back and rebuild with the old Kconfig symbols) if the modem
does not come up, and work through the hardware milestones below in order
rather than assuming full functionality from a successful boot.

The existing ``modem_v1`` driver source remains in-tree
(``drivers/misc/modem_v1``, ``drivers/misc/mcu_ipc``) as the executable
contract reference and rollback path; it is Kconfig-disabled, not removed.

Contract authorities
====================

* Pinned Samsung kernel: ``Cronos_8890`` commit
  ``0460c258d6910628410263dc838a81be8bda6776``.
* Effective herolte revision DT and mailbox/channel tables.
* Same-device stock boot, crash, dump, and ``cbd`` logs.
* Existing ``modem_v1`` UAPI, SIPC5 headers, SBD descriptors, and firmware
  security call ordering.

The rewrite must preserve these external contracts while replacing internal
ownership, synchronization, memory, and Linux API usage.

Subsystem boundaries
====================

``exynos8890-cpctl``
  Owns the CP state machine, PMU registers, secure monitor calls, CP_FAIL and
  CP_WDT IRQs, boot/release/reset ordering, and status notification. Also owns
  the 16-doorbell generic-mailbox client and the 64-word ISSR syscon regmap
  (mailbox registers and IRQ dispatch have no separate driver; they are not
  modem policy, so they stay behind the same accessor functions cpctl exports
  to the SIPC transport). No SIPC parsing or userspace ioctl handling belongs
  here.

``exynos8890-sipc5``
  Owns reserved-memory mapping, SIPC5 framing, SBD rings, mailbox client state,
  flow control, WWAN ports, and packet network interfaces. All offsets and ring
  indexes must be range-checked before dereference.

``exynos8890-cbd-compat``
  Owns only legacy device names and ioctls required by Samsung ``cbd``. It
  translates requests into cpctl and SIPC operations and must contain no direct
  PMU, SMC, mailbox, or shared-memory accesses.

Implementation phases
=====================

1. CP control
-------------

* Implement one serialized state machine with legal transition validation.
* Match the vendor SMC register ABI, 32-bit packed return decoding, and the
  required full-system barrier before every call.
* Implement PMU power-down configuration and exact reset/release ordering.
* Register CP_FAIL/CP_WDT with complete unwind and shutdown ordering.
* Validate on/off/status only before enabling firmware loading.

2. Mailbox
----------

* Document every AP-to-CP and CP-to-AP mailbox word and interrupt bit.
* Implement mask, unmask, clear, pending, and raise operations with proper MMIO
  ordering.
* Ensure IRQ teardown precedes channel and controller lifetime teardown.
* Validate doorbells independently before attaching the SIPC transport.

3. Shared memory and SBD
------------------------

* Map only the reserved modem region supplied by DT.
* Keep physical layout, cache policy, and AP/CP ownership explicit.
* Validate all global descriptor, channel, ring, vector, slot, and payload
  ranges using overflow-safe arithmetic.
* Use acquire/release ordering for shared read/write pointers.
* Reject malformed frame lengths, channel IDs, wrap arithmetic, and fragmented
  frame sequences without advancing shared ownership.

4. WWAN presentation
--------------------

* Expose formatted/RFS/control channels as WWAN ports.
* Expose packet-data channels through normal netdevices and NAPI.
* Bound every RX/TX queue and implement backpressure from SBD fullness.
* Keep the SIPC channel object alive across file, port, netdev, NAPI, and queued
  work references.

5. cbd compatibility
--------------------

* Confirm each legacy ioctl payload size for native and compat tasks.
* Copy and validate complete userspace payloads before changing CP state.
* Keep boot image ranges inside the reserved boot region.
* Preserve device-node names and command values but translate errors into
  stable Linux errno values.
* Implement crash/dump and SRINFO only after normal boot is reliable.

Required tests
==============

* Compile-time assertions for every shared and userspace ABI layout.
* KUnit tests for state transitions, SIPC header parsing, frame padding,
  integer overflow, SBD wrap/full/empty arithmetic, malformed descriptor
  rejection, and rollback.
* Fake-MMIO mailbox tests for pending/clear/mask behavior.
* Shared-memory producer/consumer tests with forced wrap and concurrent reset.
* Fault-injection tests for every allocation, IRQ, mailbox, SMC, copy, and
  firmware-loading boundary.
* Hardware milestones in this order: power/status, reset, secure load, release,
  boot mailbox, one formatted channel, one packet channel, crash, dump.
