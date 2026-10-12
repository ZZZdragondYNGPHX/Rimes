"""Tests for the edits scheme_build makes so upstream files pass the import contract."""

import unittest
from unittest.mock import patch
import json
import subprocess
import zipfile
from pathlib import Path
import tempfile

import yaml

import scheme_build as build
import scheme_lint as lint

ORIGIN = build.Origin("example/upstream", "0" * 40, "file", "MIT")


def item(text: str) -> build.Item:
    return build.Item(text.encode("utf-8"), ORIGIN)


def load(entry: build.Item):
    # libyaml, like the importer: it accepts a tab before a comment.
    return yaml.load(entry.data.decode("utf-8"), Loader=lint._Loader)


class ReferenceTests(unittest.TestCase):
    def test_yaml_suffix_and_leading_slash_are_rewritten(self):
        entry = item(
            "__include: rime_ice.schema.yaml:/\n"
            "speller:\n  algebra:\n    __include: /split_canonicalize\n"
            "__patch:\n  - 'grammar.yaml:/hant?'\n  - default:/menu\n  - :/key_binder/custom_bindings?\n"
            "note: /not/a/reference\n"
        )
        build.normalize_references(entry)
        document = load(entry)
        self.assertEqual(document["__include"], "rime_ice.schema:/")
        self.assertEqual(document["speller"]["algebra"]["__include"], "split_canonicalize")
        self.assertEqual(document["__patch"], ["grammar:/hant?", "default:/menu", "key_binder/custom_bindings?"])
        self.assertEqual(document["note"], "/not/a/reference")
        self.assertEqual(len(entry.modified), 1)

    def test_untouched_file_records_no_modification(self):
        entry = item("__include: default:/menu\n")
        build.normalize_references(entry)
        self.assertEqual(entry.modified, [])
        self.assertEqual(entry.data, b"__include: default:/menu\n")


class DuplicateKeyTests(unittest.TestCase):
    def test_earlier_duplicate_is_removed_and_last_value_kept(self):
        entry = item(
            "translator:\n  dictionary: first\n  enable_user_dict: false\n  dictionary: second\n"
            "punctuator:\n  symbols:\n    '/tz': [a]\n    '/x': [b]\n    '/tz':\n      - c\n      - d\n"
        )
        build.remove_duplicate_keys(entry, "demo.schema.yaml")
        self.assertIsNone(lint.inspect_files({"x.schema.yaml": b"schema: {schema_id: x}", "probe.yaml": entry.data}).fatal)
        document = load(entry)
        self.assertEqual(document["translator"], {"enable_user_dict": False, "dictionary": "second"})
        self.assertEqual(document["punctuator"]["symbols"]["/tz"], ["c", "d"])
        self.assertEqual(len(entry.modified), 2)

    def test_duplicate_directives_are_left_alone(self):
        text = "__patch: a:/x\nvalue: 1\n__patch: b:/y\n"
        entry = item(text)
        build.remove_duplicate_keys(entry, "demo.yaml")
        self.assertEqual(entry.data.decode(), text)


class TolerantSourceTests(unittest.TestCase):
    def test_tab_before_comment_and_byte_order_mark_do_not_stop_edits(self):
        entry = item("\ufeff# 注释\nspeller:\n  auto_select: false\t# 制表符\n  algebra:\n    __include: /local\n")
        build.normalize_references(entry)
        self.assertTrue(entry.data.startswith("\ufeff".encode()))
        self.assertEqual(load(entry)["speller"]["algebra"]["__include"], "local")

    def test_dictionary_header_duplicate_is_removed_without_touching_the_body(self):
        body = "一\ta\n丁\tb\n"
        entry = item("---\nname: demo\nsort: original\nversion: '1'\nsort: by_weight\n...\n" + body)
        build.remove_duplicate_keys(entry, "demo.dict.yaml")
        header = yaml.safe_load(lint.dictionary_header(entry.data, "demo.dict.yaml"))
        self.assertEqual(header["sort"], "by_weight")
        self.assertTrue(entry.data.decode().endswith("...\n" + body))

    def test_require_inside_a_block_comment_is_shielded(self):
        entry = item('--[[\n示例:\n  foo = require("bar")\n]]\nlocal real = require("present")\n')
        build.shield_commented_requires(entry)
        text = entry.data.decode()
        self.assertIn('\n--   foo = require("bar")\n', text)
        self.assertIn('\nlocal real = require("present")\n', text)
        files = {"x.schema.yaml": b"schema: {schema_id: x}", "rime.lua": entry.data, "lua/present.lua": b"return {}"}
        self.assertIsNone(lint.inspect_files(files).fatal)


