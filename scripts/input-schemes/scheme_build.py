#!/usr/bin/env python3
"""Build standard RIMES input-scheme packages from pinned upstream sources.

Reads `Catalog/input-schemes/sources.json`, fetches each upstream repository
at its pinned revision, assembles a self-contained Rime data directory that
satisfies the import contract checked by `scheme_lint.py`, and writes one ZIP
per package together with a report.

A package is self-contained because the importer deploys it as both the shared
and the user data directory: presets, dependency schemes, dictionaries, the
essay vocabulary and OpenCC data all have to be inside it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.request
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

import yaml

import scheme_lint as lint

REPO_ROOT = Path(__file__).resolve().parents[2]
CATALOG_DIR = REPO_ROOT / "Catalog" / "input-schemes"
SOURCES_PATH = CATALOG_DIR / "sources.json"
DEFAULT_OUTPUT = REPO_ROOT / "build" / "input-schemes"
MANIFEST_NAME = "rimes-package.yaml"
ZIP_TIMESTAMP = (1980, 1, 1, 0, 0, 0)
PRESET_SECTIONS = lint.DEFAULT_SECTIONS + ("switcher",)
LFS_MARKER = b"version https://git-lfs.github.com/spec/"
# Dictionaries above the importer's 32 MB per-file limit are split into parts.
SPLIT_ABOVE_BYTES = 30 * 1024 * 1024
SPLIT_PART_BYTES = 24 * 1024 * 1024
# Import tables upstream leaves for the user to create.
USER_TABLE = re.compile(r"(custom|user|private|personal)", re.IGNORECASE)
DIRECTIVE_KEY = re.compile(r"^__|/[+=]$")
# Frontend themes and deployment state that are never scheme data.
EXCLUDED_ROOT_SUFFIXES = (".trime.yaml", ".custom.yaml.bak", ".userdb.txt")
OPEN_LICENSES = {
    "GPL-3.0", "GPL-2.0", "LGPL-3.0", "LGPL-2.1", "AGPL-3.0", "MIT", "Apache-2.0",
    "BSD-3-Clause", "BSD-2-Clause", "MPL-2.0", "CC-BY-4.0", "CC-BY-SA-4.0", "CC0-1.0", "Unlicense",
    "GPL-3.0-only", "GPL-3.0-or-later", "LGPL-3.0-only", "LGPL-3.0-or-later",
}


class BuildError(Exception):
    pass


@dataclass
class Origin:
    repo: str
    revision: str
    path: str
    license: str | None = None


@dataclass
class Item:
    data: bytes
    origin: Origin
    modified: list[str] = field(default_factory=list)


@dataclass
class Tree:
    """One fetched upstream directory that files are resolved against."""
    repo: str
    revision: str
    root: Path
    base: Path
    license: str | None
    local: bool = False

    def origin(self, relative: str) -> Origin:
        prefix = self.root.relative_to(self.base).as_posix()
        return Origin(self.repo, self.revision, relative if prefix == "." else f"{prefix}/{relative}", self.license)

    def read(self, relative: str) -> bytes | None:
        path = self.root / relative
        if not path.is_file() or path.is_symlink():
            return None
        return path.read_bytes()


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_sources() -> dict:
    return json.loads(SOURCES_PATH.read_text(encoding="utf-8"))


_fetch_locks: dict[str, threading.Lock] = {}
_fetch_guard = threading.Lock()


def fetch(repo: str, revision: str, cache: Path) -> Path:
    """Download and unpack one pinned revision; returns the unpacked tree."""
    key = repo.replace("/", "--") + "@" + revision[:12]
    with _fetch_guard:
        lock = _fetch_locks.setdefault(key, threading.Lock())
    with lock:
        return _fetch(repo, revision, cache, key)


def _fetch(repo: str, revision: str, cache: Path, key: str) -> Path:
    tree = cache / "trees" / key
    if (tree / ".complete").exists():
        return tree
    archive = cache / "archives" / (key + ".tar.gz")
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        url = f"https://codeload.github.com/{repo}/tar.gz/{revision}"
        request = urllib.request.Request(url, headers={"User-Agent": "rimes-scheme-build"})
        token = os.environ.get("GITHUB_TOKEN")
        if token:
            request.add_header("Authorization", f"Bearer {token}")
        partial = archive.with_suffix(".partial")
        try:
            with urllib.request.urlopen(request, timeout=120) as response, open(partial, "wb") as handle:
                shutil.copyfileobj(response, handle, 1 << 20)
        except Exception as error:
            partial.unlink(missing_ok=True)
            raise BuildError(f"cannot download {repo}@{revision[:12]}: {error}") from None
        partial.rename(archive)
    if tree.exists():
        shutil.rmtree(tree)
    staging = Path(tempfile.mkdtemp(dir=cache, prefix="unpack-"))
    try:
        with tarfile.open(archive) as bundle:
            bundle.extractall(staging, filter="data")
        children = [child for child in staging.iterdir() if child.is_dir()]
        if len(children) != 1:
            raise BuildError(f"{repo}: unexpected archive layout")
        tree.parent.mkdir(parents=True, exist_ok=True)
        children[0].rename(tree)
        (tree / ".complete").write_text(revision + "\n")
    except (tarfile.TarError, OSError) as error:
        archive.unlink(missing_ok=True)
        raise BuildError(f"cannot unpack {repo}@{revision[:12]}: {error}") from None
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return tree


def license_text(tree: Path) -> tuple[str, bytes] | None:
    for name in ("LICENSE", "LICENSE.txt", "LICENSE.md", "LICENCE", "COPYING", "COPYING.txt", "UNLICENSE", "License", "license"):
        path = tree / name
        if path.is_file():
            return name, path.read_bytes()
    return None


def fetch_release(entry: dict, cache: Path) -> Path:
    """Use a checksum-pinned release asset when it contains generated resources."""
    spec = entry["sourceArchive"]
    digest = spec["sha256"]
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise BuildError("sourceArchive requires a SHA-256 checksum")
    archive = cache / "archives" / (digest + ".zip")
    archive.parent.mkdir(parents=True, exist_ok=True)
    if not archive.exists():
        request = urllib.request.Request(spec["url"], headers={"User-Agent": "rimes-scheme-build"})
        partial = archive.with_suffix(".partial")
        try:
            with urllib.request.urlopen(request, timeout=120) as response, open(partial, "wb") as handle:
                shutil.copyfileobj(response, handle, 1 << 20)
            partial.rename(archive)
        finally:
            partial.unlink(missing_ok=True)
    if sha256(archive.read_bytes()) != digest:
        raise BuildError(f"release checksum mismatch: {archive}")
    tree = cache / "trees" / (entry["id"] + "-release-" + digest[:16])
    if (tree / ".complete").is_file():
        return tree
    staging = Path(tempfile.mkdtemp(dir=cache, prefix="release-"))
    try:
        with zipfile.ZipFile(archive) as bundle:
            for info in bundle.infolist():
                path = Path(info.filename)
                if path.is_absolute() or ".." in path.parts or "\\" in info.filename:
                    raise BuildError(f"unsafe release archive path: {info.filename}")
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise BuildError(f"release archive contains a symbolic link: {info.filename}")
            bundle.extractall(staging)
        tree.parent.mkdir(parents=True, exist_ok=True)
        if tree.exists():
            shutil.rmtree(tree)
        staging.rename(tree)
        (tree / ".complete").write_text(digest + "\n")
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return tree


class Resolver:
    """Finds files by Rime resource name across the package root and providers."""

    def __init__(self, sources: dict, cache: Path, primary: Tree):
        self.sources = sources
        self.cache = cache
        self.trees: list[Tree] = [primary]
        self.provider_trees: dict[str, Tree] = {}
        self.lookup: dict[str, str] = {}
        for name, provider in sources.get("providers", {}).items():
            for resource in provider.get("provides", []):
                self.lookup.setdefault(resource, name)

    def provider(self, name: str) -> Tree:
        if name not in self.provider_trees:
            spec = self.sources["providers"][name]
            if spec.get("type") == "local":
                base = REPO_ROOT / spec["path"]
                if not base.is_dir():
                    raise BuildError(f"provider {name}: {spec['path']} is missing ({spec.get('hint', 'see sources.json')})")
                tree = Tree(spec["origin"], spec.get("version", "local"), base, base, spec.get("license"), local=True)
            else:
                base = fetch(spec["repo"], spec["revision"], self.cache)
                tree = Tree(spec["repo"], spec["revision"], base / spec.get("root", ""), base, spec.get("license"))
            self.provider_trees[name] = tree
        return self.provider_trees[name]

    def find(self, relative: str) -> tuple[bytes, Tree] | None:
        """Package root and its overlays first, then the provider registered for this file."""
        for tree in self.trees:
            data = tree.read(relative)
            if data is not None:
                return data, tree
        name = self.lookup.get(relative)
        if name is None:
            return None
        tree = self.provider(name)
        data = tree.read(relative)
        return (data, tree) if data is not None else None


def compose(data: bytes, path: str) -> yaml.Node | None:
    try:
        return yaml.compose(data.decode("utf-8"), Loader=lint._Loader)
    except (yaml.YAMLError, UnicodeDecodeError):
        return None


class Assembly:
    """Collects the dependency closure of the selected schemes."""

    def __init__(self, resolver: Resolver):
        self.resolver = resolver
        self.items: dict[str, Item] = {}
        self.notes: list[str] = []
        self.pending: list[str] = []
        self.flat: dict[str, str] = {}
        self.uses_lua = False
        # Optional `name:/path?` references whose file no source provides.
        self.stubs: dict[str, set[str]] = {}

    def add(self, relative: str, data: bytes, tree: Tree, source_path: str | None = None) -> bool:
        if relative in self.items:
            return False
        if data.startswith(LFS_MARKER):
            raise BuildError(f"{source_path or relative} is a Git LFS pointer; the data is not in the source archive")
        self.items[relative] = Item(data, tree.origin(source_path or relative))
        self.pending.append(relative)
        return True

    def want(self, relative: str, required_by: str, optional: bool = False) -> bool:
        if relative in self.items:
            return True
        found = self.resolver.find(relative)
        if found is None:
            if not optional:
                self.notes.append(f"{required_by} needs {relative}, which no source provides")
            return False
        self.add(relative, found[0], found[1])
        return True

    def want_resource(self, name: str, suffix: str, required_by: str, optional: bool = False) -> bool:
        """A dictionary or text table referenced by Rime resource id. Ids naming a
        subdirectory are legal for Rime but not for the importer, so such files
        move to the package root under a flattened name."""
        if "/" in name:
            flat = self.flat.get(name) or re.sub(r"[^A-Za-z0-9_.-]", "_", name.replace("/", "_"))
            if flat + suffix in self.items:
                return True
            found = self.resolver.find(name + suffix)
            if found is not None and lint.valid_id(flat):
                self.flat[name] = flat
                self.add(flat + suffix, found[0], found[1], source_path=name + suffix)
                return True
        return self.want(name + suffix, required_by, optional)

    def run(self, schemas: list[str]) -> None:
        for schema in schemas:
            if not self.want(schema + ".schema.yaml", "catalog"):
                raise BuildError(f"scheme {schema} is not in the source tree")
        self.want("default.yaml", "package")
        while self.pending:
            relative = self.pending.pop()
            item = self.items[relative]
            if relative.endswith(".dict.yaml"):
                self._scan_dictionary(relative, item)
            elif relative.endswith(".yaml"):
                self._scan_config(relative, item)
            elif relative.endswith(".lua"):
                self._scan_lua(relative, item)
            elif relative.startswith("opencc/") and relative.endswith(".json"):
                self._scan_opencc(relative, item)
        if self.uses_lua:
            self._add_lua_data()
            self._add_unreferenced_lua()
        self._add_referenced_text()
        self._add_stubs()

    def _scan_config(self, relative: str, item: Item) -> None:
        node = compose(item.data, relative)
        if node is None:
            return
        if relative.endswith(".schema.yaml"):
            schema_id = relative[: -len(".schema.yaml")]
            for dependency in lint.string_list(lint.child(lint.child(node, "schema"), "dependencies")):
                self.want(dependency + ".schema.yaml", relative)
            patch = self.resolver.trees[0].read(schema_id + ".custom.yaml")
            if patch is not None and compose(patch, schema_id) is not None:
                self.add(schema_id + ".custom.yaml", patch, self.resolver.trees[0])
            self._scan_simplifiers(relative, node)
        fields = lint.scalar_fields(node)
        stable = {value for key, value in fields if key.split("/")[-1] == "db_class" and value in ("stabledb", "tabledb")}
        for key, value in fields:
            leaf = key.split("/")[-1]
            if not value:
                continue
            if leaf == "dictionary":
                self.want_resource(value, ".dict.yaml", relative, optional=not self._is_main_dictionary(node, value))
            elif leaf == "user_dict" and stable:
                self.want_resource(value, ".txt", relative, optional=True)
            elif leaf == "import_preset":
                self.want(value + ".yaml", relative)
            elif leaf in ("__include", "__patch") and ":" in value:
                name, _, target = value.partition(":")
                name = name.removesuffix(".yaml")
                if not name:
                    continue  # `:/path` points into the same file
                if not self.want(name + ".yaml", relative, optional=value.endswith("?")) and value.endswith("?"):
                    self.stubs.setdefault(name + ".yaml", set()).add(target.rstrip("?").strip("/"))
            elif leaf == "opencc_config":
                self.want("opencc/" + value, relative)
            elif value.startswith("lua_") and "@" in value:
                self.uses_lua = True
                target = value.split("@")[1]
                if target.startswith("*"):
                    module = target[1:].split("*", 1)[0].replace(".", "/")
                    self.want(f"lua/{module}.lua", relative)
                else:
                    self.want("rime.lua", relative)

    @staticmethod
    def _is_main_dictionary(node: yaml.Node, value: str) -> bool:
        return lint.scalar(lint.child(lint.child(node, "translator"), "dictionary")) == value

    def _scan_simplifiers(self, relative: str, node: yaml.Node) -> None:
        """A simplifier with no opencc_config falls back to OpenCC's t2s.json."""
        for name in lint.string_list(lint.child(lint.child(node, "engine"), "filters")):
            if name != "simplifier" and not name.startswith("simplifier@"):
                continue
            namespace = name.split("@", 1)[1] if "@" in name else "simplifier"
            if lint.scalar(lint.child(lint.child(node, namespace), "opencc_config")) is None:
                self.want("opencc/t2s.json", relative)

    def _scan_dictionary(self, relative: str, item: Item) -> None:
        try:
            header = lint.compose_dictionary_header(item.data, relative)
        except lint.Fatal:
            return
        for table in lint.string_list(lint.child(header, "import_tables")):
            if self.want_resource(table, ".dict.yaml", relative, optional=bool(USER_TABLE.search(table))):
                continue
            if USER_TABLE.search(table) and lint.valid_id(table):
                text = (
                    "# Placeholder generated for a RIMES scheme package. Upstream leaves this table for\n"
                    "# the user to create; Rime skips a missing import, the importer does not.\n"
                    f"---\nname: {table}\nversion: \"0\"\nsort: by_weight\n...\n"
                )
                self.items[table + ".dict.yaml"] = Item(
                    text.encode("utf-8"), Origin("scholay/rimes", "generated", table + ".dict.yaml", "Apache-2.0"),
                    ["generated empty table for an import upstream leaves to the user"])
        vocabulary = lint.scalar(lint.child(header, "vocabulary"))
        preset = lint.scalar(lint.child(header, "use_preset_vocabulary"))
        if vocabulary or (preset or "").lower() == "true":
            self.want((vocabulary or "essay") + ".txt", relative)

    def _scan_lua(self, relative: str, item: Item) -> None:
        self.uses_lua = True
        for module, _ in lint.lua_requirements(item.data.decode("utf-8", errors="replace")):
            if all(c in lint.LUA_MODULE_CHARS for c in module):
                self.want("lua/" + module.replace(".", "/") + ".lua", relative, optional=True)

    def _scan_opencc(self, relative: str, item: Item) -> None:
        try:
            document = json.loads(item.data)
        except ValueError:
            return
        folder = relative.rsplit("/", 1)[0]
        for reference in lint._json_file_references(document):
            local = "/".join(part for part in reference.split("/") if part != ".")
            if local:
                self.want(f"{folder}/{local}", relative, optional=True)

    def _add_stubs(self) -> None:
        """The importer requires the file behind every `name:/path` reference,
        optional or not. An empty node keeps an optional patch a no-op."""
        for relative, targets in sorted(self.stubs.items()):
            if relative in self.items or not lint.valid_id(relative[: -len(".yaml")]):
                continue
            document: dict = {}
            for target in sorted(targets):
                node = document
                for part in [part for part in target.split("/") if part]:
                    node = node.setdefault(part, {})
            text = (
                "# Placeholder generated for a RIMES scheme package. Upstream references this\n"
                "# file optionally; it is absent here, so the referenced nodes are left empty.\n"
                + yaml.safe_dump(document or {"placeholder": True}, allow_unicode=True, default_flow_style=False)
            )
            self.items[relative] = Item(text.encode("utf-8"), Origin("scholay/rimes", "generated", relative, "Apache-2.0"),
                                        ["generated placeholder for an optional upstream reference"])

    def _add_lua_data(self) -> None:
        """Scripts open their tables by path, so take lua/ data files wholesale."""
        tree = self.resolver.trees[0]
        folder = tree.root / "lua"
        if not folder.is_dir():
            return
        for path in sorted(folder.rglob("*")):
            relative = path.relative_to(tree.root).as_posix()
            if path.is_file() and not path.is_symlink() and relative.lower().endswith((".txt", ".json")):
                self.add(relative, path.read_bytes(), tree)

    def _add_unreferenced_lua(self) -> None:
        """Scripts also load modules by computed name, which no scan can follow.
        Take every other module under lua/ whose own requirements the package can
        meet; one with a missing requirement would make the importer refuse it all."""
        tree = self.resolver.trees[0]
        folder = tree.root / "lua"
        if not folder.is_dir():
            return
        candidates: dict[str, list[str]] = {}
        for path in sorted(folder.rglob("*.lua")):
            relative = path.relative_to(tree.root).as_posix()
            if relative in self.items or path.is_symlink() or path.stat().st_size > lint.MAX_LUA_BYTES:
                continue
            try:
                requirements = lint.lua_requirements(path.read_text(encoding="utf-8"))
                candidates[relative] = [lint.lua_path(module) for module, optional in requirements if not optional]
            except (UnicodeDecodeError, lint.Fatal):
                continue
        while True:
            unmet = [relative for relative, needs in candidates.items()
                     if any(need not in self.items and need not in candidates for need in needs)]
            if not unmet:
                break
            for relative in unmet:
                del candidates[relative]
        for relative in sorted(candidates):
            self.add(relative, (tree.root / relative).read_bytes(), tree)
        self.pending.clear()

    def _add_referenced_text(self) -> None:
        """Root text tables are read by name from Lua or listed in configs."""
        tree = self.resolver.trees[0]
        haystack = b"\n".join(
            item.data for relative, item in self.items.items()
            if relative.endswith((".lua", ".yaml")) and not relative.endswith(".dict.yaml")
        )
        for path in sorted(tree.root.glob("*.txt")):
            name = path.name
            if name in self.items or path.is_symlink() or name.lower().endswith(EXCLUDED_ROOT_SUFFIXES):
                continue
            if re.search(rb"(?<![\w.-])" + re.escape(path.stem.encode()) + rb"(?![\w-])", haystack):
                self.add(name, path.read_bytes(), tree)

    def rewrite_flattened(self) -> None:
        """Point references at the flattened names and align dictionary ids."""
        used = {stem: flat for stem, flat in self.flat.items()
                if flat + ".dict.yaml" in self.items or flat + ".txt" in self.items}
        if not used:
            return
        pattern = re.compile(
            r"(?<![\w./-])(" + "|".join(re.escape(stem) for stem in sorted(used, key=len, reverse=True)) + r")(?![\w./-])"
        )
        for relative, item in self.items.items():
            if not relative.endswith(".yaml"):
                continue
            if relative.endswith(".dict.yaml"):
                try:
                    split = len(lint.dictionary_header(item.data, relative))
                except lint.Fatal:
                    continue
                head, body = item.data[:split], item.data[split:]
            else:
                head, body = item.data, b""
            text = head.decode("utf-8", errors="surrogateescape")
            replaced, count = pattern.subn(lambda match: used[match.group(1)], text)
            stem = relative[: -len(".dict.yaml")] if relative.endswith(".dict.yaml") else None
            if stem in used.values():
                replaced, renamed = re.subn(r"(?m)^(name:\s*)[\"']?[^\s\"'#]+[\"']?", lambda m: m.group(1) + stem, replaced, count=1)
                count += renamed
            if count:
                item.data = replaced.encode("utf-8", errors="surrogateescape") + body
                item.modified.append("resource ids rewritten for flattened subdirectory files")


