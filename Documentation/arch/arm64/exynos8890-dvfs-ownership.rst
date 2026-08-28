.. SPDX-License-Identifier: GPL-2.0-only

============================================
Exynos8890 DVFS and register ownership rules
============================================

The Exynos8890 vendor kernel represents clocks twice: Linux CCF operations are
forwarded into PWRCAL, and PWRCAL owns the register-level DFS transaction.  A
mainline kernel cannot keep that arrangement after registering the same mux,
divider, gate and PLL fields as native CCF clocks.  CCF and PWRCAL otherwise
maintain independent topology, rate cache, reference count and locking state.

The native port therefore uses one writable owner for every physical register
field.  ``CONFIG_EXYNOS8890_CALIBRATION`` retains ECT and ASV as read-only
characterization data; it is not a clock, power-domain, voltage or memory
controller driver.

The legacy ECT decoder still treats the reserved-memory blob as trusted
bootloader input.  Native consumers validate the copied rows they use, but the
decoder is not hardened against arbitrary out-of-bounds offsets or counts in a
malicious blob.  That trust boundary must be revisited before ECT can be
accepted from an untrusted firmware source.

Ownership matrix
================

.. list-table:: Final physical ownership
   :widths: 22 27 25 26
   :header-rows: 1

   * - Physical block or register class
     - Examples
     - Sole writable owner
     - Data/read owner and constraints
   * - Bootloader ECT reserved memory
     - DVFS, ASV, MARGIN, GEN/PSCDC and TIMING blocks
     - None; Linux must never modify it
     - Exynos8890 calibration provider parses and copies it once
   * - Secure ASV fuse SFRs
     - ``0x101e9000`` through ``0x101e9010``
     - Secure firmware only
     - Calibration provider performs legacy register-read SMC calls only
   * - TOP CMU ordinary clock fields
     - BUS PLLs, source muxes, dividers and gates
     - Samsung CCF
     - Aggregate DVFS owners may request CCF operations but must not write the
       same fields directly
   * - Mongoose and Apollo CMUs
     - CPU PLL, main/bypass muxes and CPU-local dividers
     - Respective aggregate CPU-domain clock
     - cpufreq requests one domain rate; leaf transition clocks are not public
       writable policy endpoints
   * - CPU SYSREG voltage-assist fields
     - Mongoose/Apache EMA and Mongoose assist controls
     - CPU voltage-transition owner
     - EMA thresholds and values come from the calibration provider
   * - CPU CMU SMPL fields
     - ``PWR_CTRL4_*`` initialization, trigger and status bits
     - CPU power/cpufreq coordination code
     - Calibration provider supplies masks and values but never applies them
   * - G3D CMU and G3D SYSREG
     - G3D PLL/mux/dividers, EMA and DVS controls
     - Aggregate G3D clock plus GPU devfreq/voltage owner
     - Ordinary unrelated G3D gates remain CCF-owned
   * - INT, CAM and DISP DVFS clock fields
     - Characterized TOP mux/divider columns
     - One aggregate domain clock per domain
     - Unrelated peripheral gates remain normal CCF clocks
   * - MIF PLL and transition-only MIF/CCORE fields
     - Four-channel MIF selectors/dividers, BUS-PLL selector and CCORE path
       used by PSCDC
     - Exynos8890 DMC/devfreq driver
     - CCF may expose stable parents; the six CCORE output gates sharing TOP
       selector words are read-only/critical and enabled only by the DMC owner
   * - TOP/CCORE PSCDC controller
     - ``PSCDC_CTRL*`` and SMC/SCI FIFO command registers
     - Exynos8890 DMC/devfreq driver
     - GEN/PSCDC rows are immutable calibration inputs
   * - Four DREX/SMC channels
     - Timing sets, mode-register commands, pause and Q-channel controls
     - Exynos8890 DMC/devfreq driver
     - All four channels are one atomic frequency domain
   * - Four DDR PHY instances
     - DVFS timing words, training offsets and clock-gating controls
     - Exynos8890 DMC/devfreq driver
     - TIMING rows are raw words interpreted only by that driver
   * - MIF PMIC voltage controls
     - S2MPS16 Buck1 voltage and ``VTH_OFFSET``
     - Regulator core and the typed S2MPS16 regulator API called by DMC
     - DMC raises VTH after entering the safe path before crossing 1.539 GHz
       upward, and lowers it after the final downward switch
   * - PMU DREX calibration words
     - DRAM manufacturer/timing selection key
     - PMU hardware/firmware
     - DMC driver reads the key and asks the calibration provider for a copied
       matching table
   * - PMU local-power and system-power fields
     - CPU/cluster power, generic power domains and system sleep
     - Exynos PM, cpuidle and genpd drivers
     - Clock drivers do not bulk-restore PMU state
   * - Clock context across system sleep
     - CMU PLL/mux/divider/gate state
     - CCF and each dedicated aggregate owner for its private fields
     - No CAL/PWRCAL bulk CMU save-and-restore pass is permitted
   * - APM Cortex-M3
     - Firmware DVFS mailbox and PMU local-power state
     - Exactly one of firmware or Linux
     - The current native design powers APM off and claims Linux ownership