class DependencyCycleTests(unittest.TestCase):
    def test_back_edge_is_removed_from_the_dependency_not_the_selection(self):
        items = {
            "luna.schema.yaml": item("schema:\n  schema_id: luna\n  dependencies:\n    - stroke\n"),
            "stroke.schema.yaml": item("schema:\n  schema_id: stroke\n  dependencies:\n    - luna\n    - other\n"),
            "other.schema.yaml": item("schema: {schema_id: other, dependencies: [luna, stroke]}\n"),
        }
        build.break_dependency_cycles(items, ["luna"])
        self.assertEqual(load(items["luna.schema.yaml"])["schema"]["dependencies"], ["stroke"])
        self.assertEqual(load(items["stroke.schema.yaml"])["schema"]["dependencies"], ["other"])
        self.assertEqual(load(items["other.schema.yaml"])["schema"]["dependencies"], [])
        report = lint.inspect_files({path: entry.data for path, entry in items.items()})
        self.assertEqual(lint.dependency_order(report, ["luna"]), ["other", "stroke", "luna"])


class SplitTests(unittest.TestCase):
    def test_oversized_table_is_split_and_importers_follow(self):
        original = (build.SPLIT_ABOVE_BYTES, build.SPLIT_PART_BYTES)
        build.SPLIT_ABOVE_BYTES, build.SPLIT_PART_BYTES = 400, 300
        try:
            body = "".join(f"词{index}\tcode{index}\t{index}\n" for index in range(60))
            items = {
                "main.dict.yaml": item("---\nname: main\nversion: '1'\nimport_tables:\n  - big\n  - small\n...\n甲\ta\n"),
                "big.dict.yaml": item("# 表头\n---\nname: big\nversion: '1'\nsort: by_weight\n...\n" + body),
                "small.dict.yaml": item("---\nname: small\nversion: '1'\n...\n乙\tb\n"),
            }
            build.split_oversized_dictionaries(items)
        finally:
            build.SPLIT_ABOVE_BYTES, build.SPLIT_PART_BYTES = original
        parts = sorted(path for path in items if path.startswith("big.part"))
        self.assertGreaterEqual(len(parts), 2)
        rebuilt = b""
        for path in ["big.dict.yaml"] + parts:
            data = items[path].data
            rebuilt += data[data.index(b"\n...\n") + 5:]
            header = yaml.safe_load(lint.dictionary_header(data, path))
            self.assertEqual(header["name"], path[: -len(".dict.yaml")])
            self.assertEqual(header["sort"], "by_weight")
        self.assertEqual(rebuilt.decode(), body)
        imports = yaml.safe_load(lint.dictionary_header(items["main.dict.yaml"].data, "main"))["import_tables"]
        self.assertEqual(imports, ["big", "small"] + [path[: -len(".dict.yaml")] for path in parts])
        # A scheme using the table directly reaches the parts through the table itself.
        own = yaml.safe_load(lint.dictionary_header(items["big.dict.yaml"].data, "big"))["import_tables"]
        self.assertEqual(own, [path[: -len(".dict.yaml")] for path in parts])


class LuaShimTests(unittest.TestCase):
    def test_submodule_component_gets_a_module_and_keeps_its_name_space(self):
        items = {
            "demo.schema.yaml": item(
                "schema: {schema_id: demo}\nengine:\n  processors:\n"
                "    - lua_processor@*wanxiang.unicode*P\n    - lua_filter@*snow.table*t12@named\n    - lua_translator@*plain\n"
            ),
        }
        build.shim_lua_submodules(items)
        processors = load(items["demo.schema.yaml"])["engine"]["processors"]
        self.assertEqual(processors[0], "lua_processor@*rimes_shims.wanxiang_unicode__P@*wanxiang.unicode*P")
        self.assertEqual(processors[1], "lua_filter@*rimes_shims.snow_table__t12@named")
        self.assertEqual(processors[2], "lua_translator@*plain")
        self.assertEqual(
            items["lua/rimes_shims/wanxiang_unicode__P.lua"].data.decode().splitlines()[-1],
            'return require("wanxiang.unicode").P',
        )
        self.assertEqual(lint.lua_path("rimes_shims.wanxiang_unicode__P"), "lua/rimes_shims/wanxiang_unicode__P.lua")