def marked(text: str) -> yaml.Node | None:
    """Compose with source positions. libyaml counts characters after a BOM,
    so callers work on text from `source_text`, which has none."""
    try:
        return yaml.compose(text, Loader=lint._Loader)
    except yaml.YAMLError:
        return None


def source_text(data: bytes) -> tuple[str, str]:
    """(byte-order mark or "", text) so edits by position stay aligned."""
    text = data.decode("utf-8")
    return ("\ufeff", text[1:]) if text.startswith("\ufeff") else ("", text)


def plain(node: yaml.Node | None):
    """Rime's reading of a node: scalars as text, later duplicate keys winning."""
    if isinstance(node, yaml.SequenceNode):
        return [plain(value) for value in node.value]
    if isinstance(node, yaml.MappingNode):
        return {(key.value if isinstance(key, yaml.ScalarNode) else None): plain(value) for key, value in node.value}
    return node.value if node is not None else None


def normalize_references(item: Item) -> None:
    """Rewrite reference spellings Rime accepts and the importer rejects:
    `name.yaml:/path` (the suffix is redundant) and the local forms `/path` and
    `:/path` (the same node as `path`)."""
    bom, text = source_text(item.data)
    root = marked(text)
    if root is None:
        return
    edits: list[tuple[int, int, str]] = []

    def walk(node: yaml.Node, key: str) -> None:
        if isinstance(node, yaml.ScalarNode):
            if key not in ("__include", "__patch") and not key.endswith(("/__include", "/__patch")):
                return
            value = node.value
            name, separator, rest = value.partition(":")
            if separator and name.endswith(".yaml"):
                changed = name[: -len(".yaml")] + ":" + rest
            elif separator and not name and rest.strip("/?"):
                changed = rest.lstrip("/")
            elif not separator and value.startswith("/") and value.strip("/"):
                changed = value.lstrip("/")
            else:
                return
            source = text[node.start_mark.index:node.end_mark.index]
            if value in source:
                edits.append((node.start_mark.index, node.end_mark.index, source.replace(value, changed, 1)))
        elif isinstance(node, yaml.SequenceNode):
            for value in node.value:
                walk(value, key)
        elif isinstance(node, yaml.MappingNode):
            for child_key, value in node.value:
                walk(value, child_key.value if isinstance(child_key, yaml.ScalarNode) else "")

    walk(root, "")
    if not edits:
        return
    for start, end, replacement in sorted(set(edits), reverse=True):
        text = text[:start] + replacement + text[end:]
    item.data = (bom + text).encode("utf-8")
    item.modified.append("configuration references rewritten to the form the importer accepts (same target)")


