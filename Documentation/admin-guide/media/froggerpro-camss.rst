.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro CAMSS bring-up
===================================

The initial Eliza (SM7750) support exposes one Titan 970 Lite CSID/VFE
with four raw RDI outputs, four CSIPHY v2.2.1 receivers supporting D-PHY
and an initial three-trio C-PHY profile,
two TPG v1.4 generators, and two CCI controllers with four I2C buses.
The FroggerPro device tree enables these blocks, the CSI analog supplies
and the Sony IMX355 ultrawide sensor using the existing mainline driver.
The S5KKD1 front sensor and its AW37004 digital supply have initial drivers
and board wiring. Front-camera capture works; image tuning remains to be done.
The S5KJN5 telephoto sensor probes; its initial D-PHY preview was black.
The board selects its stock C-PHY mode and short-channel PHY profile.
The user confirmed a working telephoto camera after applying these changes
and rebooting, including the sensor readout correction for the mirrored image.

The first device boot reached CAMSS entity registration and configured the
SGM38120 camera PMIC. IMX355's initial chip-ID read timed out on CCI0 master
0, queue 0. The CCI pinctrl states have since been moved from the I2C
adapter nodes to their controller nodes so the platform probe selects them.
The next boot log no longer reports the CCI timeout or IMX355 probe failure.
IMX355 now probes and sends valid RAW10 packets through CSIPHY0. Initial raw
captures stalled before a buffer completed; enabling the missing CAMNOC QDSS
XO clock restored capture as described below. The IMX896 rear main sensor now has an initial driver and board wiring;
its capture path still needs hardware validation. Full TFE processing and
ISP image processing are outside the initial support. The test generators can exercise CSID and VFE without a sensor;
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

* ``cam_csiphy_2_2_1_hwreg.h`` for D-PHY and C-PHY settings;
* ``cam_ife_csid_lite970.h`` and its Lite 880/980 register tables;
* ``cam_vfe_lite97x.h`` and its Lite 98x bus table;
* ``tpg_hw_v_1_4`` for the generator start command and payload format.

The Lite CSID and VFE share the window at ``0x0ad6d000``. The VFE mapping
starts at ``0x0ad6e000`` so its offsets are relative to the VFE version
register and do not overlap the CSID resource. Raw DMA uses SMMU stream
ID ``0x1c00``. CCI buses use GPIO pairs 70/71, 72/73, 74/75 and 76/77.

Build and boot
--------------

``eliza_defconfig`` enables CAMCC built in and CAMSS, CCI, IMX355, IMX896,
S5KKD1, S5KJN5,
SGM38120 and AW37004 as modules.
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

S5KKD1 front camera
-------------------

Initial support added on 2026-10-05 uses the stock 3280 by 2464, approximately
30 fps, four-lane D-PHY RAW10 mode. It exposes GRBG Bayer output, exposure,
analogue gain (1/32 units, 1x through 16x), fixed unity digital gain (256),
vertical blanking and the four stock test-pattern settings. Orientation is
Front and rotation is 90 degrees, corrected after an upside-down preview. Full
resolution, HDR and additional frame rates are not exposed by this driver.
The user confirmed working capture on 2026-10-05 and a much improved image
after applying the Bayer-format and mounting-rotation corrections.

Image format validation on 2026-10-05
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The initial driver advertised RGGB based on the stock mode metadata.
Its sensor color-bar output instead identifies GRBG: the yellow bar has
``[1023, 1023; 0, 1023]`` in a 2 by 2 tile, cyan has
``[1023, 0; 1023, 1023]`` and green has ``[1023, 0; 0, 1023]``.
Decoding all eight bars as GRBG produces white, yellow, cyan, green,
magenta, red, blue and black in order. Treating green samples as red and
blue explained the nearly monochrome optical preview.

