====================
Exynos8890 PCIe TODO
====================

The first implementation intentionally targets one thing: bring PCIe RC0 up
with the generic DesignWare host framework and enumerate the herolte Broadcom
Wi-Fi endpoint.  It avoids Samsung Android policy/debug infrastructure until
that path is proven on hardware.

Bring-up checklist
==================

* Build with ``CONFIG_PCI_EXYNOS=y`` and include ``pcie-fragment.dtsi`` from
  the herolte DTS.
* Confirm the driver probes without an SError or external abort.
* Confirm the log reaches ``PCIe Gen.1 x1 link up``.
* Confirm ``lspci -nn`` shows the Broadcom endpoint below RC0.
* Confirm MSI allocation/interrupt delivery works.  Exynos8890 carries the
  DWC MSI indication in ``PCIE_IRQ_LEVEL[1]`` on the same physical ELBI IRQ.
* Confirm config reads remain stable once the endpoint enters L0s/L1.

Hardware validation still needed
================================

* Verify the explicit mainline FSYS1 clock set is minimal.  Downstream hid
  most controller fabric clocks behind ``gate_pciewifi0`` and HWACG; the
  mainline Exynos8890 clock driver disables automatic Q-channel/Q-state
  gating, so the bring-up fragment enables the relevant leaf gates directly.
* If link training is unreliable, add a small, bounded PHY/PERST retry around
  the modern DWC link-start path.  Do not restore the downstream ten-retry
  host/enumeration state machine wholesale.
* Keep the initial link capped to Gen1.  Validate Gen2 only after repeated
  cold boots, warm reboots and Wi-Fi traffic are stable.
* Validate INTx fallback even though the intended Wi-Fi path uses MSI.
* Validate link-down handling.  The first driver intentionally omits the
  downstream panic notifier, register dumps, workqueue recovery and BUG_ONs.

Power management
================

* Add system suspend/resume using the modern DWC host lifecycle.
* Add runtime PM once Wi-Fi enumeration is stable.
* Move WLAN_REG_ON ownership away from the provisional ``vpcie-supply``
  coupling if a proper PCI endpoint power-control description (for example
  PCI power control) fits the Broadcom device.
* Add WLAN host-wake/WoWLAN support; downstream uses GPA0-7 for host wake.
* Revisit CLKREQ# low-power pin state once runtime PM exists.

PHY follow-up
=============

* Integrate Exynos8890 OTP PCIe PHY tuning after the fixed downstream PHY
  table is known to train reliably.  The ``fun`` branch already has
  Exynos8890 OTP support; downstream uses magic code ``0x5030`` for RC0 and
  ``0x5031`` for RC1.
* Move the Exynos8890 PHY tables/reset programming to
  ``drivers/phy/samsung/phy-exynos-pcie.c`` after the initial host-driver
  bring-up is stable.  The current in-driver placement deliberately preserves
  downstream sequencing while debugging first hardware access.
* Revisit ASPM/L1SS with the generic PCI core.  Do not port Samsung's Argos
  throughput notifier or 40-second delayed L1SS policy unless a hardware
  requirement is demonstrated.

RC1 and DT cleanup
==================

* Add RC1 only after RC0 works.  Downstream uses DBI ``0x157b0000``, ELBI
  ``0x15670000``, PHY ``0x15640000``, PCS ``0x15650000``, config
  ``0x1e000000``, GIC SPI 205 and PMU PHY control offset ``0x0720``.
* Add/extend the Samsung Exynos PCIe DT schema for
  ``samsung,exynos8890-pcie`` and run ``dtbs_check``.
* Fold the SoC-level RC0 node into ``exynos8890.dtsi`` after bring-up.  Keep
  only board-specific endpoint power and pin wiring in ``exynos8890-herolte.dts``.
* Run ``scripts/checkpatch.pl`` and a clean arm64 build before upstream-style
  submission.
