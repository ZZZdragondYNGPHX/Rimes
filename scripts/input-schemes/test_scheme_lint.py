"""Contract tests for scheme_lint, preserving the platform-neutral upstream package contract."""

import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

import scheme_lint as lint

SCHEMA = """schema:
  schema_id: demo
  name: 测试音形
engine:
  processors: [lua_processor@*demo/topup, speller, selector, express_editor]
  segmentors: [abc_segmentor]
  translators: [table_translator]
translator:
  dictionary: demo
speller:
  alphabet: abcdefghijklmnopqrstuvwxyz
"""


def fixture() -> dict[str, bytes]:
    return {
        "demo.schema.yaml": SCHEMA.encode(),
        "demo.dict.yaml": "---\nname: demo\nversion: '1'\n...\n示例\taa\n".encode(),
        "lua/demo/topup.lua": b"return function(key, env) return 2 end",
        "default.yaml": b"schema_list: [{schema: demo}]\nswitcher: {caption: old}\nkey_binder: {bindings: []}\n",
    }


def archive(files: dict[str, bytes], compressed: bool = False) -> Path:
    handle = tempfile.NamedTemporaryFile(suffix=".zip", delete=False)
    with zipfile.ZipFile(handle, "w", zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED) as bundle:
        for path in sorted(files):
            bundle.writestr(path, files[path])
    handle.close()
    return Path(handle.name)


class InspectTests(unittest.TestCase):
    def inspect(self, files: dict[str, bytes]) -> lint.Report:
        return lint.inspect_files(files)

    def test_scheme_identity_is_separate_from_frontend_layout(self):
        files = fixture()
        files["hamster.yaml"] = b"keyboards: [{name: xingmao, rows: []}]"
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertEqual([scheme.id for scheme in report.schemes], ["demo"])
        self.assertEqual(report.schemes[0].name, "测试音形")
        self.assertEqual(report.schemes[0].blocking, [])
        self.assertTrue(report.schemes[0].recommended)
        self.assertIn("hamster.yaml", report.skipped)

    def test_global_settings_are_not_permitted_resources(self):
        files = fixture()
        files["default.custom.yaml"] = b"patch: {schema_list: [{schema: replace_existing}]}"
        files["installation.yaml"] = b"installation_id: private"
        files["demo.custom.yaml"] = b"patch: {speller/auto_select: true}"
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertNotIn("default.custom.yaml", report.permitted)
        self.assertNotIn("installation.yaml", report.permitted)
        self.assertIn("demo.custom.yaml", report.permitted)

    def test_enclosing_folder_is_the_package_root(self):
        wrapped = {"方案/" + path: data for path, data in fixture().items()}
        report = self.inspect(wrapped)
        self.assertIsNone(report.fatal)
        self.assertEqual(report.prefix, "方案/")
        self.assertEqual(report.importable, ["demo"])

    def test_schemas_in_two_directories_are_rejected(self):
        files = fixture()
        files["other/extra.schema.yaml"] = b"schema: {schema_id: extra, name: x}"
        self.assertIn("several directories", self.inspect(files).fatal)

    def test_missing_primary_dictionary_and_dependency_block_the_scheme(self):
        files = fixture()
        del files["demo.dict.yaml"]
        files["demo.schema.yaml"] = SCHEMA.replace("name: 测试音形", "name: 测试音形\n  dependencies: [missing_schema]").encode()
        report = self.inspect(files)
        issues = " ".join(report.schemes[0].blocking)
        self.assertIn("demo.dict.yaml", issues)
        self.assertIn("missing_schema.schema.yaml", issues)
        self.assertEqual(report.importable, [])

    def test_missing_dictionary_import_blocks_and_cyclic_imports_are_fatal(self):
        files = fixture()
        files["demo.dict.yaml"] = b"---\nname: demo\nimport_tables: [absent]\n...\n"
        self.assertTrue(any("absent.dict.yaml" in issue for issue in self.inspect(files).schemes[0].blocking))
        files["absent.dict.yaml"] = b"---\nname: absent\nimport_tables: [demo]\n...\n"
        self.assertIn("cyclic", self.inspect(files).fatal)

    def test_required_lua_module_is_fatal_optional_one_is_reported(self):
        files = fixture()
        files["lua/demo/topup.lua"] = b"local m = require('demo.missing')\nreturn m"
        self.assertIn("lua/demo/missing.lua", self.inspect(files).fatal)
        files["lua/demo/topup.lua"] = b"local ok, lfs = pcall(require, 'lfs')\nlocal launch = os.execute\nreturn function() return 2 end"
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertTrue(any("lfs" in warning for warning in report.warnings))
        self.assertTrue(report.unsupported)
        self.assertEqual(report.schemes[0].blocking, [])

    def test_missing_lua_component_blocks_the_scheme(self):
        files = fixture()
        del files["lua/demo/topup.lua"]
        self.assertTrue(any("lua/demo/topup.lua" in issue for issue in self.inspect(files).schemes[0].blocking))

    def test_opencc_paths_outside_the_package_are_fatal(self):
        for path in ("/private/file", "../outside.txt", "nested/../../outside.txt", "C:\\outside.txt"):
            files = fixture()
            files["opencc/bad.json"] = json.dumps(
                {"conversion_chain": [{"dict": {"type": "group", "dicts": [{"type": "text", "file": path}]}}]}
            ).encode()
            self.assertIsNotNone(self.inspect(files).fatal, path)

    def test_opencc_same_directory_reference_is_allowed(self):
        files = fixture()
        files["opencc/safe.json"] = b'{"conversion_chain":[{"dict":{"file":"./safe.txt"}}]}'
        files["opencc/safe.txt"] = "甲\t乙\n".encode()
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertFalse(any("safe.txt" in warning for warning in report.warnings))

    def test_native_resource_ids_outside_the_package_are_fatal(self):
        for key, value in (("dictionary", "../outside"), ("prism", "/private/cache"),
                           ("user_dict", "../../user"), ("opencc_config", "../other.json"),
                           ("user_dict", "en_dicts/cn_en")):
            files = fixture()
            files["demo.schema.yaml"] = (SCHEMA + f"\nauxiliary:\n  {key}: '{value}'\n").encode()
            self.assertIsNotNone(self.inspect(files).fatal, key)
        files = fixture()
        files["symbols.yaml"] = b"punctuator: {__include: '../../outside:/punctuator'}"
        self.assertIsNotNone(self.inspect(files).fatal)

    def test_optional_reference_still_needs_its_file(self):
        files = fixture()
        files["demo.schema.yaml"] = (SCHEMA + "__patch:\n  - grammar:/hant?\n").encode()
        self.assertIn("grammar.yaml", self.inspect(files).fatal)
        files["grammar.yaml"] = b"hant: {}\n"
        self.assertIsNone(self.inspect(files).fatal)

    def test_illegal_yaml_controls_are_reported_not_fatal(self):
        files = fixture()
        files["symbols.yaml"] = "punctuator:\n  symbols:\n    '/x': ['a\u0011\u0010b']\n".encode()
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertTrue(any("symbols.yaml" in w and "U+0010" in w and "U+0011" in w for w in report.warnings))

    def test_depth_budget_counts_parser_events(self):
        files = fixture()
        files["bad.yaml"] = ("value: " + "[']'," * 90 + "null" + "]" * 90).encode()
        self.assertIsNotNone(self.inspect(files).fatal)
        files["bad.yaml"] = b"a: &a [x, x]\nb: &b [*a, *a]\nc: &c [*b, *b]\n"
        self.assertIsNone(self.inspect(files).fatal)

    def test_empty_configuration_is_fatal(self):
        files = fixture()
        files["notes.yaml"] = b"# only a comment\n"
        self.assertIn("notes.yaml", self.inspect(files).fatal)

    def test_dictionary_header_tolerates_tabs_after_the_terminator(self):
        files = fixture()
        files["demo.dict.yaml"] = "# exported\t\n\t\n---\t\nname: demo\t\nversion: '1'\t\n...\t\n示例\taa\n".encode()
        self.assertEqual(self.inspect(files).importable, ["demo"])

    def test_dictionary_without_terminator_is_fatal(self):
        files = fixture()
        files["demo.dict.yaml"] = "---\nname: demo\n示例\taa\n".encode()
        self.assertIn("end marker", self.inspect(files).fatal)

    def test_symbol_duplicates_warn_but_schema_duplicates_are_fatal(self):
        files = fixture()
        files["symbols.yaml"] = b"punctuator:\n  symbols:\n    '/x': [first]\n    '/x': [last]\n"
        report = self.inspect(files)
        self.assertIsNone(report.fatal)
        self.assertTrue(any("/x" in warning for warning in report.warnings))
        files["demo.schema.yaml"] = (SCHEMA + "\nschema: {schema_id: replacement}\n").encode()
        self.assertIn("duplicate", self.inspect(files).fatal)

    def test_dependency_order_and_cycles(self):
        files = fixture()
        files["demo.schema.yaml"] = SCHEMA.replace("name: 测试音形", "name: 测试音形\n  dependencies: [aux]").encode()
        files["aux.schema.yaml"] = "schema: {schema_id: aux, name: 辅助}\ntranslator: {dictionary: demo}\n".encode()
        report = self.inspect(files)
        self.assertEqual(lint.dependency_order(report, ["demo"]), ["aux", "demo"])
        files["aux.schema.yaml"] = "schema: {schema_id: aux, name: 辅助, dependencies: [demo]}\ntranslator: {dictionary: demo}\n".encode()
        with self.assertRaises(lint.Fatal):
            lint.dependency_order(self.inspect(files), ["demo"])


