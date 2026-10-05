.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro CAMSS bring-up
===================================

The initial Eliza (SM7750) support exposes one Titan 970 Lite CSID/VFE
with four raw RDI outputs, four CSIPHY v2.2.1 receivers in D-PHY mode,
two TPG v1.4 generators, and two CCI controllers with four I2C buses.
The FroggerPro device tree enables these blocks, the CSI analog supplies
and the Sony IMX355 ultrawide sensor using the existing mainline driver.

The first device boot reached CAMSS entity registration and configured the
SGM38120 camera PMIC. IMX355's initial chip-ID read timed out on CCI0 master
0, queue 0. The CCI pinctrl states have since been moved from the I2C
adapter nodes to their controller nodes so the platform probe selects them.
The next boot log no longer reports the CCI timeout or IMX355 probe failure.
IMX355 now probes and sends valid RAW10 packets through CSIPHY0. Initial raw
captures stalled before a buffer completed; enabling the missing CAMNOC QDSS
XO clock restored capture as described below. The other physical sensors, C-PHY,
full TFE processing and ISP image processing are outside the initial
support. The test generators can exercise CSID and VFE without a sensor;
they do not exercise the external PHYs or CCI buses.

Hardware validation on 2026-10-04
--------------------------------

SSH testing on FroggerPro with kernel ``7.3.0-rc3`` confirms that IMX355
appears as ``imx355 4-001a`` and libcamera enumerates it. Both sensor color
bars at 3280 by 2464 RAW10 and TPG0 at 640 by 480 RAW8 successfully reach
``VIDIOC_STREAMON``, then time out without dequeuing any buffers. TPG0
also times out on RDI1, RDI2 and RDI3. GNOME Camera's black preview is
therefore consistent with the raw capture failure.

During streaming, the CSID receiver packet counter advances and its ECC
and CRC error counters remain zero. A diagnostic packet-header capture
from IMX355 reports VC0, data type ``0x2b`` and a 4100-byte payload, the
expected packed RAW10 line size for 3280 pixels. This establishes sensor
and PHY packet delivery, rather than a completed image in memory.

The Lite 980 common table used by Lite 970 selects timestamp strobe 2.
With this setting, CSID's SOF and EOF timestamps advance for both sources.
The VFE write master has an image address programmed, but its consumed
address and bus-completion status stay zero. Sampling queued MMAP buffers
prefilled with a marker during a three-second TPG stream found the marker
unchanged. No SMMU fault accompanied these tests. The remaining failure
is in frame delivery through the Lite CSID/VFE path; timestamp correction
alone does not enable DMA or fix the preview.

Experiments with RAW decoding, line-based write-master dimensions,
additional RUP/AUP commands, frame/IRQ subsampling, a software CSID reset,
secondary VC/DT matching and VFE routing/clock overrides did not produce
frames. These experimental settings are not part of the driver change.

Follow-up investigation on 2026-10-05
------------------------------------

Kernel ``7.3.0-rc6`` still stalls for both IMX355 color bars and TPG0:
the capture files remain empty, receiver packets and SOF/EOF timestamps
advance, and VFE consumed addresses and completion status remain zero.
Receiver MISR enable, disabling the receiver's RUP/AUP latch, one-frame
batch configuration and VFE common configuration ``0x1`` did not recover
the running TPG capture. Their original configuration values were restored.

The matching downstream kernel configures the RDI path, submits RUP/AUP,
enables the receiver and resumes the path. Mainline queues VFE addresses
and submits RUP/AUP before configuring CSID. The Eliza stream-start change
adds a path-only RUP after RDI configuration and before receiver setup.
It deliberately leaves AUP clear to avoid submitting the initial buffer
addresses a second time. This corrects the configuration-update ordering;
two TPG and two sensor-bar starts after installing the change and rebooting
still timed out with empty files. It does not resolve the DMA stall by itself.
A path-only RUP issued after the stream had already stalled did not recover it.

