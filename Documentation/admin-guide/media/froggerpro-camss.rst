.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro CAMSS bring-up
===================================

The initial Eliza (SM7750) support exposes one Titan 970 Lite CSID/VFE
with four raw RDI outputs, four CSIPHY v2.2.1 receivers in D-PHY mode,
two TPG v1.4 generators, and two CCI controllers with four I2C buses.
The FroggerPro device tree enables these blocks, the CSI analog supplies
and the Sony IMX355 ultrawide sensor using the existing mainline driver.

This is a bring-up implementation: device boot, interrupts, DMA and frame
capture still need hardware validation. The other physical sensors, C-PHY,
full TFE processing and ISP image processing are outside the initial
support. The test generators can exercise CSID and VFE without a sensor;
they do not exercise the external PHYs or CCI buses.

Hardware information
--------------------

The camera clock drivers originate from the reference Eliza mainline tree.
Resources and supply assignments come from the SM7750/Kera (SoC ID 659)
base DTB in the FroggerPro LineageOS device tree. Register programming is
derived from the matching Qualcomm camera-kernel sources:

* ``cam_csiphy_2_2_1_hwreg.h`` for D-PHY settings;
* ``cam_ife_csid_lite970.h`` and its Lite 880/980 register tables;
* ``cam_vfe_lite97x.h`` and its Lite 98x bus table;
* ``tpg_hw_v_1_4`` for the generator start command and payload format.

The Lite CSID and VFE share the window at ``0x0ad6d000``. The VFE mapping
starts at ``0x0ad6e000`` so its offsets are relative to the VFE version
register and do not overlap the CSID resource. Raw DMA uses SMMU stream
ID ``0x1c00``. CCI buses use GPIO pairs 70/71, 72/73, 74/75 and 76/77.

Build and boot
--------------

``eliza_defconfig`` enables CAMCC built in and CAMSS, CCI, IMX355 and the
SGM38120 camera PMIC as modules.
With an existing configured output directory, object and DTB checks can
be limited to the affected targets::

    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        drivers/clk/qcom/camcc-eliza.o \
        drivers/clk/qcom/cambistmclkcc-eliza.o \
        drivers/media/platform/qcom/camss/ \
        drivers/i2c/busses/i2c-qcom-cci.o \
        drivers/regulator/sgm38120.o \
        drivers/media/i2c/imx355.o \
        qcom/eliza-nothing-froggerpro.dtb

Object checks do not produce an updated boot image or installable modules.
Boot testing requires a kernel, matching modules and the new board DTB
assembled with the normal device boot workflow. On the device::

    modprobe i2c-qcom-cci
    modprobe sgm38120
    modprobe imx355
    modprobe qcom-camss
    dmesg | grep -Ei 'camss|cam_cc|cci|csid|csiphy|vfe|smmu|defer'
    media-ctl -d /dev/media0 -p

Select the media device whose topology contains ``msm_tpg0``, ``msm_csid0``
and ``msm_vfe0_rdi0``. Device numbering may differ. There should be four
VFE video nodes and four CCI adapters. With the sensor endpoint present,
CAMSS completes media registration after IMX355 binds. If sensor probe
fails, inspect the IMX355 and SGM38120 errors before testing TPG capture.

First capture using TPG0
-----------------------

The following is the proposed first hardware test, using 640 by 480 RAW8
color bars on RDI0. It has not yet been run on FroggerPro. Substitute the
CAMSS media device for ``/dev/media0`` as needed::

    media-ctl -d /dev/media0 -r
    media-ctl -d /dev/media0 -l '"msm_tpg0":0 -> "msm_csid0":0 [1]'
    media-ctl -d /dev/media0 -l '"msm_csid0":1 -> "msm_vfe0_rdi0":0 [1]'
    media-ctl -d /dev/media0 -V '"msm_tpg0":0 [fmt:SBGGR8_1X8/640x480]'
    media-ctl -d /dev/media0 -V '"msm_csid0":0 [fmt:SBGGR8_1X8/640x480]'
    media-ctl -d /dev/media0 -V '"msm_csid0":1 [fmt:SBGGR8_1X8/640x480]'
    media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":0 [fmt:SBGGR8_1X8/640x480]'
    media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":1 [fmt:SBGGR8_1X8/640x480]'
    v4l2-ctl -d "$(media-ctl -d /dev/media0 -e msm_tpg0)" \
        --set-ctrl=test_pattern=9
    timeout 15s v4l2-ctl \
        -d "$(media-ctl -d /dev/media0 -e msm_vfe0_video0)" \
        --set-fmt-video=width=640,height=480,pixelformat=BA81 \
        --stream-mmap=4 --stream-count=30 \
        --stream-to=/tmp/froggerpro-tpg.raw