Migration invariants
====================

#. One physical bit field has one writable Linux owner.  A register containing
   fields from different subsystems must use a shared regmap lock or a single
   higher-level owner; independent read-modify-write paths are forbidden.
#. A DVFS transition is one transaction.  Safe-source selection, high-divider
   ordering, PLL programming and lock polling, switch-back, voltage/EMA order,
   and rollback are covered by the domain owner's lock.  Locking each leaf CCF
   call separately is insufficient.
#. A domain is cut over atomically.  Once a native owner is enabled, the
   corresponding PWRCAL DFS objects, bindings and raw access functions are not
   built.  A run-time fallback between native and PWRCAL writers is forbidden.
#. All public clock and OPP rates use Hz and all voltages use microvolts.  The
   generic devfreq ``DEV_PM_QOS_*_FREQUENCY`` ABI is the explicit exception:
   it consumes kHz, so native owners convert only at that boundary.  Raw ECT
   mux selectors, gate states and divider ``ratio - 1`` values remain
   explicitly typed so that they cannot be mistaken for rates.
#. ECT strings identify characterization columns only.  Consumers explicitly
   map them to native clock IDs during probe; global clkdev-name lookup is not
   part of the ABI.
#. The calibration provider may map ECT memory, copy it, and issue secure read
   SMC calls.  It contains no ``writel()``, regmap write, CCF setter, regulator
   setter, PMU setter, mailbox command or writable debug interface.
#. OPPs disabled by either the DVFS level mask or the selected ASV table stay
   disabled.  Unsupported fuse group/table combinations fail initialization
   instead of silently selecting group zero.
#. MIF is a four-channel memory-controller transaction, not a collection of
   leaf clocks.  PSCDC, DREX timing, DDR PHY timing, mode-register commands and
   modem arbitration move to the DMC driver together.
#. Hardware completion waits have finite timeouts and propagate errors.  A
   failed transition either restores the previous known-safe state or leaves
   the domain on its documented safe source; it never updates a software rate
   cache as though the transition succeeded.
#. Suspend/resume uses the same owners as run time.  No second component may
   restore overlapping CMU, DMC, DDR PHY or PMU state behind those owners.

The experimental MIF node defaults to the ``performance`` governor.  Until a
native PPMU/interconnect policy and complete display/GPU bandwidth votes exist,
an autonomous low-frequency policy could under-provision memory bandwidth.
The full characterized OPP table remains available to explicit devfreq and QoS
requests for transition validation.

System suspend does not force MIF to the vendor 421 MHz suspend OPP.  The CPUs
remain active during the current s2idle path, so devfreq freezes the currently
constrained MIF rate and DMC verifies that same calibrated state at noirq
resume before normal devfreq policy is restarted.

A PSCDC completion timeout is indeterminate because command issue may have
already changed the live DRAM path.  The DMC owner retains its prepared
Q-channel/direct-control state, stops further DVFS writes, and reports a
critical error instead of guessing a rollback direction.

CP-audio suspend PLL sharing remains disabled.  Enabling it requires a native
modem/audio owner to define the mailbox arbitration and suspend ordering with
the DMC; the PM sequencer must not call the removed PWRCAL handoff callbacks.

Read-only calibration interface
===============================

``<linux/soc/samsung/exynos8890-calibration.h>`` exposes immutable domain OPP
rows, the ordered and typed ECT member matrix, supported limits, CPU
safe-switch/EMA/SMPL metadata, PSCDC rows, and key-selected raw MIF timing
matrices.  Initialization is idempotent so an early built-in clock provider or
a later consumer probe can request it, but transitions only consume data copied
by their owner during initialization.

The MIF timing accessor accepts a nonzero PMU calibration key and applies the
vendor ``key | 1`` timing-table selection.  The keyed-voltage accessor applies
``(key & ~0xff) | 3`` and returns one finalized voltage per MIF OPP.  It adds
the exactly-one-word-per-ASV-level manufacturer margin before the vendor
1,000,000-uV cap and monotonic clipping, applies SSA last, then maps ASV levels
to the corresponding DVFS rows.  Reading the PMU key remains the DMC driver's
responsibility because it owns the memory-controller hardware resources.
If the normalized manufacturer-margin key is absent, the accessor returns the
vendor-compatible unkeyed baseline and identifies it with result key zero.

PSCDC rows are positional and have exactly the MIF OPP row count.  Their first
column is the SCI clock rate normalized from MHz to Hz; it is not a MIF lookup
key.  The remaining columns retain their typed selector or raw field meaning.
