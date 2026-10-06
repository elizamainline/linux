#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise FroggerPro's QMI LOC receiver over the mainline QRTR transport."""

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
import os
import pty
import signal
import socket
import struct
import sys
import time
import tty

SERVICE_LOC = 16
QRTR_CTRL = 0xfffffffe
HEADER = struct.Struct('<BHHH')
SV = struct.Struct('<IIHBIBfff')
STATUS = {0: 'success', 1: 'in-progress', 2: 'failure', 3: 'timeout',
          4: 'user-ended', 5: 'bad-parameter', 6: 'phone-offline',
          7: 'engine-locked'}


def tlv(tag, value):
    return struct.pack('<BH', tag, len(value)) + value


def fields(data):
    result = {}
    while data:
        if len(data) < 3:
            raise ValueError('truncated TLV header')
        tag, size = struct.unpack_from('<BH', data)
        if size > len(data) - 3 or tag in result:
            raise ValueError('truncated or duplicate TLV')
        result[tag], data = data[3:3 + size], data[3 + size:]
    return result


def scalar(data, tag, fmt, default=None):
    if tag not in data:
        return default
    return struct.unpack('<' + fmt, data[tag])[0]


def discover(sock, node):
    sock.bind((sock.getsockname()[0], 0))
    local_node = sock.getsockname()[0]
    # Ask the local nameserver for LOC v2, all instances. Its empty
    # NEW_SERVER packet terminates the initial lookup dump.
    sock.sendto(struct.pack('<IIIII', 10, SERVICE_LOC, 2, 0, 0),
                (local_node, QRTR_CTRL))
    servers = set()
    deadline = time.monotonic() + 5
    while True:
        if time.monotonic() >= deadline:
            raise TimeoutError('QRTR LOC lookup timed out')
        sock.settimeout(max(0.001, deadline - time.monotonic()))
        packet, address = sock.recvfrom(65536)
        if address != (local_node, QRTR_CTRL) or len(packet) != 20:
            continue
        command, service, instance, server_node, port = struct.unpack('<IIIII', packet)
        if command != 4:
            continue
        if not any((service, instance, server_node, port)):
            break
        if service == SERVICE_LOC and instance == 2 and (node is None or node == server_node):
            servers.add((server_node, port))
    if len(servers) != 1:
        raise RuntimeError(f'expected one LOC v2 instance 0 server, found {sorted(servers)}; use --node if needed')
    server = servers.pop()
    # The same socket becomes the LOC client. QRTR client identity is the
    # socket endpoint, so separate qmicli processes cannot replace this.
    sock.connect(server)
    return server


class Receiver:
    def __init__(self, sock, callback):
        self.sock, self.callback, self.transaction = sock, callback, 0

    def receive(self, timeout):
        self.sock.settimeout(timeout)
        packet = self.sock.recv(65536)
        if len(packet) < HEADER.size:
            raise ValueError('truncated QMI header')
        flag, transaction, message, size = HEADER.unpack_from(packet)
        if size != len(packet) - HEADER.size:
            raise ValueError('invalid QMI payload length')
        data = fields(packet[HEADER.size:])
        if flag == 4:
            self.callback(message, data)
        return flag, transaction, message, data

    def request(self, message, payload):
        self.transaction = self.transaction % 65535 + 1
        self.sock.send(HEADER.pack(0, self.transaction, message, len(payload)) + payload)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            flag, transaction, reply, data = self.receive(max(0.001, deadline - time.monotonic()))
            if (flag, transaction, reply) == (2, self.transaction, message):
                result, error = struct.unpack('<HH', data[2])
                if result:
                    raise RuntimeError(f'QMI LOC 0x{message:04x} failed with protocol error {error}')
                return
        raise TimeoutError(f'QMI LOC 0x{message:04x} timed out')


def position(data):
    report = {'status': scalar(data, 1, 'I'),
              'session': scalar(data, 2, 'B'),
              'technology': scalar(data, 0x23, 'I', 0),
              'utc_ms': scalar(data, 0x25, 'Q'),
              'latitude': scalar(data, 0x10, 'd'),
              'longitude': scalar(data, 0x11, 'd'),
              'accuracy_m': scalar(data, 0x14, 'f', scalar(data, 0x12, 'f')),
              'altitude_m': scalar(data, 0x1b, 'f'),
              'ellipsoid_m': scalar(data, 0x1a, 'f'),
              'speed_m_s': scalar(data, 0x18, 'f'),
              'heading': scalar(data, 0x20, 'f'),
              'satellites_used': 0, 'hdop': None}
    used_tag = 0x36 if 0x36 in data else 0x2c
    if used_tag in data:
        used = data[used_tag]
        if not used or len(used) != 1 + 2 * used[0]:
            raise ValueError('invalid satellites-used list')
        report['satellites_used'] = used[0]
    if 0x24 in data:
        _, report['hdop'], _ = struct.unpack('<fff', data[0x24])
    lat, lon = report['latitude'], report['longitude']
    report['fix'] = (report['status'] == 0 and bool(report['technology'] & 1)
                     and lat is not None and lon is not None
                     and math.isfinite(lat) and math.isfinite(lon)
                     and -90 <= lat <= 90 and -180 <= lon <= 180)
    return report