The driver now advertises ``SGRBG10_1X10`` and the capture helper requests
packed GRBG RAW10 (``pgAA``) for S5KKD1. IMX355 retains RGGB (``pRAA``).
Three-frame front captures completed with a 4112-byte stride and
10,131,968 bytes per frame. A libcamera preview also completed eight
frames at approximately 30 fps. After the user's reboot, the sensor
reports GRBG and ``camera_sensor_rotation=90``.

Libcamera 0.7.2 on the phone still falls back to ``uncalibrated.yaml``
because there is no ``s5kkd1.yaml``. That profile enables automatic white
balance, black-level processing, image adjustments and exposure control,
but leaves sensor-specific color correction disabled. The pre-correction
preview log showed an identity color matrix and saturation 1, rather
than an intentional grayscale conversion. The missing S5KKD1 sensor helper
also means analogue-gain conversion needs userspace support: the driver's
register units are 1/32, with 32 representing unity gain.

Further image-quality work belongs primarily in libcamera: verify gain
conversion, measure black level and fit color correction under known
lighting. The supplied stock tree contains
``com.qti.tuned.FroggerPro_qtech_s5kkd1_front.bin`` and
``com.qti.eeprom.FroggerPro_front_p24u128b_s5kkd1_eeprom.so``. These are
potential sources of tuning and per-module calibration information;
they are not directly usable as libcamera tuning files, and their contents
have not yet been decoded or applied.

===================  ===============================================
Resource             Assignment
===================  ===============================================
Control bus          CCI1, master 1, GPIO76/77
I2C address          0x7a stock 8-bit address, 0x3d in Linux
Identity             0x4841 at 16-bit register 0x0000
CSI receiver         CSIPHY3, four D-PHY data lanes
Master clock         CAM BIST MCLK3, GPIO68, 19.2 MHz
Reset                GPIO126, active low
Digital power        AW37004 LDO1, 1.05 V
Interface power      SGM38120 LDO4, 1.8 V, shared with IMX355
Analog power         Fixed 2.8 V LDO, PM8550VS-D GPIO6 enable
AW37004              QUP I2C0 at 0x28; GPIO54 EN held low
AW37004 inputs       S2B for digital LDOs; BOB for analog LDOs and bias
===================  ===============================================

The AW37004 driver keeps VIN2 powered for bias and I2C access, even when
only a digital output is used. EN remains low for individual I2C control;
raising it enables all four outputs at their default voltages. Digital
selectors are 600 mV plus 6 mV per step (1.05 V uses selector 75), and
analog selectors are 1.2 V plus 12.5 mV per step. All 256 selectors are
represented. No unused output is explicitly enabled by the driver.
The register map and EN behavior were checked against the Awinic AW37004
V1.7 datasheet and the matching LineageOS regulator source. The
`manufacturer product page <https://www.awinic.com/en/productDetail/AW37004DNR>`_
provides the device documentation.

The wiring is from the Kera QRD overlay in the LineageOS FroggerPro
``dtbo.img`` (entry 49, and the equivalent UFS3 entry 46). The front slot
is ``qcom,cam-sensor1`` under CCI1, with ``cci-master = <1>`` and
``csiphy-sd-index = <3>``. Its core rail is AW_LDO1, not an SGM38120 output.
The fixed analog regulator's GPIO6 belongs to PM8550VS-D, as recorded in
the overlay's fixups.

Register provenance and timing
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The register data comes from the locally supplied LineageOS file::

    vendor/nothing/FroggerPro/proprietary/vendor/lib64/camera/
      com.qti.sensormodule.FroggerPro_qtech_s5kkd1_front.bin

Its SHA256 is::

    d41f929486f62ffa46201c42ffdb8cb080afa754fdd5d00b0269e9d684d28fd9

