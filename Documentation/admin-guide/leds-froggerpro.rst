.. SPDX-License-Identifier: GPL-2.0-only

FroggerPro Glyph LED bring-up
============================

The Nothing Phone (4a) Pro (FroggerPro / A069P) has 137 white Glyph pixels
connected to an Awinic AW20144 18x8 matrix controller. Initial support uses
the standard LED class with one device per pixel, eight-bit brightness and
software LED triggers. It does not implement the downstream Android
``matrix-leds`` misc device, mmap streaming or proprietary sysfs attributes.

Wiring and downstream evidence
------------------------------

The prebuilt device trees in the LineageOS source tree provide the wiring:

* ``device/nothing/FroggerPro/dtb/sm8750.dtb`` maps
  ``qupv3_se0_i2c`` to the GENI controller at ``0xa80000``, which is
  ``&i2c0`` in the mainline tree.
* ``device/nothing/FroggerPro/dtbo.img`` contains AW20144 nodes at both
  seven-bit addresses ``0x20`` and ``0x21`` on this bus. Both reference
  PMXR2230 GPIO 8 as their enable line. Hardware testing confirmed chip
  ID ``0x74`` at ``0x21``; ``0x20`` NACKs. Mainline selects ``0x21``.
  Do not instantiate both nodes together, since they share the enable GPIO.
* The ``0x20`` node supplies GCCR value 13, channel scaling 255 and maximum
  brightness 255. The ``0x21`` node instead supplies GCCR value 20.
  These are register values, not current limits in microamps: the external
  ISET resistor also determines output current.

The vendor driver is
``kernel/nothing/sm8750/drivers/leds/aw20144/leds-aw20144.c``. It accepts
chip IDs ``0x74`` and ``0x71`` at control-page register ``0x2f``. Its
``mapping_channels[VALID_CHANNEL]`` array supplies the complete pixel map
used by ``eliza-nothing-froggerpro-glyph.dtsi``. The NanoGlyph service also
selects AW20144 and 137 pixels for A069P.

The downstream ``enable-gpio`` cell encodes active-low, but its driver
uses the legacy integer GPIO API and drives the physical line low to
disable and high to enable. Mainline therefore uses ``GPIO_ACTIVE_HIGH``
with the descriptor API. The GPIO pinctrl state sets normal output mode
and initially drives low; it leaves the power-source selection inherited
from firmware because the downstream node does not specify it.

Driver behavior
---------------

``CONFIG_LEDS_AW20144=m`` builds ``leds-aw20144.ko``. The driver validates
the channel map, reads the chip ID, resets the device and initializes all
144 PWM values to zero before enabling outputs. Only the 137 populated
channels have scaling enabled. Channels 89, 90, 107, 108, 125, 126 and 143
remain disabled.

The global current register uses the downstream ``0x21`` setting of 20.
All eight scan switches are active. ``awinic,pwm-frequency`` selects the
nominal PWM clock in Hz; the default is 62500. FroggerPro selects 977 Hz,
matching the vendor driver's PCCR value ``0xc0``. Automatic breathing
remains disabled after reset. The register layout comes from
the AW20144 datasheet and vendor driver; the upstream AW200xx driver has
a different layout and cannot drive this device unchanged.

Every page selection and LED write is serialized. The brightness callback
returns I2C failures, and the driver's cached PWM value changes only after
a successful write. The LED core queues sysfs writes and reports callback
failures in the kernel log; a successful sysfs write is not an I2C completion
notification. System suspend drives EN low. Brightness changes during suspend
update the software cache; resume resets and restores both banks before
enabling outputs. A failed resume leaves EN low, with subsequent writes
cached for a later resume attempt. Driver removal, probe failure and
shutdown also drive EN low.

Expected userspace interface
---------------------------

LED devices are named ``white:indicator-0`` through
``white:indicator-136`` under ``/sys/class/leds/``. The number is the
downstream pixel index, while each DT child's ``reg`` is its physical
AW20144 channel. Brightness ranges from 0 to 255. LED directory enumeration
order is not a pixel ordering; use the numeric suffix.