class DefaultTests(unittest.TestCase):
    def test_default_lists_only_package_schemes_and_keeps_presets(self):
        entry = item(
            "config_version: '0.40'\nschema_list:\n  - schema: luna_pinyin\n  - schema: cangjie5\n"
            "menu:\n  page_size: 5\nkey_binder:\n  bindings:\n    __patch:\n      - key_bindings:/emacs_editing\n"
            "ascii_composer:\n  switch_key:\n    Shift_L: noop\ncustom_top_level: yes\n"
        )
        build.regenerate_default(entry, ["demo"], "20260101.abcdef0")
        document = load(entry)
        self.assertEqual(document["schema_list"], [{"schema": "demo"}])
        self.assertEqual(document["menu"], {"page_size": 5})
        self.assertEqual(document["key_binder"]["bindings"]["__patch"], ["key_bindings:/emacs_editing"])
        self.assertEqual(document["ascii_composer"]["switch_key"]["Shift_L"], "noop")
        self.assertNotIn("custom_top_level", document)


class ArchiveTests(unittest.TestCase):
    def test_release_source_checks_bytes_even_when_extracted_tree_is_cached(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder)
            archive = cache / "source.zip"
            build.write_zip(archive, {"demo.schema.yaml": b"schema: {schema_id: demo}\n"})
            digest = build.sha256(archive.read_bytes())
            (cache / "archives").mkdir()
            pinned = cache / "archives" / (digest + ".zip")
            archive.rename(pinned)
            entry = {"id": "demo", "sourceArchive": {"url": "https://example.invalid/source.zip", "sha256": digest}}
            tree = build.fetch_release(entry, cache)
            self.assertTrue((tree / "demo.schema.yaml").is_file())
            pinned.write_bytes(b"changed release")
            with self.assertRaisesRegex(build.BuildError, "checksum mismatch"):
                build.fetch_release(entry, cache)

    def test_release_source_rejects_parent_traversal(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder)
            archive = cache / "source.zip"
            build.write_zip(archive, {"../outside.txt": b"outside"})
            digest = build.sha256(archive.read_bytes())
            (cache / "archives").mkdir()
            archive.rename(cache / "archives" / (digest + ".zip"))
            entry = {"id": "demo", "sourceArchive": {"url": "https://example.invalid/source.zip", "sha256": digest}}
            with self.assertRaisesRegex(build.BuildError, "unsafe release archive path"):
                build.fetch_release(entry, cache)

    def test_archive_is_deterministic_and_within_the_ratio_limit(self):
        files = {"demo.schema.yaml": b"schema: {schema_id: demo, name: x}\ntranslator: {dictionary: demo}\n",
                 "demo.dict.yaml": b"---\nname: demo\n...\n" + b"\xe4\xb8\x80\ta\n" * 200_000,
                 "blank.txt": b" " * 500_000, "empty.txt": b""}
        with tempfile.TemporaryDirectory() as folder:
            first, second = Path(folder) / "a.zip", Path(folder) / "b.zip"
            build.write_zip(first, files)
            build.write_zip(second, dict(reversed(list(files.items()))))
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertIsNone(lint.inspect_archive(first).fatal)
            with zipfile.ZipFile(first) as archive:
                for info in archive.infolist():
                    self.assertLessEqual(info.file_size, max(1, info.compress_size) * lint.MAX_COMPRESSION_RATIO)


class VerificationTests(unittest.TestCase):
    def test_data_verification_does_not_claim_native_typing(self):
        import argparse
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "reports").mkdir()
            (root / "packages").mkdir()
            report_path = root / "reports/demo.json"
            report_path.write_text(json.dumps({"archive": "demo.zip", "recommended": ["demo"]}))
            build.write_zip(root / "packages/demo.zip", {
                "demo.schema.yaml": b"schema: {schema_id: demo, name: Demo}\n",
            })
            args = argparse.Namespace(output=root, only=["demo"], missing=False)
            with patch.object(build, "load_sources", return_value={}), patch.object(
                build, "selected_entries", return_value=[{"id": "demo"}]
            ):
                self.assertEqual(build.command_verify(args), 0)
            result = json.loads(report_path.read_text())["verification"]
            self.assertTrue(result["passed"])
            self.assertIsNone(result["deploy"])
            self.assertEqual(result["kind"], "python-data-contract")

    def test_invalid_archive_returns_failure(self):
        import argparse
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "reports").mkdir()
            (root / "packages").mkdir()
            (root / "reports/demo.json").write_text(json.dumps({"archive": "demo.zip", "recommended": ["demo"]}))
            (root / "packages/demo.zip").write_bytes(b"invalid zip")
            args = argparse.Namespace(output=root, only=["demo"], missing=False)
            with patch.object(build, "load_sources", return_value={}), patch.object(
                build, "selected_entries", return_value=[{"id": "demo"}]
            ):
                self.assertEqual(build.command_verify(args), 1)



if __name__ == "__main__":
    unittest.main()