The parameter directory at file offset ``0x18c`` contains 16-byte entries
(ID, payload offset, length, type). Payload offsets are relative to the
sensor data at ``0x19414``. Entry 5009 contains 258 initialization writes;
the driver splits it after the first four writes to preserve the 8000 us
delay after ``0x6010 = 1``. The selected resolution is entry 35's second
mode, whose 234 writes are referenced by entry 1281. The 492 register
addresses, values and widths in the driver were compared against these
entries. Page-select writes to ``0xfcfc`` retain their original order;
mode programming ends on page ``0x4000`` before controls and stream-on.

Stock timing programs line length 3968 and frame length 4728. The VT PLL
registers imply 563.2 MHz at the 19.2 MHz board clock, yielding about
30.02 fps; this is the pixel-rate control used with blanking and exposure.
The stock mode's output pixel clock is 510.96 MHz, which gives a nominal
638.7 MHz CSI link clock at ten bits per pixel over four DDR lanes.
These two clocks serve different purposes. Confirm receiver packet timing,
CRC/ECC counters and measured frame rate on the phone before extending
mode support or treating the link rate as hardware-verified.

Power-up follows the stock sequence: assert reset, enable I/O and wait
1 ms, enable digital and wait 2 ms, enable analog and wait 1 ms,
deassert reset and wait 3 ms, then enable MCLK and wait 12 ms. Streaming
replays the software reset and initialization after each power cycle.
Stopping or a failed start synchronously powers the sensor down.

Incremental build and first device test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Keep the existing build configuration, enable the two new modules, and
build only the new objects and board DTB::

    scripts/config --file /tmp/froggerpro-build/.config \
        --module VIDEO_S5KKD1 --module REGULATOR_AW37004
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 olddefconfig
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 W=1 \
        drivers/media/i2c/s5kkd1.o drivers/regulator/aw37004.o \
        qcom/eliza-nothing-froggerpro.dtb

The new ``s5kkd1.ko`` and ``aw37004.ko`` have also been built using the
existing output directory, with ``KERNELRELEASE=7.3.0-rc6`` to match the
supplied boot log. The media-core and regmap-I2C module targets were included
in that limited build so modpost could resolve their exports. No kernel
Image rebuild or device installation was performed. If building modules
again, use the actual target kernel release and its matching build output.

Install both new modules and repack the active header-v2 boot image with
the updated board DTB and existing kernel and ramdisk. CAMSS waits for
both connected sensors, so an unresolved front-sensor probe can delay media
device registration. After booting, load the supplies and sensors::

    modprobe i2c-qcom-cci
    modprobe sgm38120
    modprobe aw37004
    modprobe imx355
    modprobe s5kkd1
    modprobe qcom-camss
    dmesg | grep -Ei 's5kkd1|aw37004|sgm38120|cci|camss|defer'
    media-ctl -d /dev/media0 -p

Check for ``s5kkd1 <bus>-003d`` linked to ``msm_csiphy3``. Its probe checks
chip ID 0x4841 before registering the sensor. For an I2C error or ID
mismatch, collect the boot log and deferred-device list and check AW37004,
GPIO126, GPIO68 and the three rails before attempting capture.

With the updated helper copied to the phone and camera apps closed::

    sudo sh froggerpro-capture.sh s5kkd1-bars
    sudo sh froggerpro-capture.sh s5kkd1

Repeat each command to check start/stop and power cycling. Ten frames at
3280 by 2464 packed RAW10 should total 101,319,680 bytes if CAMSS negotiates
the same 4112-byte stride as IMX355. Verify the reported stride and buffer
count, decode a later color-bar frame, then inspect an uncovered optical
frame. Check that CSID CRC/ECC counters stay zero and that libcamera lists
both cameras. Clear the test pattern before testing the front camera in
GNOME Camera, and visually verify mounting rotation with the phone upright.

S5KJN5 telephoto camera
----------------------

