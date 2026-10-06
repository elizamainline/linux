.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro NFC bring-up
=================================

FroggerPro uses an ST54L controller with raw NCI 2.0 packets on I2C9,
address 0x08. TLMM GPIO7 carries the active-high interrupt, GPIO57 is
active-low reset, and GPIO6 is the controller's clock-request output.
The board uses ``CONFIG_NFC``, ``CONFIG_NFC_NCI`` and
``CONFIG_NFC_ST21NFC_I2C`` as modules.

Initialization and clock request
-------------------------------

The controller sends a power-on ``CORE_RESET_NTF`` with trigger 0x01
after releasing hardware reset. Consume this notification before sending
``CORE_RESET_CMD``; otherwise it can complete the command's request early,
and the subsequent command-reset notification can complete ``CORE_INIT``
before its response arrives.

ST's NFC mode command must follow the first ``CORE_INIT``. The driver's
``post_setup`` callback sends ``2f 02 02 02 01``, waits for the mode response
and reset notification, then initializes the restarted controller again.
The response and notification have separate completions because they can
arrive in either order. Brief I2C NACKs during controller restart are retried
with a bounded delay before reporting a header-read failure.

GPIO6 must be requested as an interrupt input. Requesting its IRQ enables
the TLMM wakeup route, allowing the clock-request signal to reach the
always-on hardware. The downstream ST driver explicitly enables the same
route through ``msm_gpio_mpm_wake_set()``. An input pinctrl state alone does
not enable it. Without the route, ``RF_DISCOVER_RSP`` reports success but
polling produces repeated ``CORE_GENERIC_ERROR_NTF`` status 0xe6, which ST's
HAL identifies as a clock error. The IRQ handler only acknowledges changes;
reference-clock handling is performed by the hardware.

ST reports MIFARE Classic with proprietary RF protocol 0x90. The driver's
``get_rfprotocol`` callback maps it to ``NFC_PROTO_MIFARE_MASK`` so the NCI
core can publish the automatically activated target.

The downstream references are in the LineageOS tree:

* ``hardware/st/nfc/st21nfc/hal_wrapper.cc``: mode-switch ordering and
  clock-error decoding.
* ``kernel/nothing/sm8750-modules/st/opensource/driver/nfc/st21nfc.c``:
  clock-request GPIO routing.
* ``vendor/nothing/FroggerPro/proprietary/vendor/etc/libnfc-hal-st.conf``:
  proprietary MIFARE protocol assignment.

Hardware validation on 2026-10-06
--------------------------------

Tests used the connected phone running ``7.3.0-rc6`` and incrementally built
NFC modules. No kernel-image rebuild or device-tree change was needed.
The original driver failed device enable with mode-command status 0x06.
Correcting initialization restored power-on; enabling the clock-request
route removed the repeated 0xe6 notifications and allowed RF activation.

With a MIFARE Classic card held against the antenna, five consecutive
power-on, discovery, socket-exchange and power-off cycles passed:

* ``nfctool`` reported a discovered tag on each cycle.
* Generic netlink ``NFC_CMD_GET_TARGET`` reported protocol mask 0x04 and
  a consistent four-byte NFC-A identifier.
* An ``AF_NFC`` / ``SOCK_SEQPACKET`` / ``NFC_SOCKPROTO_RAW`` socket connected
  to each target using ``NFC_PROTO_MIFARE``.
* A read-only, unauthenticated block-zero command (``30 00``) reached the
  controller, which returned data packet ``00 00 01 03``. With the NCI
  signed-error fix, the socket correctly returned ``ENOSYS`` for this
  unrecognized controller error instead of a positive receive length.
* The final driver reported no header-read failures during these cycles.

Polling after three seconds of idle also detected the card. Loading the
installed modules through ``modprobe`` repeated discovery and the expected
read-error result. Authenticated MIFARE reads, multiple simultaneous tags,
other tag protocols, secure-element operation and system suspend/resume
have not been validated.

Incremental build and smoke test
-------------------------------

Keep the existing output directory and configuration. For module replacement
on the test kernel, use its release string explicitly::

  make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build \
       KERNELRELEASE=7.3.0-rc6 -j24 \
       net/rfkill/rfkill.ko net/nfc/nfc.ko net/nfc/nci/nci.ko \
       drivers/nfc/st21nfc/st21nfc_i2c.ko

Listing the dependent modules lets modpost resolve their exports while
reusing their existing objects. For this live test, the cached ``vmlinux``
BTF differed from the running kernel's BTF. Test copies of ``nci.ko`` and
``st21nfc_i2c.ko`` had their ``.BTF`` and ``.BTF.ext`` sections removed with
``llvm-objcopy`` before loading. Use matching kernel BTF for normal builds.

After installing matching modules, place a tag against the antenna and run::

  sudo modprobe st21nfc_i2c
  sudo nfctool -d nfc0 --enable
  sudo timeout 10 nfctool -d nfc0 --poll
  sudo nfctool -d nfc0 --disable

The poll command should report ``Targets found for nfc0``. ``nfctool --list``
alone does not enumerate the target list; use the poll event or generic
netlink target dump to confirm detection.