def remove_duplicate_keys(item: Item, relative: str) -> None:
    """Rime keeps the last of two equal keys; the importer rejects the file.
    Delete the earlier one when that leaves Rime's reading unchanged."""
    split = len(item.data)
    if relative.endswith(".dict.yaml"):
        try:
            split = len(lint.dictionary_header(item.data, relative))
        except lint.Fatal:
            return
    rest = item.data[split:]
    try:
        bom, text = source_text(item.data[:split])
    except UnicodeDecodeError:
        return
    for _ in range(64):
        root = marked(text)
        if root is None:
            return
        target = None
        stack = [root]
        while stack and target is None:
            node = stack.pop()
            if isinstance(node, yaml.SequenceNode):
                stack.extend(node.value)
            elif isinstance(node, yaml.MappingNode):
                last = {key.value: index for index, (key, _) in enumerate(node.value) if isinstance(key, yaml.ScalarNode)}
                for index, (key, value) in enumerate(node.value):
                    if isinstance(key, yaml.ScalarNode) and last[key.value] != index:
                        target = (node, key, value)
                        break
                stack.extend(value for _, value in node.value)
        if target is None:
            return
        mapping, key, value = target
        lines = text.split("\n")
        first = key.start_mark.line
        inline = isinstance(value, yaml.ScalarNode) or value.flow_style
        final = value.end_mark.line if inline else value.end_mark.line - 1
        if DIRECTIVE_KEY.search(key.value) or mapping.flow_style or lines[first][: key.start_mark.column].strip() or final < first:
            return
        candidate = "\n".join(lines[:first] + lines[final + 1:])
        if plain(marked(candidate)) != plain(root):
            return
        text = candidate
        item.data = (bom + text).encode("utf-8") + rest
        item.modified.append(f"earlier duplicate of key {key.value!r} removed; Rime reads the last one")