Initial support on 2026-10-05 imports Wenmeng Liu's `v5 binding
<https://lore.kernel.org/r/20260928-sk5jn5-v5-1-19aa0a0a68eb@oss.qualcomm.com>`_
and `v5 driver
<https://lore.kernel.org/r/20260928-sk5jn5-v5-2-19aa0a0a68eb@oss.qualcomm.com>`_.
The original authorship and register tables are preserved in separate commits.
A follow-up adapts the pad callbacks to this tree's V4L2 API and acquires the
module-dependent AF rail and the unused 1.2 V I/O rail as optional regulators.
Missing optional rails are not replaced with dummy regulators; other regulator
errors, including probe deferral, still propagate.

The initial D-PHY experiment probed successfully after the user's reboot,
appearing as ``s5kjn5 6-002d`` linked to ``msm_csiphy2``. The user reported a
black preview. Stock telephoto metadata specifies three C-PHY trios
(``laneCount = 3``, ``is3Phase = 1``), so the board now selects C-PHY at both
ends of the link. The imported four-lane D-PHY mode remains available for
modules wired for that interface.

The C-PHY mode exposes 4096 by 3072 RAW10 with BGGR output, exposure,
analogue gain, digital gain, vertical blanking and test patterns. Its
``V4L2_CID_LINK_FREQ`` is 998.4 MHz, half the 1.9968 Gsymbols/s C-PHY symbol
rate, following the V4L2 CSI-2 convention. The VT pixel clock is 921.6 MHz;
line length is 4844 and the default frame length is 6338, giving about
30.02 fps. The stock frame length 3169 is the minimum, allowing about
60.04 fps by reducing vertical blanking to 97. The default vertical blanking
is 3266. The transport frequency stays fixed when frame timing changes.
Native color-bar samples establish GRBG before the mirror correction. The
sensor vertical flip changes this to BGGR. The rotation of 270 follows the
corrected rear-camera metadata used for IMX355. The user confirmed that the
camera works correctly after applying the readout correction and rebooting.

CAMSS carries the bus type through CSIPHY and CSID, accounts for C-PHY's
16/7 coding ratio when deriving a missing link frequency, and sets the Lite
CSID PHY-type bit at bit 24. The v2.2.1 PHY uses the stock reset-exit value
``0x0e`` with its 3048 us delay and enables trios with mask ``0x2a``. Its
rate-dependent settings precede the common C-PHY mission settings, matching
the downstream driver. This initial profile supports only three trios mapped
0, 1, 2 on Eliza, short-channel settings and symbol rates above 1.7 and up
to 2.0 Gsymbols/s. It uses the table's settle count ``0x27``; other rates,
channel types and adaptive settle timing need additional implementation.
Existing D-PHY register tables and test-generator routing are preserved.

===================  ===============================================
Resource             Assignment
===================  ===============================================
Control bus          CCI1, master 0, GPIO74/75
I2C address          0x5a stock 8-bit address, 0x2d in Linux
Identity             0x38e5 at 16-bit register 0x0000
CSI receiver         CSIPHY2, three C-PHY trios
Master clock         CAM BIST MCLK2, GPIO67, 19.2 MHz
Reset                GPIO125, active low
Digital power        SGM38120 LDO1, 1.0 V, core and MIPI
Interface power      SGM38120 LDO4, 1.8 V, shared with other sensors
Analog power         SGM38120 LDO3, 2.2 V
===================  ===============================================

Wiring comes from the LineageOS FroggerPro Kera QRD overlay (``dtbo.img``
entry 49; equivalent UFS3 entry 46), slot ``qcom,cam-sensor3`` under CCI1.
The sensor identity, address and C-PHY metadata come from::

    vendor/nothing/FroggerPro/proprietary/vendor/lib64/camera/
      com.qti.sensormodule.FroggerPro_qtech_s5kjn5_tele.bin

The file's sensor parameter directory starts at ``0x27c`` and ends at
``0x8d62c``; payload offsets are relative to ``0x8d664``. Its root slave
information records address ``0x5a`` and identity ``0x38e5``. The supplied
AAC v2 module uses the same sensor identity and address. The external
DW9827C actuator, OIS, their 3.3 V rails and EEPROM are not driven by this
initial sensor support. Optical focus and stabilization need separate work.