If capture succeeds, 30 unpadded RAW8 frames total 9,216,000 bytes.
Check the negotiated stride, captured content and advancing CSID/VFE IRQ
counters, rather than relying only on the command's exit status. Repeat
stream start/stop before proceeding to other RDI paths or sensor wiring.

For failures, preserve the full boot dmesg, the media topology, the capture
command output, ``/proc/interrupts`` before and after capture, and
``/sys/kernel/debug/devices_deferred``. Clock/reset timeouts point toward
power or clock setup; SMMU faults point toward stream ID or DMA setup;
capture timeouts without faults require checking TPG routing and CSID
interrupt handling. Sensor work additionally needs confirmed identities,
CCI addresses, reset GPIOs, MCLK outputs, power rails and CSI lane wiring
from the board's stock configuration.

IMX355 ultrawide
----------------

The FroggerPro ``com.qti.sensormodule.FroggerPro_qtech_imx355_uw.bin``
identifies the sensor's 8-bit I2C address as ``0x34`` (7-bit ``0x1a``),
and its register settings program ``0x0114 = 3`` for four CSI data lanes
and ``0x0136/0x0137 = 0x13/0x33`` for 19.2 MHz input clock. The Kera QRD
overlay's ultrawide slot (``qcom,cam-sensor2``) supplies the board wiring:

===================  ===============================================
Resource             Assignment
===================  ===============================================
Control bus          CCI0, master 0, GPIO70/71
CSI receiver         CSIPHY0, four data lanes
Master clock         CAM BIST MCLK0, GPIO65, 19.2 MHz
Reset                GPIO123, active low
Digital power        SGM38120 LDO2, 1.2 V
Interface power      SGM38120 LDO4, 1.8 V
Analog power         SGM38120 LDO7, 2.8 V
Camera PMIC          QUP I2C0 at 0x35; PM8550VS-D GPIO5 reset
PMIC input supplies  S2B for LDO1/2; BOB for LDO3 through LDO7
===================  ===============================================

The SGM38120 driver follows the `SG Micro datasheet
<https://www.sg-micro.com/product/SGM38120>`_ voltage selectors
and system-enable bit. It does not enable unused camera outputs. The
mainline IMX355 driver uses a 360 MHz link frequency in four-lane mode;
its mode timings differ from the vendor settings. Lane order, power
sequencing and sensor capture remain to be confirmed on the phone.

First verify that IMX355 probes successfully and appears in ``media-ctl -p``.
The driver checks chip ID ``0x0355`` at register ``0x0016``. A read failure
or mismatch requires checking PMIC probe, regulator voltages, MCLK and
reset before attempting a stream.

For a proposed full-resolution RAW10 test, replace ``N`` in the sensor
entity name with the CCI adapter number shown in the topology::

    sensor='imx355 N-001a'
    media-ctl -d /dev/media0 -r
    media-ctl -d /dev/media0 -l '"msm_csiphy0":1 -> "msm_csid0":0 [1]'
    media-ctl -d /dev/media0 -l '"msm_csid0":1 -> "msm_vfe0_rdi0":0 [1]'
    v4l2-ctl -d "$(media-ctl -d /dev/media0 -e "$sensor")" \
        --set-ctrl=horizontal_flip=0,vertical_flip=0,test_pattern=2
    media-ctl -d /dev/media0 -V "\"$sensor\":0 [fmt:SRGGB10_1X10/3280x2464]"
    media-ctl -d /dev/media0 -V '"msm_csiphy0":0 [fmt:SRGGB10_1X10/3280x2464]'
    media-ctl -d /dev/media0 -V '"msm_csiphy0":1 [fmt:SRGGB10_1X10/3280x2464]'
    media-ctl -d /dev/media0 -V '"msm_csid0":0 [fmt:SRGGB10_1X10/3280x2464]'
    media-ctl -d /dev/media0 -V '"msm_csid0":1 [fmt:SRGGB10_1X10/3280x2464]'
    media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":0 [fmt:SRGGB10_1X10/3280x2464]'
    media-ctl -d /dev/media0 -V '"msm_vfe0_rdi0":1 [fmt:SRGGB10_1X10/3280x2464]'
    timeout 20s v4l2-ctl \
        -d "$(media-ctl -d /dev/media0 -e msm_vfe0_video0)" \
        --set-fmt-video=width=3280,height=2464,pixelformat=pRAA \
        --stream-mmap=4 --stream-count=10 \
        --stream-to=/tmp/froggerpro-imx355.raw

The sensor-to-CSIPHY link is immutable and already enabled.
``test_pattern=2`` uses the sensor's color bars and exercises the external
PHY as well as CSID/VFE. After that succeeds, set ``test_pattern=0`` for
optical capture. Inspect the returned pixel format and stride: ``pRAA``
is packed RGGB RAW10, rather than 16-bit unpacked pixels. Save kernel logs
and IRQ counters for both successful and failed stream start/stop cycles.