def append_import_tables(item: Item, relative: str, tables: list[str]) -> None:
    """Add entries to the import_tables list of a dictionary header."""
    split = len(lint.dictionary_header(item.data, relative))
    (bom, head), rest = source_text(item.data[:split]), item.data[split:]
    root = marked(head)
    sequence = lint.child(root, "import_tables")
    lines = head.split("\n")
    if isinstance(sequence, yaml.SequenceNode) and sequence.value and not sequence.flow_style:
        last = sequence.value[-1]
        indent = lines[last.start_mark.line][: last.start_mark.column]
        lines[last.end_mark.line + 1:last.end_mark.line + 1] = [f"{indent}{table}" for table in tables]
    elif isinstance(sequence, yaml.SequenceNode) and sequence.flow_style:
        closing = sequence.end_mark.index - 1
        joined = ", ".join(tables)
        head = head[:closing] + (", " if sequence.value else "") + joined + head[closing:]
        lines = head.split("\n")
    else:
        lines += ["import_tables:"] + [f"  - {table}" for table in tables]
    updated = "\n".join(lines)
    if lint.string_list(lint.child(marked(updated), "import_tables"))[-len(tables):] != tables:
        raise BuildError(f"cannot add split parts to import_tables of {relative}")
    item.data = (bom + updated).encode("utf-8") + rest
    item.modified.append("import_tables extended with the parts of a dictionary split for the 32 MB file limit")


def split_oversized_dictionaries(items: dict[str, Item]) -> None:
    """Keep the first part in place and move the rest to sibling tables. Rime reads
    import_tables one level deep, so every dictionary that pulls the table in, and
    the table itself when a scheme uses it directly, imports the new parts."""
    for relative in sorted(items):
        item = items[relative]
        if not relative.endswith(".dict.yaml") or len(item.data) <= SPLIT_ABOVE_BYTES:
            continue
        stem = relative[: -len(".dict.yaml")]
        head = lint.dictionary_header(item.data, relative)
        terminator = re.match(rb"\n\.\.\.[ \t\r]*\n?", item.data[len(head):])
        if terminator is None:
            continue
        body = item.data[len(head) + terminator.end():]
        chunks, current, size = [], [], 0
        for line in body.splitlines(keepends=True):
            if size + len(line) > SPLIT_PART_BYTES and current:
                chunks.append(b"".join(current))
                current, size = [], 0
            current.append(line)
            size += len(line)
        chunks.append(b"".join(current))
        if len(chunks) < 2:
            continue
        parts = []
        for number, chunk in enumerate(chunks[1:], start=2):
            name = f"{stem}.part{number}"
            header, renamed = re.subn(rb"(?m)^(name:[ \t]*)[^\r\n#]*", lambda match: match.group(1) + name.encode(), head, count=1)
            if not renamed or name + ".dict.yaml" in items:
                raise BuildError(f"cannot split {relative}")
            items[name + ".dict.yaml"] = Item(
                header + terminator.group(0) + chunk, item.origin,
                [f"part {number} of {relative}, split verbatim at a line boundary for the 32 MB file limit"])
            parts.append(name)
        item.data = head + terminator.group(0) + chunks[0]
        item.modified.append(f"split into {len(chunks)} files for the 32 MB file limit; this file keeps the first part")
        for other in sorted(items):
            if not other.endswith(".dict.yaml") or other[: -len(".dict.yaml")] in parts:
                continue
            try:
                header_node = lint.compose_dictionary_header(items[other].data, other)
            except lint.Fatal:
                continue
            if other == relative or stem in lint.string_list(lint.child(header_node, "import_tables")):
                append_import_tables(items[other], other, parts)


LUA_BLOCK_COMMENT = re.compile(r"--\[(=*)\[.*?\]\1\]", re.DOTALL)


def shield_commented_requires(item: Item) -> None:
    """The importer reads `require` line by line and does not know block comments,
    so example code inside one counts as a missing module. Turn those lines into
    line comments; they stay inside the block comment and mean the same to Lua."""
    try:
        text = item.data.decode("utf-8")
    except UnicodeDecodeError:
        return
    changed = False

    def shield(match: re.Match) -> str:
        nonlocal changed
        lines = match.group(0).split("\n")
        for index in range(1, len(lines)):
            if not lines[index].lstrip().startswith("--") and any(p.search(lines[index]) for p in lint.REQUIRE_PATTERNS):
                lines[index] = "-- " + lines[index]
                changed = True
        return "\n".join(lines)

    text = LUA_BLOCK_COMMENT.sub(shield, text)
    if changed:
        item.data = text.encode("utf-8")
        item.modified.append("require lines inside a block comment prefixed with '--' so the importer skips them")


SUBMODULE_COMPONENT = re.compile(r"^(lua_\w+)@\*([A-Za-z0-9_./-]+)((?:\*[A-Za-z_][A-Za-z0-9_]*)+)(?:@(.*))?$")


def shim_lua_submodules(items: dict[str, Item]) -> None:
    """librime-lua accepts `lua_x@*module*field`; the importer cannot parse it.
    Point the component at a one-line module returning that field, and pass the
    original spelling as the name space so the script sees what it always saw."""
    for relative in sorted(items):
        item = items[relative]
        if not relative.endswith(".yaml") or relative.endswith(".dict.yaml"):
            continue
        try:
            bom, text = source_text(item.data)
        except UnicodeDecodeError:
            continue  # Left as is; the contract check reports the encoding.
        root = marked(text)
        if root is None:
            continue
        edits: list[tuple[int, int, str]] = []
        stack = [root]
        while stack:
            node = stack.pop()
            if isinstance(node, yaml.SequenceNode):
                stack.extend(node.value)
            elif isinstance(node, yaml.MappingNode):
                stack.extend(value for _, value in node.value)
            elif isinstance(node, yaml.ScalarNode):
                match = SUBMODULE_COMPONENT.match(node.value)
                if not match:
                    continue
                component, module, fields, name_space = match.groups()
                names = fields.strip("*").split("*")
                shim = "rimes_shims." + re.sub(r"[^A-Za-z0-9_]", "_", module) + "__" + "_".join(names)
                path = "lua/" + shim.replace(".", "/") + ".lua"
                if path not in items:
                    body = (
                        f"-- Generated for a RIMES scheme package: stands in for the component\n"
                        f"-- spelling *{module}{fields}, which the importer cannot parse.\n"
                        f"return require(\"{module}\")" + "".join(f".{name}" for name in names) + "\n"
                    )
                    items[path] = Item(body.encode("utf-8"), Origin("scholay/rimes", "generated", path, "Apache-2.0"),
                                       ["generated module returning one field of an upstream Lua module"])
                changed = f"{component}@*{shim}@{name_space if name_space is not None else '*' + module + fields}"
                source = text[node.start_mark.index:node.end_mark.index]
                if node.value in source:
                    edits.append((node.start_mark.index, node.end_mark.index, source.replace(node.value, changed, 1)))
        if edits:
            for start, end, replacement in sorted(set(edits), reverse=True):
                text = text[:start] + replacement + text[end:]
            item.data = (bom + text).encode("utf-8")
            item.modified.append("Lua components of the form *module*field routed through generated one-line modules")