def satellites(data):
    # gnss8 uses the expanded list: each legacy 28-byte SV entry is
    # followed by a GLONASS frequency index. Both lists have u8 counts.
    tag = 0x11 if 0x11 in data else 0x10
    if tag not in data:
        return None
    raw = data[tag]
    size = SV.size + (1 if tag == 0x11 else 0)
    if not raw or len(raw) != 1 + raw[0] * size:
        raise ValueError('invalid satellite list')
    systems = Counter()
    tracked, best = 0, 0.0
    for offset in range(1, len(raw), size):
        valid, system, _, _, state, _, _, _, cn0 = SV.unpack_from(raw, offset)
        if valid & 1:
            systems[system] += 1
        if valid & 8 and state == 3:
            tracked += 1
        if valid & 0x80 and math.isfinite(cn0):
            best = max(best, cn0)
    return {'listed': raw[0], 'tracked': tracked, 'best_cn0_dbhz': round(best, 1),
            'systems': dict(systems)}


def number(value, decimals=2):
    return f'{value:.{decimals}f}' if value is not None and math.isfinite(value) else ''


def coordinate(value, latitude):
    # Round total minutes first so 59.999999... carries into degrees.
    minutes = round(abs(value) * 60, 7)
    degrees = int(minutes // 60)
    text = f'{degrees:0{2 if latitude else 3}d}{minutes % 60:010.7f}'
    hemisphere = ('N' if value >= 0 else 'S') if latitude else ('E' if value >= 0 else 'W')
    return text, hemisphere


def sentence(parts):
    body = ','.join(parts)
    checksum = 0
    for byte in body.encode('ascii'):
        checksum ^= byte
    return f'${body}*{checksum:02X}\r\n'


def nmea(report):
    # Intermediate reports can contain reference/stale coordinates. Never
    # advertise these as a fix, even when latitude and longitude are present.
    utc = (datetime.fromtimestamp(report['utc_ms'] / 1000, timezone.utc)
           if report['utc_ms'] is not None else None)
    clock = utc.strftime('%H%M%S.') + f'{utc.microsecond // 10000:02d}' if utc else ''
    date = utc.strftime('%d%m%y') if utc else ''
    valid = report['fix'] and utc is not None
    lat, ns = coordinate(report['latitude'], True) if valid else ('', '')
    lon, ew = coordinate(report['longitude'], False) if valid else ('', '')
    speed = report['speed_m_s']
    speed = speed * 1.943844492 if valid and speed is not None else None
    altitude, ellipsoid = report['altitude_m'], report['ellipsoid_m']
    separation = ellipsoid - altitude if valid and altitude is not None and ellipsoid is not None else None
    return (sentence(['GNRMC', clock, 'A' if valid else 'V', lat, ns, lon, ew,
                      number(speed), number(report['heading']) if valid else '', date, '', '',
                      'A' if valid else 'N']) +
            sentence(['GNGGA', clock, lat, ns, lon, ew, '1' if valid else '0',
                      f"{report['satellites_used'] if valid else 0:02d}",
                      number(report['hdop']) if valid else '', number(altitude) if valid else '',
                      'M', number(separation), 'M', '', '']))


class NmeaPty:
    """Expose a raw pseudo-terminal for a read-only GPSD consumer."""

    def __init__(self, path):
        self.path, self.pending = path, b''
        self.master, self.slave = pty.openpty()
        try:
            tty.setraw(self.slave)
            os.set_blocking(self.master, False)
            self.target = os.ttyname(self.slave)
            # Refuse to replace an existing path, including a symlink.
            os.symlink(self.target, path)
        except BaseException:
            os.close(self.master)
            os.close(self.slave)
            raise

    def write(self, text):
        # Bound queued output when GPSD is absent or slow. Keep incomplete
        # writes intact; dropping only whole new records preserves framing.
        if len(self.pending) < 8192:
            self.pending += text.encode('ascii')
        try:
            written = os.write(self.master, self.pending)
            self.pending = self.pending[written:]
        except BlockingIOError:
            pass

    def close(self):
        try:
            if os.path.islink(self.path) and os.readlink(self.path) == self.target:
                os.unlink(self.path)
        finally:
            os.close(self.master)
            os.close(self.slave)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', type=int, help='QRTR modem node (automatically discovered)')
    parser.add_argument('--duration', type=int, default=60, help='seconds to run; 0 runs until interrupted')
    parser.add_argument('--session', type=int, default=73, help='LOC session ID (0..255)')
    parser.add_argument('--show-position', action='store_true', help='include coordinates of successful GNSS fixes in JSON')
    stream = parser.add_mutually_exclusive_group()
    stream.add_argument('--nmea', action='store_true', help='stream generated RMC/GGA on stdout; diagnostics go to stderr')
    stream.add_argument('--pty', metavar='PATH', help='create a raw NMEA pseudo-terminal symlink for GPSD')
    parser.add_argument('--require-fix', action='store_true', help='exit 2 if no successful GNSS fix was received')
    args = parser.parse_args()
    if args.duration < 0 or not 0 <= args.session <= 255 or (args.node is not None and args.node < 0):
        parser.error('invalid duration, session, or node')

    def interrupted(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, interrupted)
    output = sys.stderr if args.nmea else sys.stdout

    def emit(event, **values):
        print(json.dumps({'event': event, **values}, allow_nan=False), file=output, flush=True)

    counts = Counter()
    maximum = {'tracked': 0, 'best_cn0_dbhz': 0.0}
    last_log = {'position': 0.0, 'satellites': 0.0}
    first_fix = None
    began = time.monotonic()

    def indication(message, data):
        nonlocal first_fix
        now = time.monotonic()
        if message == 0x24:
            report = position(data)
            if report['session'] != args.session:
                return
            counts['position_reports'] += 1
            counts['fixes'] += int(report['fix'])
            is_first = report['fix'] and first_fix is None
            if is_first:
                first_fix = round(now - began, 2)
            if args.nmea:
                sys.stdout.write(nmea(report))
                sys.stdout.flush()
            if nmea_pty:
                nmea_pty.write(nmea(report))
            if is_first or now - last_log['position'] >= 10:
                values = {key: report[key] for key in ('fix', 'technology', 'satellites_used')}
                values['status'] = STATUS.get(report['status'], str(report['status']))
                values['accuracy_m'] = float(number(report['accuracy_m'])) if number(report['accuracy_m']) else None
                if args.show_position and report['fix']:
                    values.update(latitude=report['latitude'], longitude=report['longitude'])
                emit('position', **values)
                last_log['position'] = now
        elif message == 0x25:
            report = satellites(data)
            if report is None:
                return
            counts['satellite_reports'] += 1
            for key in maximum:
                maximum[key] = max(maximum[key], report[key])
            if now - last_log['satellites'] >= 10:
                emit('satellites', **report)
                last_log['satellites'] = now

    failed = False
    nmea_pty = None
    with socket.socket(socket.AF_QIPCRTR, socket.SOCK_DGRAM) as sock:
        receiver = Receiver(sock, indication)
        started = False
        try:
            if args.pty:
                nmea_pty = NmeaPty(args.pty)
                emit('pty', path=args.pty)
            node, port = discover(sock, args.node)
            emit('server', node=node, port=port)
            # AFW is essential on gnss8. Omission defaults to NFW and
            # makes Register Events/Start fail. Do not select PRIVILEGED.
            registration = (tlv(1, struct.pack('<Q', 3)) + tlv(0x10, b'MHAL') +
                            tlv(0x11, struct.pack('<I', 1)) + tlv(0x12, b'\x01'))
            receiver.request(0x21, registration)
            emit('registered', client='AFW')
            started = True  # Stop even if a Start response is lost.
            receiver.request(0x22, tlv(1, bytes([args.session])) +
                             tlv(0x10, struct.pack('<I', 1)) +
                             tlv(0x12, struct.pack('<I', 1)) +
                             tlv(0x13, struct.pack('<I', 1000)))
            began = time.monotonic()
            emit('started', session=args.session)
            while args.duration == 0 or time.monotonic() - began < args.duration:
                remaining = args.duration - (time.monotonic() - began) if args.duration else 1
                try:
                    receiver.receive(max(0.001, min(1, remaining)))
                except socket.timeout:
                    pass
        except KeyboardInterrupt:
            emit('interrupted')
        except (OSError, ValueError, RuntimeError, KeyError, struct.error) as error:
            emit('error', detail=str(error))
            failed = True
        finally:
            if started:
                # Avoid a broken NMEA consumer preventing the Stop request.
                receiver.callback = lambda message, data: None
                try:
                    receiver.request(0x23, tlv(1, bytes([args.session])))
                    emit('stopped', session=args.session)
                except (OSError, ValueError, RuntimeError, KeyError, struct.error) as error:
                    emit('stop-error', detail=str(error))
                    failed = True
            if nmea_pty:
                nmea_pty.close()
    emit('summary', **counts, max_tracked=maximum['tracked'],
         best_cn0_dbhz=maximum['best_cn0_dbhz'], first_fix_seconds=first_fix)
    return 1 if failed else (2 if args.require_fix and not counts['fixes'] else 0)


if __name__ == '__main__':
    sys.exit(main())