Register provenance
~~~~~~~~~~~~~~~~~~~

The Qtech sensor binary's SHA256 is::

    2ef1f1691f2a165de51718e3f145da80fca3fec9ae5526c51a5b6bfc784ec83d

Directory entry 18429 supplies 3486 initialization writes with a 5 ms delay
after write 12. Entry 35's mode 3 references entry 3238, which supplies 592
mode writes. C-PHY uses these paired stock sequences, including their
firmware, rather than combining the stock mode with the imported firmware.
Six contiguous firmware/calibration blocks match the imported driver's byte
arrays and are shared; two additional blocks are specific to the stock
initialization. Expanded addresses, values, widths and the delay were checked
against the binary. Programming ends on page ``0x4000`` before applying
controls and stream-on. Frame-length control changes the stock 60 fps timing
to the default 30 fps timing after mode programming.

PHY settings come from the locally supplied Qualcomm camera-kernel source::

    drivers/cam_sensor_module/cam_csiphy/include/cam_csiphy_2_2_1_hwreg.h

The 72 common C-PHY writes and 36 short-channel 2.0 Gsymbols/s profile
writes were compared against this header. Reset, lane-enable and programming
order follow ``cam_csiphy_core.c``. The standard-channel profile produced
sparse packets and CRC errors on the phone. The short-channel profile receives
complete RAW10 color bars without CRC or ECC errors. During earlier diagnosis,
some starts returned one empty buffer and stalled with RX stream-underflow
bit 23. Startup ordering and interrupt recovery experiments are not included
in the committed changes. The subsequent reboot test confirmed a working
camera.

Incremental build and next device test
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Use the existing configured output and its symbol exports. Build the enabled
sensor modules, the small videobuf2 dependency set for CAMSS modpost, CAMSS,
and the board DTB::

    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        KERNELRELEASE=7.3.0-rc6 W=1 M=drivers/media/i2c \
        MO=/tmp/froggerpro-build/drivers/media/i2c modules
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        KERNELRELEASE=7.3.0-rc6 W=1 M=drivers/media/common/videobuf2 \
        MO=/tmp/froggerpro-build/drivers/media/common/videobuf2 modules
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        KERNELRELEASE=7.3.0-rc6 W=1 M=drivers/media/platform/qcom/camss \
        MO=/tmp/froggerpro-build/drivers/media/platform/qcom/camss \
        KBUILD_EXTRA_SYMBOLS=/tmp/froggerpro-build/drivers/media/common/videobuf2/Module.symvers \
        modules
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        DT_SCHEMA_FILES='media/i2c/samsung,s5kjn5.yaml:media/qcom,eliza-camss.yaml' \
        CHECK_DTBS=y qcom/eliza-nothing-froggerpro.dtb

Use ``KERNELRELEASE`` only when it matches the kernel on the phone. This
procedure does not rebuild the kernel Image. Install the updated
``s5kjn5.ko`` and ``qcom-camss.ko``, then repack the active boot image with
the C-PHY board DTB and its existing kernel and ramdisk. The running kernel
has no OF overlay support; replacing modules alone cannot change its live
D-PHY endpoints. Reboot with the new DTB before testing. Keep the working
boot image and modules available for rollback. CAMSS waits for all enabled
sensors to bind, so a failed telephoto probe can also prevent the other
cameras from appearing.

After booting the updated DTB, load ``s5kjn5`` and confirm that the topology
contains ``s5kjn5 N-002d`` linked to ``msm_csiphy2``. Close camera applications,
then collect sensor bars before optical frames::

    sudo sh froggerpro-capture.sh s5kjn5-bars
    sudo sh froggerpro-capture.sh s5kjn5

