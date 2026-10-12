import copy
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import unittest
import zipfile

import packages


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.package = json.loads((packages.ROOT / "plugins/polisher/plugin.json").read_text())

    def test_supported_manifest_contains_functional_instruction(self):
        packages.validate(self.package)
        self.assertIn("Preserve", self.package["contribution"]["instructions"]["default"])

    def test_reject_unsupported_sdk_runtime_and_capabilities(self):
        for key, bad in (("schemaVersion", 99), ("sdkVersion", 2),
                         ("runtime", "native-dylib"), ("capabilities", ["credentials.read"]),
                         ("minimumHostVersion", "1.01.0"), ("id", "../../other")):
            with self.subTest(key=key):
                item = copy.deepcopy(self.package)
                item[key] = bad
                with self.assertRaises(ValueError):
                    packages.validate(item)

    def test_reject_unknown_interpreter_and_oversized_instruction(self):
        for contribution in ({"type": "shell", "instructions": {"default": "echo test"}},
                             {"type": "ai.prompt.v1", "instructions": {"default": ""}},
                             {"type": "ai.prompt.v1", "instructions": {"default": "x" * 32769}},
                             {"type": "ai.prompt.v1", "instructions": {"default": "x\0y"}},
                             {"type": "ai.prompt.v1", "instructions": {"default": "ok"}, "command": "sh"}):
            item = copy.deepcopy(self.package)
            item["contribution"] = contribution
            with self.assertRaises(ValueError):
                packages.validate(item)

    def test_platform_inventory_preserves_mobile_plugins(self):
        items = packages.manifests(packages.ROOT)
        self.assertEqual(len([p for p in items if "macos" in p["platforms"]]), 21)
        self.assertEqual(len([p for p in items if p.get("platforms", {}).get("macos", {}).get("distribution") == "download"]), 8)
        for platform in ("ios", "android"):
            self.assertTrue({"builtin.ask", "builtin.poem", "builtin.art", "builtin.polisher"} <= {p["id"] for p in items if platform in p["platforms"]})
        chord = next(p for p in items if p["id"] == "builtin.fly-chord-learning")
        self.assertEqual(set(chord["platforms"]), packages.PLATFORMS)

    def test_reject_unsafe_source_paths_and_symlinks(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "escape").symlink_to(root.parent)
            for path in ("../secret", "/absolute", "native/../../secret", "native\\bad", "escape/secret", "native//file"):
                with self.subTest(path=path), self.assertRaises(ValueError):
                    packages.safe_path(root, path)

    def test_packages_and_native_source_bundle_are_reproducible_and_pinned(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / "one", root / "two"
            catalog = packages.build(packages.ROOT, first, "1.1.0")
            packages.build(packages.ROOT, second, "1.1.0")
            self.assertEqual({p.name: p.read_bytes() for p in first.iterdir()},
                             {p.name: p.read_bytes() for p in second.iterdir()})
            for entry in catalog["plugins"]:
                data = (first / entry["downloadAssetName"]).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
                manifest = json.loads(data)
                self.assertEqual(manifest["licenseText"], (packages.ROOT / "LICENSE").read_text())
                self.assertEqual(manifest["contribution"], entry["contribution"])
            with zipfile.ZipFile(first / "native-sources-1.1.0.zip") as archive:
                source_map = json.loads(archive.read("source-map.json"))
                for entry in source_map["files"]:
                    data = archive.read(entry["source"])
                    self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
                self.assertIn("LICENSES/MIT-legacy.txt", archive.namelist())

    def test_duplicate_plugin_and_host_destination_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            shutil.copytree(packages.ROOT / "plugins", root / "plugins")
            duplicate = root / "plugins/duplicate"
            duplicate.mkdir()
            (duplicate / "plugin.json").write_text(json.dumps(self.package))
            with self.assertRaisesRegex(ValueError, "duplicate plugin"):
                packages.manifests(root)
            shutil.rmtree(duplicate)
            shutil.copytree(packages.ROOT / "native", root / "native")
            source_map = json.loads((packages.ROOT / "source-map.json").read_text())
            source_map["files"].append(source_map["files"][0])
            (root / "source-map.json").write_text(json.dumps(source_map))
            with self.assertRaisesRegex(ValueError, "duplicate host destination"):
                packages.native_sources(root, {p["id"] for p in packages.manifests(root)})


if __name__ == "__main__":
    unittest.main()