Rapid updates may be coalesced by the LED core. During stress testing,
its sysfs brightness cache could retain an earlier value even after the
hardware had reached the final requested PWM value. Allow queued updates
to settle before comparing values; use controller readback for diagnostics.
Direct I2C diagnostics must also avoid racing the driver's page selections.

The vendor frame API's integer values are narrowed to one byte before
being written to PWM registers. This driver exposes the native eight-bit
range explicitly. Android applications expecting ``/dev/matrix-leds``
need a separate userspace adaptation.

Incremental build
-----------------

Retain the existing output directory and configuration::

  scripts/config --file /tmp/froggerpro-build/.config --module LEDS_AW20144
  make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 olddefconfig
  make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 W=1 \
       drivers/base/regmap/regmap-i2c.ko drivers/leds/leds-aw20144.ko
  make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
       qcom/eliza-nothing-froggerpro.dtb

The regmap module target supplies its exports to modpost when the existing
symbol table contains only previously built module subsets. Keep the DTB
target in a separate invocation so Kbuild does not split the two module
targets into separate modpost runs. These targets do not rebuild the
kernel image.

Validation status
-----------------

Hardware tests on 2026-10-06 used the connected handset running
``7.3.0-rc6``. Its boot DT still described ``0x20``, so a temporary test
module instantiated a client at ``0x21`` using the existing GPIO and pixel
nodes. A secondary software node supplied the PWM frequency property;
direct diagnostics supplied GCCR 20 while the boot DT still contained 13.
The rebuilt driver was loaded without flashing or rebuilding the kernel
image. Its temporary test copy omitted BTF metadata because the local
build's base BTF differed from the running kernel.

Hardware checks passed:

* Chip ID ``0x74`` at ``0x21``; the initial ``0x20`` probe failed with
  ``-ENXIO``. The driver registered 137 LED class devices.
* Physical readback of all PWM and scaling registers, including the seven
  unused channels, matched the DT map. Whole-panel PWM values 16, 64, 128
  and 255 and an asymmetric 137-pixel frame reached the expected channels.
* The operator confirmed visible illumination at PWM 192, GCCR 20 and
  PCCR ``0xc0``, using both direct frame programming and standard sysfs
  brightness writes. This verifies the stock combination, not the
  independent effect of changing current or PWM frequency.
* Four concurrent writers issued 400 updates and the final physical PWM
  values matched their requests. The software timer trigger produced
  physical PWM values 0 and 128.
* A temporary test helper invoked only the driver's suspend/resume
  callbacks while the phone remained awake. EN went low; brightness changes
  left physical PWM unchanged during suspend. Resume restored the cached
  PWM, scaling and selected frequency. This checks the driver callbacks,
  not full system suspend or wakeup.
* Unbind drove EN low and removed all pixel devices. Reprobe restored all
  137 devices with zero PWM. There were no new runtime kernel errors.

The test client and temporary module were removed after testing, and all
pixels were left off. Deploy the corrected DTB and driver module for normal
operation after reboot. Visible pixel order, calibrated brightness, actual
GPIO voltage and full system suspend/resume remain unverified.

Software checks on 2026-10-06 passed:

* Incremental ARM64 LLVM module build with ``W=1``, including the regmap
  I2C dependency, and incremental FroggerPro DTB build.
* ``dt_binding_check`` for ``leds/awinic,aw20144.yaml`` and validation of
  the compiled DTB against that binding. Including the PMIC GPIO schema
  reports an existing warning about the two UIM level-shift state names;
  the new Glyph state passes.
* Decoding the compiled DTB confirms exactly one AW20144 node, its bus,
  address, enable GPIO, current setting, 137 unique channels and exact
  agreement with the vendor's pixel map.
* A temporary host C harness compiled the driver's initialization,
  brightness and sleep routines against mocked regmap/GPIO operations.
  It checked zero-PWM initialization, concurrent channel writes,
  brightness-write failures, suspend caching, restored brightness and
  each of the 12 failing initialization operations during resume.
  This checks software sequencing, not real I2C or PMIC behavior.
* Strict ``checkpatch.pl`` and ``git diff --check``.