The helper requests ten 4096 by 3072 packed BGGR RAW10 frames (``pBAA``)
through RDI0 and saves the negotiated format, topology, IRQ counts and dmesg.
At a 5120-byte stride, ten frames occupy 157,286,400 bytes. Confirm the
negotiated stride rather than assuming this total. Repeat starts, inspect
color-bar content and record CSI CRC/ECC counters. Neither successful builds,
probe nor stream-on alone establishes working telephoto capture. Clear the
sensor test pattern before trying a preview; libcamera gain conversion and
sensor-specific image tuning may also need follow-up.

Telephoto mirror correction
~~~~~~~~~~~~~~~~~~~~~~~~~~

After rebooting with C-PHY support, the user confirmed that the camera works
but both preview and saved photos exchange left and right. The telephoto node
correctly reports a rear-facing camera (``orientation = <1>``) and a 270-degree
mounting rotation. The orientation property identifies Front/Back/External;
it does not describe an optical reflection. Keep this metadata unchanged.

The FroggerPro C-PHY profile now sets the sensor's 8-bit orientation register
``0x0101`` to ``0x02`` after the stock mode sequence and before streaming.
At a 270-degree mounting rotation, this sensor vertical flip corrects a
horizontal mirror in the displayed image. It changes native GRBG to BGGR,
so the driver and capture helper advertise the matching Bayer order. The
imported D-PHY profile retains its original GBRG output and register tables.
The stock C-PHY tables also remain unchanged; the readout correction is a
separate register write after mode setup. Only the sensor module needs to be
rebuilt and replaced for this correction.

On 2026-10-05, after applying the readout correction and rebooting, the user
confirmed that the camera works correctly. The sensor module built
incrementally with ``LLVM=1 ARCH=arm64 -j24 W=1`` in the existing output
directory; no full kernel rebuild was performed for this correction.

For future regression tests, check an asymmetric scene or readable text in
both the preview and a saved photo. Confirm that left and right match the
scene and that colors remain correct.

IMX896 rear main camera
----------------------

Initial support uses the FroggerPro stock 4096 by 3072 RAW10 preview mode,
with 2 by 2 binning across the 8192 by 6144 array. The driver checks chip ID
``0x0896`` at register ``0x0016`` and accepts a 19.2 MHz input clock and three
C-PHY trios. Exposure, analogue/digital gain, vertical blanking, custom and color-bar
test patterns and firmware mounting controls are exposed through V4L2.
The stock sensor library encodes analogue gain as
``16384 / (16384 - register)`` with a 64x limit (register 16128).
Userspace sensor helpers must use this conversion for automatic gain control.
Normal readout is RGGB, verified by the captured color-bar sequence.
Mounting rotation still needs confirmation from an optical scene. There is no autofocus or OIS driver
for this module yet, so optical images may be out of focus.

The stock Kera QRD overlay, slot ``qcom,cam-sensor0``, supplies the wiring:

===================  ===============================================
Resource             Assignment
===================  ===============================================
Control bus          CCI0, master 1, GPIO72/73, 7-bit address 0x10
CSI receiver         CSIPHY1, three C-PHY trios
Master clock         CAM BIST MCLK1, GPIO66, 19.2 MHz
Reset                GPIO124, active low
Analog power 1       AW37004 LDO3, 1.8 V
Analog power 2       Fixed 2.8 V rail enabled by GPIO115
Digital power        AW37004 LDO2, 1.104 V
Interface power      SGM38120 LDO4, 1.8 V (shared with other cameras)
===================  ===============================================

Stock power-up enables analog 1, analog 2, digital and interface power in
that order, with millisecond settling delays, then MCLK and reset release.
The driver follows that sensor sequence and unwinds enabled rails on error.
Stock additionally enables AW37004 LDO4 at 3.1 V through ``CUSTOM_REG2``;
this rail and the SGM38120 LDO5 autofocus supply are left to future module
actuator/OIS support. If chip identification fails, verify the four sensor
rails and reset/MCLK before investigating that extra module supply.