def remove_dependency(item: Item, dependency: str) -> bool:
    """Drop one entry of schema/dependencies in place, leaving the rest of the file untouched."""
    try:
        bom, text = source_text(item.data)
    except UnicodeDecodeError:
        return False
    node = marked(text)
    sequence = lint.child(lint.child(node, "schema"), "dependencies")
    if not isinstance(sequence, yaml.SequenceNode):
        return False
    lines = text.split("\n")
    name = re.escape(dependency)
    for element in sequence.value:
        if not isinstance(element, yaml.ScalarNode) or element.value != dependency:
            continue
        index = element.start_mark.line
        if sequence.flow_style:
            line, count = re.subn(rf"""(\[|,)\s*["']?{name}["']?\s*(,|\])""",
                                  lambda m: "[" if m.group(1) == "[" and m.group(2) == "," else ("[]" if m.group(1) == "[" else m.group(2)),
                                  lines[index], count=1)
            if not count:
                return False
            lines[index] = line
        elif re.fullmatch(rf"""\s*-\s*["']?{name}["']?\s*(#.*)?\r?""", lines[index]):
            del lines[index]
        else:
            return False
        item.data = (bom + "\n".join(lines)).encode("utf-8")
        return True
    return False


def break_dependency_cycles(items: dict[str, Item], recommended: list[str]) -> None:
    """The importer refuses cyclic schema/dependencies, which Rime itself allows
    (luna_pinyin and stroke look each other up). Remove the edge that points
    back at a scheme already being deployed; that scheme is deployed anyway."""
    def dependencies(schema: str) -> list[str]:
        item = items.get(schema + ".schema.yaml")
        node = compose(item.data, schema) if item else None
        return lint.string_list(lint.child(lint.child(node, "schema"), "dependencies"))

    done: set[str] = set()

    def visit(schema: str, ancestors: tuple[str, ...]) -> None:
        if schema in done or schema + ".schema.yaml" not in items:
            return
        for dependency in dependencies(schema):
            if dependency in ancestors or dependency == schema:
                item = items[schema + ".schema.yaml"]
                if not remove_dependency(item, dependency):
                    raise BuildError(f"{schema} and {dependency} depend on each other and the cycle cannot be edited out")
                item.modified.append(f"removed {dependency} from schema/dependencies to break a dependency cycle")
            else:
                visit(dependency, ancestors + (schema,))
        done.add(schema)

    for schema in recommended + sorted(path[: -len(".schema.yaml")] for path in items if path.endswith(".schema.yaml")):
        visit(schema, ())


def regenerate_default(item: Item, recommended: list[str], version: str) -> None:
    """Keep the upstream preset sections, list only this package's schemes."""
    node = compose(item.data, "default.yaml")
    kept: list[tuple[yaml.Node, yaml.Node]] = []
    if isinstance(node, yaml.MappingNode):
        kept = [(key, value) for key, value in node.value
                if isinstance(key, yaml.ScalarNode) and key.value in PRESET_SECTIONS]

    def text(value: str, style: str | None = None) -> yaml.ScalarNode:
        return yaml.ScalarNode("tag:yaml.org,2002:str", value, style=style)

    schema_list = yaml.SequenceNode("tag:yaml.org,2002:seq", [
        yaml.MappingNode("tag:yaml.org,2002:map", [(text("schema"), text(schema))]) for schema in recommended
    ])
    root = yaml.MappingNode("tag:yaml.org,2002:map", [
        (text("config_version"), text(version, style="'")), (text("schema_list"), schema_list), *kept,
    ])
    item.data = yaml.serialize(root, allow_unicode=True, width=4096).encode("utf-8")
    item.modified.append("schema_list replaced with this package's schemes; unrelated top-level settings dropped")


def manifest_text(entry: dict, version: str, recommended: list[str], items: dict[str, Item], licenses: dict[str, str]) -> str:
    components: dict[tuple[str, str], dict] = {}
    for relative, item in sorted(items.items()):
        key = (item.origin.repo, item.origin.revision)
        component = components.setdefault(key, {
            "source": item.origin.repo, "revision": item.origin.revision,
            "license": item.origin.license or "none declared upstream", "files": [],
        })
        component["files"].append(relative)
    for key, component in components.items():
        if key[0] in licenses:
            component["license_file"] = licenses[key[0]]
    modified = [
        {"file": relative, "change": change}
        for relative, item in sorted(items.items()) for change in item.modified
    ]
    document = {
        "rimes_package": {
            "format": lint.FORMAT_VERSION,
            "id": entry["id"],
            "title": entry["nameZH"],
            "title_en": entry["nameEN"],
            "version": version,
            "category": entry["category"],
            "family": entry.get("family", ""),
            "homepage": "https://github.com/" + entry["repo"],
            "recommended_schemes": recommended,
        },
        "components": list(components.values()),
        "modifications": modified or [],
    }
    if entry.get("sourceArchive"):
        document["rimes_package"]["source_archive"] = entry["sourceArchive"]
    if entry.get("note"):
        document["rimes_package"]["compatibility_note"] = entry["note"]
    header = (
        "# RIMES scheme package manifest. Generated by scripts/input-schemes/scheme_build.py.\n"
        "# Every file keeps the license of the component it is listed under.\n"
    )
    return header + yaml.safe_dump(document, allow_unicode=True, sort_keys=False, width=4096)