class ArchiveTests(unittest.TestCase):
    def test_well_formed_archive_passes(self):
        report = lint.inspect_archive(archive(fixture()))
        self.assertIsNone(report.fatal)
        self.assertEqual(report.importable, ["demo"])

    def test_traversal_encryption_and_compression_bomb_are_rejected(self):
        files = fixture()
        files["../escape.txt"] = b"escape"
        self.assertIsNotNone(lint.inspect_archive(archive(files)).fatal)

        path = archive(fixture())
        data = bytearray(path.read_bytes())
        data[data.index(b"PK\x01\x02") + 8] |= 1
        path.write_bytes(data)
        self.assertIn("encrypted", lint.inspect_archive(path).fatal)

        files = fixture()
        files["bomb.txt"] = b" " * 150_000
        self.assertIn("compression-ratio", lint.inspect_archive(archive(files, compressed=True)).fatal)

    def test_symbolic_link_is_rejected(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as bundle:
            for path, data in fixture().items():
                bundle.writestr(path, data)
            link = zipfile.ZipInfo("lua/external.lua")
            link.external_attr = 0o120777 << 16
            bundle.writestr(link, "/outside")
        handle = tempfile.NamedTemporaryFile(suffix=".zip", delete=False)
        handle.write(buffer.getvalue())
        handle.close()
        self.assertIn("symbolic link", lint.inspect_archive(Path(handle.name)).fatal)

    def test_more_than_512_entries_are_rejected(self):
        files = fixture()
        for index in range(lint.MAX_ENTRIES):
            files[f"lua/pad{index}.txt"] = b"x"
        self.assertIsNotNone(lint.inspect_archive(archive(files)).fatal)


if __name__ == "__main__":
    unittest.main()
