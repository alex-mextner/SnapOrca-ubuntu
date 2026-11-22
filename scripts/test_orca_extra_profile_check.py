"""Focused regression cases for vendor metadata alongside filament presets."""

import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest

from orca_extra_profile_check import check_filament_compatible_printers


class FilamentMetadataTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp_dir.cleanup)
        self.profiles_dir = Path(self.temp_dir.name)
        self.filament_dir = self.profiles_dir / "Snapmaker" / "filament"
        self.filament_dir.mkdir(parents=True)

    def write_json(self, path, data):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data), encoding="utf-8")

    def test_metadata_files_do_not_need_profile_names(self):
        metadata = {
            "filament_allow_list.json": {"schema_version": 1, "sections": {}},
            "filament_compatibility.json": {"compatibility": {}, "description": "rules"},
            "filament_hot_bed_nozzles.json": {"btGESP": {"support": ["PLA"]}},
        }
        for filename, data in metadata.items():
            self.write_json(self.filament_dir / filename, data)
        try:
            errors = check_filament_compatible_printers(self.filament_dir)
        except KeyError as error:
            self.fail(f"Known Snapmaker rule metadata must not require a profile field: {error}")
        self.assertEqual(errors, 0)

    def test_tracked_metadata_files_do_not_need_profile_names(self):
        tracked_profiles = Path(__file__).resolve().parent.parent / "resources" / "profiles"
        for filename in (
            "filament_allow_list.json",
            "filament_compatibility.json",
            "filament_hot_bed_nozzles.json",
            "filaments_colours.json",
        ):
            source = tracked_profiles / "Snapmaker" / "filament" / filename
            (self.filament_dir / filename).write_bytes(source.read_bytes())
        self.assertEqual(check_filament_compatible_printers(self.filament_dir), 0)

    def test_valid_named_profile_is_still_checked(self):
        path = self.filament_dir / "PLA.json"
        self.write_json(path, {"name": "PLA", "instantiation": "true", "compatible_printers": ["TestPrinter"]})
        self.assertEqual(check_filament_compatible_printers(self.filament_dir), 0)
        self.write_json(path, {"name": "PLA", "instantiation": "true"})
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(check_filament_compatible_printers(self.filament_dir), 1)

    def test_real_profile_without_name_still_fails(self):
        self.write_json(self.filament_dir / "PLA.json", {"instantiation": "true", "compatible_printers": ["TestPrinter"]})
        with self.assertRaises(KeyError):
            check_filament_compatible_printers(self.filament_dir)

    def test_metadata_still_requires_valid_json_and_unique_keys(self):
        path = self.filament_dir / "filament_allow_list.json"
        for content in ('{"schema_version":', '{"schema_version": 1, "schema_version": 1}'):
            with self.subTest(content=content):
                path.write_text(content, encoding="utf-8")
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(check_filament_compatible_printers(self.filament_dir), 1)

    def test_same_filename_in_other_vendor_is_not_exempt(self):
        other_dir = self.profiles_dir / "OtherVendor" / "filament"
        self.write_json(other_dir / "filament_allow_list.json", {"schema_version": 1, "sections": {}})
        with self.assertRaises(KeyError):
            check_filament_compatible_printers(other_dir)

    def test_nested_same_filename_is_not_exempt(self):
        self.write_json(self.filament_dir / "nested" / "filament_allow_list.json", {"schema_version": 1, "sections": {}})
        with self.assertRaises(KeyError):
            check_filament_compatible_printers(self.filament_dir)


if __name__ == "__main__":
    unittest.main()
