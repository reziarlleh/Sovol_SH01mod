import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location('readback', Path(__file__).parents[1] / 'tools/verify_readback.py')
readback = importlib.util.module_from_spec(spec)
spec.loader.exec_module(readback)


def hex_image(data):
    rows = []
    for address in range(0, len(data), 16):
        payload = data[address:address + 16]
        row = bytes([len(payload)]) + address.to_bytes(2, 'big') + b'\0' + payload
        rows.append(':' + (row + bytes([-sum(row) & 255])).hex())
    return '\n'.join(rows + [':00000001ff']) + '\n'


class ReadbackTools(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.folder = Path(self.tmp.name)
        self.app = bytearray(b'\x11' * 1280)
        struct.pack_into('<II', self.app, 0, 0x20001000, 0x101)
        self.full = bytearray(b'\xff' * 32768)
        self.full[:len(self.app)] = self.app
        packed = 3600 | 65 << 19 | 9 << 26 | 1 << 30
        words = struct.pack('<3I', 0x31524853, 4, packed)
        self.full[0x7000:0x7014] = words + struct.pack('<2I', zlib.crc32(words), 0xFFFFFFFF)
        self.application = self.write('app.bin', self.app)

    def write(self, name, data):
        path = self.folder / name
        path.write_bytes(data if isinstance(data, (bytes, bytearray)) else data.encode('ascii'))
        return path

    def test_complete_hex_and_bin_match(self):
        for suffix in ['bin', 'hex']:
            path = self.write('full.' + suffix, self.full if suffix == 'bin' else hex_image(self.full))
            result = readback.verify(path, self.application)
            self.assertTrue(result['application_match'])
            self.assertEqual(result['latest_cycle']['remaining_s'], 3600)
            self.assertEqual(result['latest_cycle']['fan_percent'], 100)

    def test_partial_bin_is_not_padded(self):
        with self.assertRaises(ValueError):
            readback.verify(self.write('short.bin', self.full[:-1]), self.application)

    def test_hex_holes_and_duplicates(self):
        lines = hex_image(self.full).splitlines()
        for changed in [lines[:3] + lines[4:], lines[:-1] + [lines[3], lines[-1]]]:
            with self.assertRaises(ValueError):
                readback.verify(self.write('bad.hex', '\n'.join(changed)), self.application)

    def test_hex_checksum(self):
        data = hex_image(self.full)
        with self.assertRaises(ValueError):
            readback.verify(self.write('bad.hex', data[:9] + 'aa' + data[11:]), self.application)

    def test_wrong_firmware(self):
        self.full[500] ^= 1
        with self.assertRaises(ValueError):
            readback.verify(self.write('full.bin', self.full), self.application)

    def test_page_padding(self):
        self.full[len(self.app) + 1] = 0
        with self.assertRaises(ValueError):
            readback.verify(self.write('full.bin', self.full), self.application)

    def test_journal_crc(self):
        self.full[0x7008] ^= 1
        with self.assertRaises(ValueError):
            readback.verify(self.write('full.bin', self.full), self.application)

    def test_untouched_region(self):
        baseline = self.write('before.bin', self.full)
        self.full[0x5000] ^= 1
        with self.assertRaises(ValueError):
            readback.verify(self.write('after.bin', self.full), self.application, previous=baseline)

    def test_two_original_copies(self):
        first = self.write('first.hex', hex_image(self.full))
        second = self.write('second.bin', self.full)
        self.assertTrue(readback.verify(first, compare_copy=second)['complete_copies_match'])
        self.full[-1] ^= 1
        with self.assertRaises(ValueError):
            readback.verify(first, compare_copy=self.write('third.bin', self.full))


if __name__ == '__main__':
    unittest.main()