CPAS identifies Titan 970 (camera version ``0x00090700``) and CPAS 1.1
(``0x10010000``). During capture, its RT QCHANNEL control/status are
``0x1``/``0x4`` and its NRT control/status are ``0x1``/``0x0``. Downstream
power-on waits for status bit 0 (QACCEPTN), which is clear in both samples.
Temporarily enabling the downstream CPAS NRT/SF clocks and
applying its documented GCC camera AXI sleep-staging sequence did not
change these status values or enable DMA. Those settings were restored
and are not included in the driver change.

QDSS XO clock isolation on 2026-10-05
-----------------------------------

The reference CPAS device tree also enables ``CAM_CC_QDSS_DEBUG_XO_CLK``.
Eliza CAMSS initially omitted this branch. During a stalled TPG capture,
changing only its enable bit at CAMCC ``0x11348`` from zero to one immediately
allowed all ten buffers to dequeue, producing 3,072,000 bytes. The same
experiment restored IMX355 color-bar and optical capture at about 30 fps.
Powered sensor snapshots changed RT/NRT QCHANNEL status from ``0x4``/``0x0``
to ``0x5``/``0x1``: both QACCEPTN bits asserted, VFE reported EOF and its
consumed address advanced. No AXI reset, NRT/SF clock override or additional
RUP command was needed for this recovery.

With QDSS XO enabled before stream-on, TPG0, sensor bars and optical capture
each passed two consecutive starts. Each sensor run captured ten 3280 by 2464
packed RAW10 frames with a 4112-byte stride, totalling 101,319,680 bytes.
Unpacking a later sensor-bar frame showed the complete bar pattern. After
uncovering the lens, two further optical starts succeeded and a decoded frame
showed the illuminated scene. A first frame obtained by enabling the clock
midstream was partial; use the normal driver clock lifetime for validation.
All diagnostic clock enable bits were restored after these tests.

The binding, device tree and Eliza VFE clock list now include ``qdss_debug_xo``.
This keeps the clock enabled through the existing VFE power and clock cleanup
paths, without making it permanently critical. After installing the permanent
driver and DTB and rebooting, the user confirmed that capture and GNOME Camera
work. No new kernel, DTB or module was installed during the isolation tests.

Install the updated board DTB and matching CAMSS module, then repeat TPG0,
sensor-bar and optical starts. Ten TPG frames should total 3,072,000 bytes at
640-byte stride. Sensor runs should dequeue ten buffers with the negotiated
frame size. Clear the sensor test pattern before testing GNOME Camera.
Neither a completed build nor a successful stream-on ioctl validates frame
delivery.

Image orientation follow-up
---------------------------

The first working GNOME Camera preview was upside down. The running IMX355
exported ``camera_sensor_rotation=90`` from the board's ``rotation`` property;
the rear-facing ``camera_orientation`` control correctly reported Back.
The board rotation is now 270 degrees to correct the observed 180-degree
orientation error through mounting metadata. The IMX355 driver already exposes
this property to userspace, so no sensor-driver or flip-default change is needed.

The orientation update builds only the board DTB. A boot image repacked from
the installed image contains the updated DTB with a byte-identical kernel and
ramdisk; its decoded DTB differs only in the rotation property. Confirm that
the sensor reports 270 after booting it, then check both the GNOME Camera
preview and a saved photo with the phone upright. This orientation change
still needs that visual validation.

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
The QDSS XO branch already exists in CAMCC, so the clock fix needs only a new
CAMSS module and board DTB, not a rebuilt kernel image.
The tested phone boots an Android header-v2 image containing the DTB. Repack
that image with the updated DTB and its existing kernel and ramdisk before
booting it; the standalone DTB used by an EFI loader is not the active path.
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

Capture diagnostics
-------------------

CAMSS is a media-controller capture device. Applications must configure
the links and pad formats before streaming; its RDI outputs contain raw
Bayer data that needs conversion for display. A camera application's
empty preview alone does not establish whether raw capture works.

On Alpine/postmarketOS, install the diagnostic commands as root::

    apk add v4l-utils libcamera-tools

The Alpine packages `v4l-utils
<https://pkgs.alpinelinux.org/package/edge/community/aarch64/v4l-utils>`_
and `libcamera-tools
<https://pkgs.alpinelinux.org/package/edge/community/aarch64/libcamera-tools>`_
provide ``media-ctl``/``v4l2-ctl`` and ``cam``, respectively.