The initialization and mode sequences are extracted from the LineageOS
``vendor/nothing/FroggerPro/proprietary/vendor/lib64/camera/`` file
``com.qti.sensormodule.FroggerPro_shinetech_imx896_wide.bin``. Its SHA256 is
``f035167e6276a25d16fa1eee1c6340bc4a740d53b93253cb89f595b4b3ba2067``.
The parameter index spans offsets ``0x27c`` through ``0x2813c``, with
16-byte little-endian ID/offset/length/type records. Payload offsets are
relative to ``0x28174``. Parameter 7401 contains 473 initialization writes;
parameter 688 contains the 133 writes for resolution index 1. Each write
record has fourteen 32-bit words; its value and delay fields reference
other parameters. All writes in these two lists are bytes without delays,
and their ordering is preserved in the driver. No runtime firmware blob
is required.

The mode programs ``0x0111=3`` (C-PHY), ``0x0114=2`` (three trios), line
length 11904 and frame length 3774. The output PLL uses a 19.2 MHz clock,
predivider 19, multiplier 1955 and OP system divider 2, giving about
987.789 Msymbols/s and a V4L2 link frequency of 493894737 Hz (half the
symbol rate). The initial calculation omitted the OP system divider and
advertised twice the actual frequency. The VT PLL
programming gives a timing pixel rate of 1356800000 Hz, about 30.2 frames/s
with the stock line and frame lengths. The stock ``outputPixelClock``
metadata is a separate transport quantity and is not used for exposure
or blanking timing. The stock mode also emits phase-detection data on VC2,
data type ``0x30``; initial CAMSS capture selects the image on VC0/RAW10.
The Eliza PHY selects the stock short-channel 1.0 Gsymbols/s profile
for this mode; the 2.0 Gsymbols/s profile remains available for telephoto.

On 2026-10-05, targeted LLVM arm64 object/module and board DTB builds passed,
as did binding/example and board schema validation. The register sequences
were compared byte for byte with the stock lists. Capture helper routing,
format and pattern commands passed mock checks for the new camera and the
existing modes. These are build/static checks; no IMX896 hardware capture
was performed at that stage.

Use the existing output directory for targeted builds::

    scripts/config --file /tmp/froggerpro-build/.config --module VIDEO_IMX896
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 olddefconfig
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        drivers/media/i2c/imx896.o qcom/eliza-nothing-froggerpro.dtb
    make LLVM=1 ARCH=arm64 O=/tmp/froggerpro-build -j24 \
        M=drivers/media/i2c MO=/tmp/froggerpro-build/drivers/media/i2c imx896.ko

The module-only invocation keeps artifacts under the existing output
directory with ``MO`` and uses the full ``Module.symvers`` for
V4L2/CCI/media dependencies. A direct ``drivers/media/i2c/imx896.ko`` target
can report unresolved symbols from dependencies that are built as modules.
These commands do not rebuild the kernel image. Repack the installed boot
image with the new DTB and install the matching ``imx896.ko`` before testing.

After reboot, load ``aw37004``, ``sgm38120``, ``i2c-qcom-cci``, ``imx896``
and ``qcom-camss``, then inspect the media topology and deferred devices.
Adding the main-camera endpoint makes CAMSS wait for IMX896 to bind; a
missing module or failed probe can therefore prevent media registration.
Run the helper after closing camera applications::

    sudo sh froggerpro-capture.sh imx896-test
    sudo sh froggerpro-capture.sh imx896

The test mode selects color bars (menu index 2, register ``0x0600=2``
after ``0xa200=0``). Menu index 1 retains the stock custom pattern at
``0x0600=5``, which produces zero-filled frames with this initialization.
Optical mode clears the pattern. Both modes route CSIPHY1 to RDI0 and request ten
4096 by 3072 packed RGGB RAW10 frames. At a negotiated 5120-byte stride,
ten frames total 157286400 bytes. Verify buffer completion, frame content,
Bayer order, orientation and repeated stream starts; build success alone
is not evidence that the camera captures. If capture fails, keep the
helper's diagnostics and use the CAMSS register snapshot tool during the
stream to distinguish receiver errors from a CSID/VFE DMA stall.