def write_zip(path: Path, items: dict[str, bytes]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        for relative in sorted(items):
            data = items[relative]
            info = zipfile.ZipInfo(relative, ZIP_TIMESTAMP)
            info.external_attr = 0o100644 << 16
            info.create_system = 3
            # The importer refuses entries that expand more than 100×.
            packed = len(zlib.compress(data, 9)) if data else 0
            info.compress_type = zipfile.ZIP_STORED if packed * 90 < len(data) or not data else zipfile.ZIP_DEFLATED
            archive.writestr(info, data, compresslevel=9)


def content_digest(items: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    for relative in sorted(items):
        digest.update(relative.encode("utf-8") + b"\0" + hashlib.sha256(items[relative]).digest())
    return digest.hexdigest()


def adapt_wanxiang_portable(items: dict[str, Item]) -> None:
    """Portable Wanxiang 18.1.1 profile using the shipped mobile engine capability set."""
    for module, old, new in (
        ("super_tips", "build/tips", "wanxiang_tips"),
        ("super_replacer", "build/replacer", "wanxiang_replacer"),
    ):
        item = items[f"lua/wanxiang/{module}.lua"]
        before = f'local DB_NAME = "{old}"'.encode()
        if item.data.count(before) != 1:
            raise BuildError(f"Wanxiang {module} database patch no longer matches upstream")
        item.data = item.data.replace(before, f'local DB_NAME = "{new}"'.encode())
        item.modified.append("generated cache database moved to a mobile-compatible resource name inside the isolated user directory")
    item = items["lua/wanxiang/userdb.lua"]
    before = b"    db = UserDb(db_name, db_class)\n"
    if item.data.count(before) != 1:
        raise BuildError("Wanxiang UserDb patch no longer matches upstream")
    item.data = item.data.replace(before, before + b"    if not db then return nil end -- RIMES: do not wrap a failed database allocation\n")
    item.modified.append("return nil when native UserDb allocation fails, preventing recursive __index on a missing _db")

    item = items["lua/wanxiang/super_comment_preedit.lua"]
    before = b'path = "dicts/cuoyin.dict.yaml"'
    if item.data.count(before) != 1 or "dicts_cuoyin.dict.yaml" not in items:
        raise BuildError("Wanxiang correction dictionary patch no longer matches upstream")
    item.data = item.data.replace(before, b'path = "dicts_cuoyin.dict.yaml"')
    item.modified.append("Lua correction dictionary path aligned with the flattened packaged source table")

    item = items["wanxiang.schema.yaml"]
    config = yaml.load(item.data, Loader=lint._Loader)
    config["schema"]["name"] = "万象拼音·灵犀兼容版"
    config["schema"]["version"] = "18.1.1-rimes.1"
    config["schema"]["description"] = (
        "万象 v18.1.1 Base 全拼跨平台基础兼容包，按灵犀 iOS 的运行能力适配。保留词库、普通自动造句、用户调频及可用的 Lua 功能。"
        "不含语法模型、字符集二进制过滤和键盘内修改方案后重部署功能；详见 README-RIMES.txt。"
    )
    config.pop("grammar", None)
    config["translator"]["contextual_suggestions"] = False
    config["engine"]["filters"] = [x for x in config["engine"]["filters"] if x != "lua_filter@*wanxiang.charset_filter"]
    config["engine"]["translators"] = [x for x in config["engine"]["translators"] if x != "lua_translator@*wanxiang.set_schema"]
    config["switches"] = [x for x in config["switches"] if x.get("name") != "charset_filter"]
    config["key_binder"]["bindings"] = [x for x in config["key_binder"]["bindings"] if x.get("toggle") != "charset_filter"]
    config.pop("charset_filter", None)
    item.data = yaml.safe_dump(config, allow_unicode=True, sort_keys=False, width=4096).encode()
    item.modified.append("mobile Base full-pinyin profile: disable unavailable octagram, prebuilt charset filter and desktop redeployment commands; label compatibility limits")
    readme = "README-RIMES.txt"
    items[readme] = Item(
        (CATALOG_DIR / "wanxiang-portable.txt").read_bytes(),
        Origin("scholay/rimes", "generated", readme, "Apache-2.0"),
        ["added mobile import instructions and explicit feature limitations"],
    )


def build_package(entry: dict, sources: dict, output: Path) -> dict:
    cache = output / "cache"
    result: dict = {"id": entry["id"], "status": "failed"}
    base = fetch_release(entry, cache) if entry.get("sourceArchive") else fetch(entry["repo"], entry["revision"], cache)
    root = base / entry.get("root", "")
    if not root.is_dir():
        raise BuildError(f"root {entry.get('root')!r} is not in {entry['repo']}")
    primary = Tree(entry["repo"], entry["revision"], root, base, entry.get("license"))
    available = sorted(path.name[: -len(".schema.yaml")] for path in root.glob("*.schema.yaml"))
    if not available:
        found = sorted({path.parent.relative_to(base).as_posix() for path in base.rglob("*.schema.yaml")})
        raise BuildError("no .schema.yaml in the source root; schemas are in: " + (", ".join(found[:6]) or "nowhere"))
    excluded = set(entry.get("excludeSchemas", []))
    misnamed = []
    for schema in available:
        # A file that does not parse stays in, so the report names the real fault.
        node = compose((root / f"{schema}.schema.yaml").read_bytes(), schema)
        if node is not None and lint.scalar(lint.child(lint.child(node, "schema"), "schema_id")) != schema:
            misnamed.append(schema)
    recommended = entry.get("schemas") or [schema for schema in available if schema not in excluded and schema not in misnamed]
    if not recommended:
        raise BuildError("no scheme file declares the schema_id its name promises")
    resolver = Resolver(sources, cache, primary)
    for overlay in entry.get("overlays", []):
        if not (base / overlay).is_dir():
            raise BuildError(f"overlay {overlay!r} is not in {entry['repo']}")
        resolver.trees.append(Tree(entry["repo"], entry["revision"], base / overlay, base, entry.get("license")))
    assembly = Assembly(resolver)
    assembly.run(recommended)
    if misnamed and not entry.get("schemas"):
        assembly.notes.append("left out, schema_id differs from the file name: " + ", ".join(misnamed))
    for pattern in entry.get("extraFiles", []):
        for path in sorted(root.glob(pattern)):
            if path.is_file():
                assembly.add(path.relative_to(root).as_posix(), path.read_bytes(), primary)
    assembly.rewrite_flattened()
    for relative, item in sorted(assembly.items.items()):
        if item.origin.revision == "generated":
            continue
        try:
            if relative.endswith(".dict.yaml"):
                remove_duplicate_keys(item, relative)
            elif relative.endswith(".yaml"):
                normalize_references(item)
                if relative != "symbols.yaml":
                    remove_duplicate_keys(item, relative)
            elif relative.endswith(".lua"):
                shield_commented_requires(item)
        except UnicodeDecodeError:
            continue  # Left as is; the contract check reports the encoding.
    if entry.get("compatibility") == "wanxiang-portable-v18.1.1":
        adapt_wanxiang_portable(assembly.items)
    shim_lua_submodules(assembly.items)
    split_oversized_dictionaries(assembly.items)
    break_dependency_cycles(assembly.items, recommended)
    version = f"{entry['revisionDate'].replace('-', '')}.{entry['revision'][:7]}"
    if "default.yaml" in assembly.items:
        regenerate_default(assembly.items["default.yaml"], recommended, version)

    licenses: dict[str, str] = {}
    trees = {primary.repo: primary, **{tree.repo: tree for tree in resolver.provider_trees.values()}}
    for repo in sorted({item.origin.repo for item in assembly.items.values()}):
        tree = trees.get(repo)
        found = license_text(tree.base) if tree is not None and not tree.local else None
        # Some release ZIPs omit the license carried by their pinned source commit.
        license_path = entry.get("sourceArchive", {}).get("licenseFile")
        if not found and repo == primary.repo and license_path:
            cached = cache / "archives" / f"{entry['id']}-{entry['revision']}-LICENSE.txt"
            if not cached.exists():
                url = f"https://raw.githubusercontent.com/{repo}/{entry['revision']}/{license_path}"
                with urllib.request.urlopen(url, timeout=60) as response:
                    cached.write_bytes(response.read())
            found = (license_path, cached.read_bytes())
        if found:
            name = "LICENSE-" + re.sub(r"[^A-Za-z0-9]+", "-", repo).strip("-") + ".txt"
            licenses[repo] = name
            assembly.items[name] = Item(found[1], Origin(repo, tree.revision, found[0], tree.license))
    assembly.items[MANIFEST_NAME] = Item(
        manifest_text(entry, version, recommended, assembly.items, licenses).encode("utf-8"),
        Origin("scholay/rimes", "generated", MANIFEST_NAME, "Apache-2.0"),
    )

    files = {relative: item.data for relative, item in assembly.items.items()}
    report = lint.inspect_files(files)
    if not report.fatal:
        try:
            lint.dependency_order(report, [schema for schema in recommended if schema in report.importable])
        except lint.Fatal as error:
            report.fatal = f"staging would fail: {error}"
    archive = output / "packages" / f"rimes-scheme-{entry['id']}-{version}.zip"
    oversize = None
    expanded = sum(len(data) for data in files.values())
    largest = max(files.items(), key=lambda pair: len(pair[1]))
    if len(files) > lint.MAX_ENTRIES:
        oversize = f"{len(files)} files exceed the {lint.MAX_ENTRIES}-entry limit"
    elif len(largest[1]) > lint.MAX_FILE_BYTES:
        oversize = f"{largest[0]} is {len(largest[1]) >> 20} MB; one file may not exceed 32 MB"
    elif expanded > lint.MAX_EXPANDED_BYTES:
        oversize = f"expands to {expanded >> 20} MB; the limit is 128 MB"
    if not oversize:
        write_zip(archive, files)
        if archive.stat().st_size > lint.MAX_ARCHIVE_BYTES:
            oversize = f"archive is {archive.stat().st_size >> 20} MB; the limit is 64 MB"
            archive.unlink()
    if not oversize and not report.fatal:
        archived = lint.inspect_archive(archive)
        if archived.fatal:
            report.fatal = archived.fatal
    if oversize:
        report.fatal = oversize
    usable = report.importable
    result.update({
        "status": "rejected" if report.fatal else ("ready" if set(recommended) <= set(usable) else ("partial" if usable else "blocked")),
        "version": version,
        "recommended": recommended,
        "availableUpstream": available,
        "lint": report.as_dict(),
        "notes": lint._unique(assembly.notes),
        "components": sorted({(item.origin.repo, item.origin.revision, item.origin.license or "") for item in assembly.items.values()}),
        "modifications": sorted({(relative, change) for relative, item in assembly.items.items() for change in item.modified}),
        "fileCount": len(files),
        "expandedBytes": expanded,
        "contentSHA256": content_digest(files),
    })
    if archive.exists():
        result.update({
            "archive": archive.name,
            "bytes": archive.stat().st_size,
            "sha256": sha256(archive.read_bytes()),
        })
    return result


def remove_archives(entry: dict, output: Path) -> None:
    """Drop earlier archives of this package, also when the new build fails."""
    # Match the version exactly: the id of one package can prefix another's.
    earlier = re.compile(rf"rimes-scheme-{re.escape(entry['id'])}-\d{{8}}\.[0-9a-f]{{7}}\.zip")
    for stale in (output / "packages").glob("rimes-scheme-*.zip"):
        if earlier.fullmatch(stale.name):
            stale.unlink()


def build_one(entry: dict, sources: dict, output: Path) -> dict:
    remove_archives(entry, output)
    try:
        result = build_package(entry, sources, output)
    except BuildError as error:
        result = {"id": entry["id"], "status": "failed", "error": str(error)}
    except Exception as error:  # A malformed upstream file must not stop the batch.
        result = {"id": entry["id"], "status": "failed", "error": f"{type(error).__name__}: {error}"}
    report = output / "reports" / f"{entry['id']}.json"
    report.parent.mkdir(parents=True, exist_ok=True)
    if report.exists() and "contentSHA256" in result:
        # Identical content has already been through the importer and the engine.
        earlier = json.loads(report.read_text(encoding="utf-8"))
        if earlier.get("contentSHA256") == result["contentSHA256"] and "verification" in earlier:
            result["verification"] = earlier["verification"]
    report.write_text(json.dumps(result, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    return result


def selected_entries(sources: dict, only: list[str] | None) -> list[dict]:
    entries = sources["packages"]
    if only:
        wanted = set(only)
        entries = [entry for entry in entries if entry["id"] in wanted]
        missing = wanted - {entry["id"] for entry in entries}
        if missing:
            raise SystemExit("unknown package ids: " + ", ".join(sorted(missing)))
    return entries


def command_build(arguments: argparse.Namespace) -> int:
    sources = load_sources()
    entries = selected_entries(sources, arguments.only)
    output = arguments.output.resolve()
    with ThreadPoolExecutor(arguments.jobs) as pool:
        results = list(pool.map(lambda entry: build_one(entry, sources, output), entries))
    counts: dict[str, int] = {}
    for result in results:
        counts[result["status"]] = counts.get(result["status"], 0) + 1
        if result["status"] != "ready" or arguments.verbose:
            reason = result.get("error") or result.get("lint", {}).get("fatal") or ""
            if not reason and result.get("lint"):
                reason = "; ".join(
                    f"{scheme['id']}: {scheme['blocking'][0]}"
                    for scheme in result["lint"]["schemes"] if scheme["blocking"] and scheme["id"] in result["recommended"]
                )
            print(f"{result['status']:9} {result['id']:36} {reason[:150]}")
    print("  ".join(f"{status}={count}" for status, count in sorted(counts.items())))
    return 0


def run(command: list[str], **options) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, **options)


def command_verify(arguments: argparse.Namespace) -> int:
    """Recheck archives locally with the platform-neutral Python contract.

    This checks package data only. Native Linux typing is verified separately
    by the Fcitx5/librime smoke, not by the retired Apple importer harness.
    """
    output = arguments.output.resolve()
    failed = 0
    for entry in selected_entries(load_sources(), arguments.only):
        path = output / "reports" / f"{entry['id']}.json"
        if not path.is_file():
            continue
        result = json.loads(path.read_text(encoding="utf-8"))
        if arguments.missing and "verification" in result:
            continue
        archive_name = result.get("archive")
        if not archive_name:
            continue
        report = lint.inspect_archive(output / "packages" / archive_name)
        passed = not report.fatal and set(result["recommended"]).issubset(report.importable)
        result["verification"] = {"kind": "python-data-contract", "passed": passed,
                                  "data": report.as_dict(), "deploy": None}
        path.write_text(json.dumps(result, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
        failed += not passed
        print(f"{'pass' if passed else 'FAIL'} {entry['id']}: data contract (native typing not checked)")
    return 1 if failed else 0


def verdicts_agree(result: dict) -> bool:
    """Whether the restated contract and the importer itself reached the same verdict."""
    importer = (result.get("verification") or {}).get("importer")
    report = result.get("lint")
    if not importer or not report or "schemes" not in importer and not importer.get("fatal"):
        return True
    blocked = {scheme["id"] for scheme in importer.get("schemes", []) if scheme["blocking"]}
    usable = len(blocked) < len(importer.get("schemes", []))
    rejected = bool(importer.get("fatal") or (importer.get("stageError") and usable))
    if rejected or report.get("fatal"):
        return rejected == bool(report.get("fatal"))
    return blocked == {scheme["id"] for scheme in report.get("schemes", []) if scheme["blocking"]}


CATEGORY_TITLES = {
    "phonetic": "音码 Phonetic", "shape": "形码 Shape", "phonetic-shape": "音形码 Phonetic-shape",
    "dialect": "方言 Dialects and historical Chinese", "auxiliary": "辅助 Auxiliary",
}
INDEX_PATH = CATALOG_DIR / "index.json"
TABLE_PATH = CATALOG_DIR / "CATALOG.md"


def redistribution(entry: dict) -> str:
    if "redistribution" in entry:
        return entry["redistribution"]
    return "open" if entry.get("license") in OPEN_LICENSES else "undeclared"


def summarize(entry: dict, result: dict | None) -> dict:
    """One website-facing record: what the package is, and how far it was checked."""
    record = {
        "id": entry["id"], "nameZH": entry["nameZH"], "nameEN": entry["nameEN"],
        "category": entry["category"], "family": entry.get("family", ""),
        "upstream": {
            "repo": entry["repo"], "url": "https://github.com/" + entry["repo"], "root": entry.get("root", ""),
            "revision": entry["revision"], "revisionDate": entry["revisionDate"],
        },
        "license": entry.get("license"), "redistribution": redistribution(entry),
    }
    if entry.get("note"):
        record["note"] = entry["note"]
    if entry.get("sourceArchive"):
        record["upstream"]["sourceArchive"] = entry["sourceArchive"]
    if result is None:
        record.update({"status": "unbuilt", "statusReason": "not built yet", "publishable": False})
        return record
    verification = result.get("verification")
    lint_report = result.get("lint") or {}
    status, reason = result["status"], result.get("error") or lint_report.get("fatal") or ""
    if status in ("partial", "blocked"):
        reason = "; ".join(
            f"{scheme['id']}: {scheme['blocking'][0]}" for scheme in lint_report.get("schemes", [])
            if scheme["blocking"] and scheme["id"] in result["recommended"]
        )
    if status == "ready" and verification is not None:
        if verification.get("passed"):
            status = "lint-ok" if verification.get("kind") == "python-data-contract" else ("verified" if verification.get("deploy") else "importer-ok")
        else:
            importer = verification.get("importer") or {}
            status = "import-fails"
            reason = (
                verification.get("error") or importer.get("fatal") or importer.get("stageError")
                or "; ".join("deploy: " + failure for failure in (verification.get("deploy") or {}).get("failures", []))
                or "; ".join(f"load {bad['scheme']}: {bad['luaError'] or 'cannot be selected'}" for bad in (verification.get("load") or {}).get("failures", []))
                or "; ".join(f"typing {case['keys']} in {case['schema']} did not offer {case['expect']}" for case in verification.get("probes", []) if not case["passed"])
                or ("deployed data exceeds 512 MB" if (verification.get("deploy") or {}).get("withinDeployedLimit") is False else "")
                or "a recommended scheme is blocked"
            )
    elif status == "ready":
        status = "lint-ok"
    # Lua errors carry the temporary staging path; keep the part inside the package.
    reason = re.sub(r"\S*?/lua/", "lua/", " ".join(str(reason).split()))
    record.update({"status": status, "statusReason": reason[:300]})
    for key in ("version", "archive", "bytes", "sha256", "contentSHA256", "expandedBytes", "fileCount"):
        if key in result:
            record[key] = result[key]
    if lint_report.get("schemes"):
        record["schemes"] = [
            {"id": scheme["id"], "name": scheme["name"], "recommended": scheme["id"] in result["recommended"],
             "importable": not scheme["blocking"] and not lint_report.get("fatal")}
            for scheme in lint_report["schemes"]
        ]
        record["warnings"] = lint_report.get("warnings", [])[1:] + lint_report.get("unsupported", []) + result.get("notes", [])
    if result.get("components"):
        record["components"] = [
            {"source": source, "revision": revision, "license": license or None}
            for source, revision, license in result["components"]
        ]
        # The per-file list is in each package's rimes-package.yaml.
        changes: dict[str, int] = {}
        for _, change in result.get("modifications", []):
            kind = re.sub(r"^(removed) \S+ (from schema/dependencies)", r"\1 a scheme \2", change)
            kind = re.sub(r"^part \d+ of \S+,", "part of a dictionary,", kind)
            kind = re.sub(r"^earlier duplicate of key \S+ removed", "earlier duplicate of a key removed", kind)
            kind = re.sub(r"^split into \d+ files", "split into several files", kind)
            changes[kind] = changes.get(kind, 0) + 1
        record["modifications"] = [{"change": change, "files": count} for change, count in sorted(changes.items())]
    if verification and (verification.get("deploy") or {}).get("seconds") is not None:
        deploy = verification["deploy"]
        seconds, peak = deploy.get("seconds") or 0, deploy.get("peakMemoryMB") or 0
        record["engine"] = {
            "deploySeconds": deploy.get("seconds"), "peakMemoryMB": deploy.get("peakMemoryMB"),
            "deployedMB": deploy.get("deployedMB"),
            # Mac figures, graded against the one package known to deploy on an iPhone 15 Pro
            # Historical memory/deployment limits remain part of the data contract.
            "deviceRisk": "high" if seconds >= 60 or peak >= 2048 else ("medium" if seconds >= 10 or peak >= 1024 else "low"),
            "probes": [{"schema": case["schema"], "keys": case["keys"], "expect": case["expect"], "passed": case["passed"],
                        **({"options": case["options"]} if case.get("options") else {})}
                       for case in verification.get("probes", [])],
        }
    heavy = record.get("engine", {}).get("deviceRisk") == "high"
    record["publishable"] = status == "verified" and record["redistribution"] == "open" and not heavy
    return record


def command_index(arguments: argparse.Namespace) -> int:
    sources = load_sources()
    output = arguments.output.resolve()
    records = []
    for entry in sources["packages"]:
        path = output / "reports" / f"{entry['id']}.json"
        records.append(summarize(entry, json.loads(path.read_text(encoding="utf-8")) if path.exists() else None))
    importers = sorted({
        json.loads(path.read_text(encoding="utf-8")).get("verification", {}).get("importerSHA256", "")
        for path in (output / "reports").glob("*.json")
    } - {""})
    document = {
        "schemaVersion": 1,
        "packageFormat": sources["packageFormat"],
        "comment": "Generated by scripts/input-schemes/scheme_build.py index. Edit sources.json, not this file.",
        # SHA-256 of the RimeSchemeImportService.swift the packages were verified with.
        "verifiedWithImporter": importers,
        "importLimits": {
            "archiveBytes": lint.MAX_ARCHIVE_BYTES, "fileBytes": lint.MAX_FILE_BYTES,
            "expandedBytes": lint.MAX_EXPANDED_BYTES, "entries": lint.MAX_ENTRIES,
            "schemes": lint.MAX_SCHEMAS, "deployedBytes": lint.MAX_DEPLOYED_BYTES,
        },
        "packages": records,
    }
    INDEX_PATH.write_text(json.dumps(document, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    TABLE_PATH.write_text(catalog_table(records), encoding="utf-8")
    counts: dict[str, int] = {}
    for record in records:
        counts[record["status"]] = counts.get(record["status"], 0) + 1
    print("  ".join(f"{status}={count}" for status, count in sorted(counts.items())))
    print(f"publishable={sum(record['publishable'] for record in records)} of {len(records)}")
    if arguments.collect:
        target = arguments.collect.resolve()
        target.mkdir(parents=True, exist_ok=True)
        sums = []
        for record in records:
            if record["publishable"]:
                shutil.copyfile(output / "packages" / record["archive"], target / record["archive"])
                sums.append(f"{record['sha256']}  {record['archive']}")
        (target / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
        print(f"copied {len(sums)} publishable archives to {target}")
    return 0


def catalog_table(records: list[dict]) -> str:
    lines = [
        "# Input scheme catalog",
        "",
        "Generated by `scripts/input-schemes/scheme_build.py index` from `sources.json` and the latest build",
        "reports. Do not edit by hand. Column meanings are explained in [README.md](README.md).",
        "",
    ]
    totals: dict[str, int] = {}
    for record in records:
        totals[record["status"]] = totals.get(record["status"], 0) + 1
    lines.append(f"{len(records)} packages: " + ", ".join(f"{count} {status}" for status, count in sorted(totals.items()))
                 + f"; {sum(record['publishable'] for record in records)} publishable.")
    for category, title in CATEGORY_TITLES.items():
        rows = [record for record in records if record["category"] == category]
        if not rows:
            continue
        lines += ["", f"## {title} ({len(rows)})", "",
                  "| Package | Name | Family | Upstream | License | Schemes | Size | Status |", "|---|---|---|---|---|---:|---:|---|"]
        for record in sorted(rows, key=lambda row: (row["family"], row["id"])):
            schemes = sum(1 for scheme in record.get("schemes", []) if scheme["recommended"])
            size = f"{record['bytes'] / 1048576:.1f} MB" if "bytes" in record else "—"
            license_text = record["license"] or "none"
            if record["redistribution"] != "open":
                license_text += f" ({record['redistribution']})"
            status = record["status"]
            if record["statusReason"] and status not in ("verified", "lint-ok", "importer-ok"):
                status += ": " + record["statusReason"].replace("|", "\\|")[:110]
            name = record["nameZH"].replace("|", "\\|")
            lines.append(
                f"| `{record['id']}` | {name} | {record['family']} | [{record['upstream']['repo']}]({record['upstream']['url']}) "
                f"| {license_text} | {schemes or '—'} | {size} | {status} |"
            )
    return "\n".join(lines) + "\n"


def github_head(repo: str) -> tuple[str, str]:
    """(commit sha, commit date) of the default branch."""
    request = urllib.request.Request(
        f"https://api.github.com/repos/{repo}/commits?per_page=1",
        headers={"User-Agent": "rimes-scheme-build", "Accept": "application/vnd.github+json"},
    )
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    try:
        with urllib.request.urlopen(request, timeout=60) as response:
            commit = json.load(response)[0]
    except Exception as error:
        raise BuildError(f"cannot read the head of {repo}: {error}") from None
    return commit["sha"], commit["commit"]["committer"]["date"][:10]


def command_pin(arguments: argparse.Namespace) -> int:
    sources = load_sources()
    entries = selected_entries(sources, arguments.only)
    heads: dict[str, tuple[str, str]] = {}
    moved = 0
    for entry in entries:
        if entry.get("sourceArchive"):
            print(f"{entry['id']}: release asset is pinned; update its revision, URL and checksum together")
            continue
        if entry["repo"] not in heads:
            heads[entry["repo"]] = github_head(entry["repo"])
        revision, date = heads[entry["repo"]]
        if entry.get("revision") != revision:
            print(f"{entry['id']}: {str(entry.get('revision'))[:7]} -> {revision[:7]} ({date})")
            entry["revision"], entry["revisionDate"] = revision, date
            moved += 1
    SOURCES_PATH.write_text(json.dumps(sources, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")
    print(f"{moved} of {len(entries)} entries moved")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    build = commands.add_parser("build", help="fetch sources, assemble, lint and write packages")
    build.add_argument("--only", nargs="+", metavar="ID")
    build.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    build.add_argument("--jobs", type=int, default=4)
    build.add_argument("--verbose", action="store_true")
    build.set_defaults(run=command_build)
    verify = commands.add_parser("verify", help="verify built archives with the platform-neutral data contract")
    verify.add_argument("--only", nargs="+", metavar="ID")
    verify.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    verify.add_argument("--jobs", type=int, default=3)
    verify.add_argument("--timeout", type=int, default=600, help="seconds allowed for one engine step")
    verify.add_argument("--missing", action="store_true", help="only packages whose current content is not verified yet")
    verify.add_argument("--verbose", action="store_true")
    verify.set_defaults(run=command_verify)
    pin = commands.add_parser("pin", help="move entries to the current head of their upstream default branch")
    pin.add_argument("--only", nargs="+", metavar="ID", required=True)
    pin.set_defaults(run=command_pin)
    index = commands.add_parser("index", help="write Catalog/input-schemes/index.json and CATALOG.md from the reports")
    index.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    index.add_argument("--collect", type=Path, metavar="DIR", help="also copy the publishable archives and a SHA256SUMS file here")
    index.set_defaults(run=command_index)
    arguments = parser.parse_args(argv)
    return arguments.run(arguments)


if __name__ == "__main__":
    sys.exit(main())
