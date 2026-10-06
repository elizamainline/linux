.. SPDX-License-Identifier: GPL-2.0-only

Nothing Phone (4a) Pro GPS bring-up
===================================

FroggerPro's GNSS receiver is part of the modem. Mainline communicates with
it through QMI LOC service 16, version 2, instance 0 over QRTR. The existing
MPSS remoteproc, GLINK and ``CONFIG_QRTR`` / ``CONFIG_QRTR_SMD`` support
already supplies this transport. Hardware validation required no kernel
image, device-tree or configuration change, and no kernel rebuild.

Framework-client registration
-----------------------------

This modem requires application-framework (AFW) registration on the LOC
client's own QRTR socket before starting positioning. ``Register Events``
(0x0021) must carry these additional TLVs:

* 0x10: client identification string ``MHAL``.
* 0x11: client type 1 (AFW), encoded as a little-endian 32-bit enum.
* 0x12: positioning-request notification flag, encoded as a byte. The
  bridge sends 1; the protocol specifies that AFW ignores this flag.

Without identification, the client defaults to non-framework (NFW).
On the tested firmware, registering satellite events returned QMI error
48 (``InvalidArgument``). Starting positioning returned error 46
(``GeneralError``), followed by a position report with session status 7
(``ENGINE_LOCKED``). Changing the engine lock from ``mt`` to ``none`` did
not resolve startup; the setting was restored to ``mt``. AFW registration
allowed startup with that original lock setting.

The installed ``qmicli`` was version 1.39.0. Its ``--loc-start`` action
does not perform AFW registration, and a separate ``qmicli`` process has
a different QRTR client unless the same proxy client is explicitly reused.
Stock ModemManager also failed to enable GPS with ``GeneralError``.
Its LOC path needs AFW registration before startup. Adding the optional
input fields to libqmi alone does not make callers use them.

NMEA and Linux GPS clients
--------------------------

The downstream ``gps.conf`` selects ``NMEA_PROVIDER=0``, which generates
NMEA in application userspace. The tested firmware rejected QMI Get/Set
NMEA Types with error 94 (``NotSupported``), including after AFW
registration. Position and satellite reports work through QMI.

``tools/gps/froggerpro-gps.py`` supplies a standalone receiver and an
NMEA bridge using Python's standard library. It discovers the modem
endpoint, registers an AFW client, starts periodic positioning, and sends
Stop on normal exit, SIGINT and SIGTERM. It handles both the legacy and
expanded satellite lists used by gnss8. A disconnected modem or malformed
reply terminates the test with an error.

The bridge generates RMC and GGA with checksums. A position is valid only
when the session status is success, the technology mask includes satellite
positioning, and the coordinates are finite and in range. Intermediate
reports can contain old or reference coordinates; they produce invalid
NMEA with empty coordinates. NMEA additionally requires the modem's UTC
timestamp. The bridge reports sea-level altitude and converts speed from
metres per second to knots.

Run on the phone, with its modem firmware running::

  python3 froggerpro-gps.py --duration 120 --require-fix

JSON diagnostics omit coordinates by default. ``--show-position`` includes
coordinates of successful fixes. ``--require-fix`` returns status 2 if
the session completed without a successful satellite fix, status 1 for a
protocol or transport failure, and status 0 for a successful test.

From the development host, no file installation is needed for this test::

  ssh -F /dev/null 172.16.42.1 \
      'python3 -u - --duration 120 --require-fix' \
      < tools/gps/froggerpro-gps.py

For GPSD, first copy the tool to the phone and start the bridge in one
terminal::

  python3 froggerpro-gps.py --duration 0 --pty /tmp/froggerpro-gps-nmea

After the ``pty`` event appears, run GPSD in another terminal::

  gpsd -b -n -N -S 2948 -F /tmp/froggerpro-gpsd.sock \
       /tmp/froggerpro-gps-nmea

Connect a client to the test port::

  cgps 127.0.0.1:2948

``-b`` keeps GPSD from writing receiver configuration commands to the
pseudo-terminal. The bridge refuses to replace an existing PTY path and
removes its symlink on exit. Stop GPSD and the bridge when finished.
``--nmea`` instead streams NMEA on stdout, with JSON diagnostics on stderr.

This bridge provides RMC/GGA position data for GPSD; it does not export
GSV/GSA sky-view data or implement a ModemManager/GeoClue backend.
Its JSON satellite reports remain available for bring-up diagnostics.

Hardware validation on 2026-10-06
---------------------------------

Tests used the connected phone running ``7.3.0-rc6`` with modem firmware
``MPSS.DE.7.0.c12-00059-PAKALA_GEN_PACK-2.150829.2.160035.3``. The user placed
the phone near a window. LOC was discovered at node 0, port 115; this port
is dynamic and is discovered by the tool.

* AFW registration changed Register Events and Start from failure to
  success, without restarting the modem.
* A 120-second run received 121 position reports, including 118 successful
  satellite fixes, and 148 satellite reports. First fix arrived in 2.8
  seconds. Up to 38 satellite entries were tracked, with best reported
  C/N0 of 38.5 dB-Hz. Reports covered GPS, GLONASS, BeiDou and Galileo.
* Reported horizontal uncertainty reached 2.5 metres. This is a firmware
  estimate, not a measured error against a surveyed reference.
* A subsequent 20-second start/stop cycle produced 20 successful fixes,
  with first fix at 0.15 seconds.
* GPSD 3.27.3 consumed the raw PTY in read-only mode and delivered five
  consecutive TPV reports with mode 3, finite latitude/longitude, UTC
  time, and sea-level altitude.
* That bridge run received 121 position reports with 120 successful
  satellite fixes, 149 satellite reports, up to 40 tracked entries and
  best C/N0 of 39.0 dB-Hz. First fix arrived in 1.83 seconds. Stop
  succeeded and the PTY symlink was removed at the end.
* A bounded SIGTERM test interrupted an active session, successfully
  sent Stop, and removed its PTY symlink. The engine lock remained ``mt``
  after all tests.

``gpsd`` and ``gpsd-clients`` were installed on the test phone, along with
the receiver at ``/usr/local/bin/froggerpro-gps``. This command accepts the
same arguments as the script. Automatic startup was not enabled; the
test processes exited after their bounded sessions.

The modem was in standalone positioning mode. UTC time was injected once
with ``qmicli --loc-inject-time`` during diagnosis. Existing assistance
data was retained. These results are warm-start tests; an erased-assistance
cold start, outdoor accuracy, suspend/resume and assisted GPS downloads
remain unvalidated.

Protocol references and local checks
------------------------------------

Qualcomm's public `location service header
<https://github.com/qualcomm-linux/location-apis-qcom/blob/location.lnx.0.0/loc_api/loc_api_v02/location_service_v02.h>`_
defines the client types, registration fields, session statuses, expanded
satellite structures and position reports. Its adjacent generated
``location_service_v02.c`` specifies the TLV identifiers and wire layout.

Upstream libqmi `commit 532de37e84da
<https://chromium.googlesource.com/external/gitlab.freedesktop.org/mobile-broadband/libqmi/+/532de37e84dac82936b056e28fb71ce4c7af60d6>`_
adds AFW identification fields to Register Events for libqmi 1.40 and
documents the same startup failures on newer Snapdragon GNSS engines.

Run the decoder, NMEA and PTY checks from the kernel source tree::

  python3 tools/gps/test_froggerpro_gps.py

The tests cover truncated and duplicate TLVs, legacy/expanded satellite
formats, validity masks, NMEA units and checksums, coordinate rounding,
rejection of intermediate or non-satellite fixes, and PTY ownership and
cleanup.
