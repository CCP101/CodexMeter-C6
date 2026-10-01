from pathlib import Path
import tempfile
import unittest

from upload_ota import validate_firmware


class OtaPreflightTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / "CodexMeter-C6.ino.bin"
        self.header = bytearray(24)
        self.header[0] = 0xE9
        self.header[12] = 13

    def test_accepts_c6_application(self):
        self.path.write_bytes(self.header + bytes(4096))
        validate_firmware(self.path)

    def test_rejects_wrong_chip(self):
        self.header[12] = 9
        self.path.write_bytes(self.header)
        with self.assertRaises(ValueError):
            validate_firmware(self.path)

    def test_rejects_truncated_image(self):
        self.path.write_bytes(b"\xe9")
        with self.assertRaises(ValueError):
            validate_firmware(self.path)

    def test_rejects_image_exceeding_inactive_slot(self):
        self.path.write_bytes(self.header + bytes(0x140000))
        with self.assertRaises(ValueError):
            validate_firmware(self.path)

    def test_rejects_merged_or_bootloader_file(self):
        path = self.path.with_name("CodexMeter-C6.ino.merged.bin")
        path.write_bytes(self.header + bytes(4096))
        with self.assertRaises(ValueError):
            validate_firmware(path)


if __name__ == "__main__":
    unittest.main()