Copy ``tools/media/froggerpro-capture.sh`` to the phone and run it with
``media-ctl`` and ``v4l2-ctl`` installed, after closing camera applications::

    sudo sh froggerpro-capture.sh imx355-bars
    sudo sh froggerpro-capture.sh tpg
    sudo sh froggerpro-capture.sh imx355

Run the first two tests separately to distinguish sensor/PHY problems
from CSID/VFE problems. The helper finds the CAMSS media device and
IMX355 entity, configures RDI0 and requests ten frames with a 20-second
timeout. Set ``MEDIA_DEVICE=/dev/mediaN`` to select a particular device.
It resets mutable links and leaves the selected pad formats and test
pattern configured after the test.

Each invocation prints a new directory under ``/tmp`` containing
``commands.log``, media topologies, IRQ counters before and after capture,
the kernel log and any raw frames. Commands stop at the first failure;
diagnostics are saved even if capture times out. Preserve these files
when reporting a failure. Absence of boot probe errors does not confirm
sensor detection; the helper also checks for IMX355 in the topology.

For additional driver diagnostics, enable CAMSS dynamic debug before
running the helper, if the kernel provides the control file::

    echo 'module qcom_camss +p' | sudo tee /sys/kernel/debug/dynamic_debug/control

Disable those messages after collecting the logs::

    echo 'module qcom_camss -p' | sudo tee /sys/kernel/debug/dynamic_debug/control

For a stalled stream, copy ``tools/media/froggerpro-camss-registers.py``
to the phone alongside the capture helper. It requires Python 3,
``/dev/mem`` access and mounted debugfs. Run it while capture is still
active, keeping the capture process alive until the snapshots finish::

    sudo sh froggerpro-capture.sh tpg &
    capture_pid=$!
    sleep 2
    sudo python3 froggerpro-camss-registers.py >froggerpro-registers.txt
    wait "$capture_pid"

The script checks the Eliza compatible, CAMSS runtime power and the Lite
VFE/CSID clock enable counts before each of two snapshots. It opens the
register window read-only and does not clear IRQs or change configuration.
It refuses access when CAMSS is suspended or either required clock is
disabled. Compare receiver counters and RDI timestamps between samples,
then inspect VFE bus errors, completion status and consumed addresses.
The helper also reads CPAS QCHANNEL control/status. Downstream expects
QACCEPTN (status bit 0) during power-on; interpret this alongside the clock
and frame-delivery evidence. Debug counters depend on their enable/select
registers; zero values with diagnostics disabled do not prove no activity.
The Lite 880 table used by Lite 970 places RDI1 and RDI2 halt status at
``0x66c`` and ``0x76c``, unlike the ``0x568``/``0x868`` offsets for RDI0/3.
The packet-header fields are meaningful only if a separate diagnostic
has enabled packet capture; this script does not enable it.

For GNOME Camera, also test libcamera enumeration as the logged-in user::

    LIBCAMERA_LOG_LEVELS=*:DEBUG cam -l >libcamera-list.log 2>&1

The `libcamera simple pipeline
<https://github.com/libcamera-org/libcamera/blob/master/src/libcamera/pipeline/simple/simple.cpp>`_
supports ``qcom-camss`` with its software ISP. GNOME Camera's `Aperture
<https://gnome.pages.gitlab.gnome.org/snapshot/aperture/>`_ library uses
GStreamer and PipeWire. If raw capture works but the app does not, inspect
``libcamera-list.log`` and the PipeWire/WirePlumber user-service logs before
changing sensor register settings. The simple pipeline and software IPA
must be present in the distribution's libcamera build. Software ISP
buffer allocation also requires access to the DMA heap or udmabuf device;
the board configuration already enables ``CONFIG_UDMABUF``.

First capture using TPG0
-----------------------

The following test uses 640 by 480 RAW8 color bars on RDI0. It times out
without the CAMNOC QDSS XO clock as described above. Substitute the
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
sequencing now permit valid CSI packet reception; DMA capture remains
unresolved on the phone.

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
