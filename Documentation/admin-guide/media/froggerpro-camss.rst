.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro CAMSS bring-up
===================================

The initial Eliza (SM7750) support exposes one Titan 970 Lite CSID/VFE
with four raw RDI outputs, four CSIPHY v2.2.1 receivers in D-PHY mode,
two TPG v1.4 generators, and two CCI controllers with four I2C buses.
The FroggerPro device tree enables these blocks and the CSI analog supplies.

This is a bring-up implementation: device boot, interrupts, DMA and frame
capture still need hardware validation. Sensor drivers and their board
endpoints, C-PHY, full TFE processing and ISP image processing are outside
the initial support. The test generators can exercise CSID and VFE without
a sensor; they do not exercise the external PHYs or CCI buses.

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

``eliza_defconfig`` enables CAMCC built in and CAMSS/CCI as modules.
With an existing configured output directory, object and DTB checks can
be limited to the affected targets::

    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        drivers/clk/qcom/camcc-eliza.o \
        drivers/clk/qcom/cambistmclkcc-eliza.o \
        drivers/media/platform/qcom/camss/ \
        drivers/i2c/busses/i2c-qcom-cci.o \
        qcom/eliza-nothing-froggerpro.dtb

Object checks do not produce an updated boot image or installable modules.
Boot testing requires a kernel, matching modules and the new board DTB
assembled with the normal device boot workflow. On the device::

    modprobe i2c-qcom-cci
    modprobe qcom-camss
    dmesg | grep -Ei 'camss|cam_cc|cci|csid|csiphy|vfe|smmu|defer'
    media-ctl -d /dev/media0 -p

Select the media device whose topology contains ``msm_tpg0``, ``msm_csid0``
and ``msm_vfe0_rdi0``. Device numbering may differ. There should be four
VFE video nodes and four CCI adapters even before sensors are described.

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