Live main-camera diagnosis and receiver corrections
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

After the user's reboot on 2026-10-05, SSH captures reproduced the black
output: optical capture stalled, and a completed test buffer contained only
zero bytes. Chip ID, mode registers and the advancing sensor frame counter
confirmed that IMX896 was powered and streaming. The internal generator and
IMX355 captured ten frames each, while both C-PHY cameras stalled. CSID
reported receiver FIFO/underflow errors. Zero CRC/ECC counters alone did
not establish successful image transport.

Two receiver corrections were verified together. The stock IMX896 OP PLL
system divider is 2 at ``0x030b``, so its symbol rate is 987.789 Msymbols/s,
not 1.976 Gsymbols/s. This also agrees with the stock transport pixel clock:
three trios times symbol rate times 16/7, divided by RAW10's ten bits per
pixel, gives approximately 677.341 Mpixels/s. The sensor, binding and board
endpoint now advertise 493894737 Hz. CAMSS supports this frequency with the
complete stock short-channel 1.0 Gsymbols/s table, including settle count
``0x3c``, CDR ``0x58`` and ``0x0214/0x0614/0x0a14=9``. It selects profiles
by link frequency, retaining the existing telephoto profile and rejecting
rates between the supported windows.

The powered PHY also read ``0x1000=0`` during failed captures. Reapplying
``0x0e`` and the downstream 3048 us delay during stream setup selects
three-phase mode after pipeline power-up/reset. Applying only the rate
profile, without this stream-time mode selection, reproduced the stall.
The committed fix retains the existing power-on reset and adds stream-time
mode selection only for C-PHY.

A temporary CAMSS module selected the corrected profile for CSIPHY1 while
the running DT and sensor module still advertised the old frequency. With
both corrections, two consecutive optical captures and the stock custom
pattern capture each completed ten 4096 by 3072 packed RAW10 buffers, totaling
157286400 bytes per capture at about 30 fps. The optical frame contained
nonzero scene data; the dark, blurred image still needs exposure, focus and
image-processing work. The stock custom pattern produced zero-filled frames,
so it is unsuitable as a visible transport test. A separate color-bar test
with ``0x0600=2`` completed ten buffers with zero CRC/ECC counters and no
receiver FIFO/underflow errors in three live samples. The driver now exposes
this pattern at menu index 2 and the capture helper uses it. Decoding the
pattern as RGGB gives white, yellow, cyan, green, magenta, red, blue and
black bars in the expected order. A subsequent S5KJN5 color-bar capture also completed
ten buffers. The unchanged installed CAMSS module and WirePlumber service
were restored after the temporary tests. The final frequency-based selection
requires the updated IMX896 module, CAMSS module and board DTB together.

Temporary standard-channel, settling, trio-order, packet-check, PDAF and
higher decoder/VFE clock experiments were restored. The 480 MHz clock
experiment did not fix the failure and is not included in the changes.
Diagnostic captures and logs are retained on the phone under
``/tmp/froggerpro-imx896-debug``. A host optical sample is
``/tmp/froggerpro-imx896-optical.raw``. The public `Fairphone 6 driver
<https://forgejo.catcrafts.net/Catcrafts/milos-linux/src/branch/combined-stable/drivers/media/i2c/imx896.c>`_
provided an additional comparison for PLL and receiver setup; the mode and
PHY register tables used here remain those supplied in the FroggerPro sources.

For CAMSS-only builds with separately built videobuf2 modules, add::

    KBUILD_EXTRA_SYMBOLS=/tmp/froggerpro-build/drivers/media/common/videobuf2/Module.symvers

This supplies their symbol exports to modpost without a full kernel rebuild.
