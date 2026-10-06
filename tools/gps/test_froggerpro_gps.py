#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wire-format and no-false-fix checks for the QMI LOC/NMEA bridge."""

import importlib.util
import os
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('gps', Path(__file__).with_name('froggerpro-gps.py'))
gps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gps)


class WireFormat(unittest.TestCase):
    def test_truncated_and_duplicate_tlvs(self):
        for payload in (b'\x01', b'\x01\x04\x00\x00',
                        b'\x01\x00\x00\x01\x00\x00'):
            with self.assertRaises(ValueError):
                gps.fields(payload)

    def test_legacy_and_expanded_satellites(self):
        # GPS satellite 23, tracked, all fields valid, C/N0 32.5 dB-Hz.
        legacy = bytes.fromhex('ff000000010000001700010300000001') + struct.pack('<fff', 30, 150, 32.5)
        for tag, record in ((0x10, legacy), (0x11, legacy + b'\x00')):
            report = gps.satellites({tag: b'\x01' + record})
            self.assertEqual(report['listed'], 1)
            self.assertEqual(report['tracked'], 1)
            self.assertEqual(report['best_cn0_dbhz'], 32.5)
            self.assertEqual(report['systems'], {1: 1})
        with self.assertRaises(ValueError):
            gps.satellites({0x11: b'\x01' + legacy})

    def test_invalid_cn0_is_not_reported(self):
        record = struct.pack('<IIHBIBfff', 9, 1, 23, 0, 3, 0, 0, 0, 99)
        self.assertEqual(gps.satellites({0x10: b'\x01' + record})['best_cn0_dbhz'], 0)


class Nmea(unittest.TestCase):
    def report(self, status=0, technology=1):
        return gps.position({1: struct.pack('<I', status), 2: b'I',
                             0x10: struct.pack('<d', 37.75),
                             0x11: struct.pack('<d', -122.5),
                             0x18: struct.pack('<f', 1),
                             0x1a: struct.pack('<f', 42),
                             0x1b: struct.pack('<f', 32),
                             0x20: struct.pack('<f', 90),
                             0x23: struct.pack('<I', technology),
                             0x24: struct.pack('<fff', 2, 1, 1.5),
                             0x25: struct.pack('<Q', 1791288000000),
                             0x36: b'\x04\x01\x00\x02\x00\x03\x00\x04\x00'})

    def test_fix_time_units_and_checksum(self):
        rmc, gga = gps.nmea(self.report()).splitlines()
        self.assertIn(',120000.00,A,3745.0000000,N,12230.0000000,W,1.94,90.00,061026,', rmc)
        self.assertIn(',1,04,1.00,32.00,M,10.00,M,,', gga)
        for line in (rmc, gga):
            body, checksum = line[1:].split('*')
            value = 0
            for char in body:
                value ^= ord(char)
            self.assertEqual(checksum, f'{value:02X}')

    def test_intermediate_and_non_gnss_are_never_fixes(self):
        for report in (self.report(status=1), self.report(technology=32), self.report(status=7)):
            self.assertFalse(report['fix'])
            rmc, gga = gps.nmea(report).splitlines()
            self.assertIn(',V,,,,,', rmc)
            self.assertIn(',,,,,0,00,', gga)
            self.assertNotIn('3745.', rmc + gga)

    def test_coordinate_rounding_carries_degrees(self):
        self.assertEqual(gps.coordinate(12.999999999999, True), ('1300.0000000', 'N'))

    def test_nonfinite_or_missing_coordinate(self):
        for value in (float('nan'), float('inf'), 91):
            report = {1: struct.pack('<I', 0), 0x23: struct.pack('<I', 1),
                      0x10: struct.pack('<d', value), 0x11: struct.pack('<d', 10)}
            self.assertFalse(gps.position(report)['fix'])

    def test_pty_preserves_existing_path_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            path = str(Path(directory) / 'gps')
            stream = gps.NmeaPty(path)
            try:
                with self.assertRaises(FileExistsError):
                    gps.NmeaPty(path)
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                try:
                    stream.write(gps.nmea(self.report()))
                    self.assertIn(b'$GNRMC,', os.read(fd, 4096))
                finally:
                    os.close(fd)
            finally:
                stream.close()
            self.assertFalse(os.path.lexists(path))


if __name__ == '__main__':
    unittest.main()
