#!/usr/bin/env python3
"""Check a Rime scheme ZIP against the RIMES import contract.

The authority for this contract is the iOS importer,
the historical upstream import contract.  This module restates its
`inspect` rules so packages can be checked on any host before they are
published.  When the two disagree the Swift importer is right and this file
must be corrected.

Findings have three levels, matching what the importer does with them:

- fatal    the importer throws; the whole archive is rejected
- blocking the scheme is listed but cannot be selected
- warning  shown to the user; import proceeds
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
import unicodedata
import zipfile
from dataclasses import dataclass, field
from pathlib import Path

import yaml

# libyaml is the parser the importer itself uses (through Yams).
_Loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)

FORMAT_VERSION = 1

MAX_ARCHIVE_BYTES = 64 * 1024 * 1024
MAX_FILE_BYTES = 32 * 1024 * 1024
MAX_EXPANDED_BYTES = 128 * 1024 * 1024
MAX_ENTRIES = 512
MAX_CONFIG_BYTES = 512 * 1024
MAX_LUA_BYTES = 1024 * 1024
MAX_SCHEMAS = 32
MAX_COMPRESSION_RATIO = 100
MAX_YAML_DEPTH = 64
MAX_YAML_EVENTS = 40_000
MAX_YAML_NODES = 40_000
MAX_DEPLOYED_BYTES = 512 * 1024 * 1024

GLOBAL_FILES = frozenset({
    "default.custom.yaml", "installation.yaml", "user.yaml",
    "hamster.yaml", "hamster.custom.yaml", "weasel.yaml", "weasel.custom.yaml",
    "squirrel.yaml", "squirrel.custom.yaml", "trime.yaml", "trime.custom.yaml",
    "ibus_rime.yaml", "ibus_rime.custom.yaml",
})
# default.yaml sections the importer carries into the isolated deployment.
DEFAULT_SECTIONS = ("key_binder", "ascii_composer", "recognizer", "punctuator", "menu", "speller")
ALLOWED_TAGS = frozenset(
    "tag:yaml.org,2002:" + name
    for name in ("str", "map", "seq", "bool", "int", "float", "null", "value", "merge", "timestamp")
)
ID_CHARS = frozenset("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")
LUA_MODULE_CHARS = ID_CHARS | frozenset("/")
LUA_STANDARD_MODULES = frozenset({"utf8", "math", "string", "table", "coroutine"})
REQUIRE_PATTERNS = (
    re.compile(r"""\brequire\s*(?:\(\s*)?["']([^"']+)["']"""),
    re.compile(r"""\bpcall\s*\(\s*require\s*,\s*["']([^"']+)["']"""),
)
DESKTOP_ONLY_LUA = ("os.execute", "io.popen", "package.loadlib")


class Fatal(Exception):
    """The importer would reject the whole archive."""


@dataclass
class Scheme:
    id: str
    name: str
    dependencies: list[str]
    warnings: list[str] = field(default_factory=list)
    blocking: list[str] = field(default_factory=list)
    recommended: bool = False


@dataclass
class Report:
    schemes: list[Scheme] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    unsupported: list[str] = field(default_factory=list)
    fatal: str | None = None
    file_count: int = 0
    expanded_bytes: int = 0
    permitted: list[str] = field(default_factory=list)
    skipped: list[str] = field(default_factory=list)
    prefix: str = ""

    @property
    def importable(self) -> list[str]:
        if self.fatal:
            return []
        return [scheme.id for scheme in self.schemes if not scheme.blocking]

    def as_dict(self) -> dict:
        return {
            "fatal": self.fatal,
            "schemes": [
                {
                    "id": s.id, "name": s.name, "dependencies": s.dependencies,
                    "recommended": s.recommended, "blocking": s.blocking, "warnings": s.warnings,
                }
                for s in self.schemes
            ],
            "warnings": self.warnings,
            "unsupported": self.unsupported,
            "fileCount": self.file_count,
            "expandedBytes": self.expanded_bytes,
            "skipped": self.skipped,
        }


def valid_id(value: str) -> bool:
    return bool(value) and len(value) <= 100 and ".." not in value and all(c in ID_CHARS for c in value)


def check_relative_path(path: str) -> None:
    parts = path.split("/")
    if (
        not path or len(path.encode("utf-8")) > 1024 or path.startswith("/") or "\\" in path
        or ":" in path or ".." in parts or "." in parts or "" in parts[:-1]
        or any(unicodedata.category(c) == "Cc" for c in path)
    ):
        raise Fatal(f"unsafe resource path: {path!r}")


def allowed_resource(path: str) -> bool:
    lower = path.lower()
    if (
        lower in GLOBAL_FILES or lower.startswith(("build/", "sync/", "__macosx/"))
        or ".userdb" in lower
    ):
        return False
    if "/" not in path:
        return lower.endswith((".yaml", ".txt")) or lower == "rime.lua"
    if lower.startswith("lua/"):
        return lower.endswith((".lua", ".txt", ".json"))
    if lower.startswith("opencc/"):
        return lower.endswith((".json", ".txt", ".ocd", ".ocd2", ".lua"))
    return False


def lua_path(module: str) -> str:
    if (
        not module or len(module) > 180 or ".." in module
        or not all(c in LUA_MODULE_CHARS for c in module)
        or module.startswith("/") or module.endswith("/")
    ):
        raise Fatal(f"illegal Lua module reference: {module!r}")
    return "lua/" + module.replace(".", "/") + ".lua"


def lua_requirements(text: str) -> list[tuple[str, bool]]:
    """(module, optional) for each `require` the importer sees, line by line.
    A requirement is optional when its line mentions pcall or names a standard library."""
    found = []
    for line in text.split("\n"):
        active = line.strip(" \t\u00a0\u3000")
        if not active or active.startswith("--"):
            continue
        for pattern in REQUIRE_PATTERNS:
            for module in pattern.findall(line):
                found.append((module, "pcall" in active or module in LUA_STANDARD_MODULES))
    return found


def desktop_only_lua(text: str) -> bool:
    return any(
        token in line for line in text.split("\n")
        if not line.strip(" \t\u00a0\u3000").startswith("--") for token in DESKTOP_ONLY_LUA
    )


def _forbidden_yaml_char(code: int) -> bool:
    return (
        (code < 0x20 and code not in (0x09, 0x0A, 0x0D)) or 0x7F <= code <= 0x84
        or 0x86 <= code <= 0x9F or code in (0xFFFE, 0xFFFF)
    )


def _check_events(text: str, path: str) -> None:
    depth = events = documents = 0
    try:
        for event in yaml.parse(text, Loader=_Loader):
            events += 1
            if isinstance(event, (yaml.MappingStartEvent, yaml.SequenceStartEvent)):
                depth += 1
            elif isinstance(event, (yaml.MappingEndEvent, yaml.SequenceEndEvent)):
                depth -= 1
            elif isinstance(event, yaml.DocumentStartEvent):
                documents += 1
            if depth > MAX_YAML_DEPTH or events > MAX_YAML_EVENTS or documents > 1:
                raise Fatal(f"{path}: YAML exceeds depth, node or document limits")
    except yaml.YAMLError as error:
        raise Fatal(f"{path}: malformed YAML ({_first_line(error)})") from None


def _first_line(error: Exception) -> str:
    return " ".join(str(error).split())[:160]


def _validate_node(node: yaml.Node, path: str, allow_duplicate_keys: bool = False) -> None:
    budget = MAX_YAML_NODES

    def visit(current: yaml.Node, depth: int) -> None:
        nonlocal budget
        budget -= 1
        if budget < 0 or depth > MAX_YAML_DEPTH:
            raise Fatal(f"{path}: YAML alias expansion or nesting exceeds limits")
        if current.tag not in ALLOWED_TAGS:
            raise Fatal(f"{path}: unsupported YAML tag {current.tag}")
        if isinstance(current, yaml.SequenceNode):
            for value in current.value:
                visit(value, depth + 1)
        elif isinstance(current, yaml.MappingNode):
            keys: set[str] = set()
            for key, value in current.value:
                if not isinstance(key, yaml.ScalarNode):
                    raise Fatal(f"{path}: YAML has a non-scalar key")
                if key.value in keys and not allow_duplicate_keys:
                    raise Fatal(f"{path}: duplicate YAML key {key.value!r}")
                keys.add(key.value)
                visit(key, depth + 1)
                visit(value, depth + 1)

    visit(node, 0)


def compose_config(data: bytes, path: str, allow_duplicate_keys: bool = False) -> yaml.Node:
    if len(data) > MAX_CONFIG_BYTES:
        raise Fatal(f"{path}: configuration exceeds 512 KB")
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise Fatal(f"{path}: not UTF-8") from None
    _check_events(text, path)
    try:
        node = yaml.compose(text, Loader=_Loader)
    except yaml.YAMLError as error:
        raise Fatal(f"{path}: YAML cannot be parsed ({_first_line(error)})") from None
    if node is None:
        raise Fatal(f"{path}: YAML cannot be parsed (empty document)")
    _validate_node(node, path, allow_duplicate_keys)
    return node


def normalize_config(data: bytes, path: str) -> tuple[bytes, str | None]:
    """Mirror the importer's disclosed repairs; return (data, warning)."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        raise Fatal(f"{path}: not UTF-8") from None
    warnings = []
    bad = [c for c in text if _forbidden_yaml_char(ord(c))]
    if bad:
        codes = ", ".join(sorted({f"U+{ord(c):04X}" for c in bad}))
        text = "".join(c for c in text if not _forbidden_yaml_char(ord(c)))
        data = text.encode("utf-8")
        warnings.append(f"{len(bad)} control characters forbidden by YAML ({codes}) are removed in the deployed copy")
    if path == "symbols.yaml":
        root = compose_config(data, path, allow_duplicate_keys=True)
        duplicates = sorted(_duplicate_keys(root, ""))
        if duplicates:
            warnings.append("duplicate symbol keys keep their last value: " + ", ".join(duplicates[:8]))
            data = _serialize_last_wins(root).encode("utf-8")
    return data, (f"{path}: " + "; ".join(warnings)) if warnings else None


def _duplicate_keys(node: yaml.Node, path: str) -> set[str]:
    found: set[str] = set()
    if isinstance(node, yaml.SequenceNode):
        for value in node.value:
            found |= _duplicate_keys(value, path)
    elif isinstance(node, yaml.MappingNode):
        seen: set[str] = set()
        for key, value in node.value:
            location = key.value if not path else f"{path}/{key.value}"
            if key.value in seen:
                found.add(location)
            seen.add(key.value)
            found |= _duplicate_keys(value, location)
    return found


def _serialize_last_wins(node: yaml.Node) -> str:
    def plain(current: yaml.Node):
        if isinstance(current, yaml.SequenceNode):
            return [plain(value) for value in current.value]
        if isinstance(current, yaml.MappingNode):
            return {key.value: plain(value) for key, value in current.value}
        return current.value

    return yaml.safe_dump(plain(node), allow_unicode=True, sort_keys=False)


def scalar_fields(root: yaml.Node) -> list[tuple[str, str]]:
    """(immediate mapping key, scalar value) pairs, sequences inheriting their key."""
    result: list[tuple[str, str]] = []

    def walk(node: yaml.Node, key: str) -> None:
        if isinstance(node, yaml.ScalarNode):
            result.append((key, node.value))
        elif isinstance(node, yaml.SequenceNode):
            for value in node.value:
                walk(value, key)
        elif isinstance(node, yaml.MappingNode):
            for child_key, value in node.value:
                walk(value, child_key.value if isinstance(child_key, yaml.ScalarNode) else "")

    walk(root, "")
    return result


def child(node: yaml.Node | None, key: str) -> yaml.Node | None:
    if not isinstance(node, yaml.MappingNode):
        return None
    for candidate, value in node.value:
        if isinstance(candidate, yaml.ScalarNode) and candidate.value == key:
            return value
    return None


def scalar(node: yaml.Node | None) -> str | None:
    return node.value if isinstance(node, yaml.ScalarNode) else None


def string_list(node: yaml.Node | None) -> list[str]:
    if not isinstance(node, yaml.SequenceNode):
        return []
    return [value.value for value in node.value if isinstance(value, yaml.ScalarNode)]


def dictionary_header(data: bytes, path: str) -> bytes:
    """The YAML front matter of a .dict.yaml, as the importer reads it."""
    window = data[: MAX_CONFIG_BYTES + 16_384]
    index = window.find(b"\n...")
    if index >= 0 and len(window) > index + 4 and window[index + 4] in (9, 10, 13, 32):
        return window[:index]
    if len(data) > MAX_CONFIG_BYTES:
        raise Fatal(f"{path}: dictionary header exceeds 512 KB or has no YAML end marker")
    if 9 in data:
        raise Fatal(f"{path}: dictionary lacks the YAML end marker '...'")
    return data


def compose_dictionary_header(data: bytes, path: str) -> yaml.Node:
    header = dictionary_header(data, path).decode("utf-8", errors="replace")
    text = "\n".join("" if not line.strip() else line for line in header.split("\n"))
    return compose_config(text.encode("utf-8"), path)


def check_reference(value: str, paths: set[str], where: str) -> None:
    if value.startswith("/") or ".." in value or "\\" in value:
        raise Fatal(f"{where}: configuration reference leaves the scheme directory: {value!r}")
    if ":" in value:
        name = value.split(":", 1)[0]
        if not valid_id(name) or name + ".yaml" not in paths:
            raise Fatal(f"{where}: referenced configuration {name}.yaml is not in the package")


def _json_file_references(value) -> list[str]:
    if isinstance(value, dict):
        found = []
        for key, item in value.items():
            found += [item if isinstance(item, str) else ""] if key == "file" else _json_file_references(item)
        return found
    if isinstance(value, list):
        return [reference for item in value for reference in _json_file_references(item)]
    return []


def _unique(values: list[str]) -> list[str]:
    seen: set[str] = set()
    return [value for value in values if not (value in seen or seen.add(value))]


def inspect_files(files: dict[str, bytes], all_paths: list[str] | None = None) -> Report:
    """Inspect a path → bytes map laid out exactly as the archive would be."""
    report = Report()
    try:
        _inspect(files, all_paths if all_paths is not None else sorted(files), report)
    except Fatal as error:
        report.fatal = str(error)
    return report


def _inspect(files: dict[str, bytes], all_paths: list[str], report: Report) -> None:
    schema_paths = [path for path in all_paths if path.endswith(".schema.yaml")]
    if not 1 <= len(schema_paths) <= MAX_SCHEMAS:
        raise Fatal(f"archive must hold 1–{MAX_SCHEMAS} .schema.yaml files, found {len(schema_paths)}")
    parents = {path.rsplit("/", 1)[0] if "/" in path else "" for path in schema_paths}
    if len(parents) != 1:
        raise Fatal("schemas are spread over several directories: " + ", ".join(sorted(parents)))
    parent = next(iter(parents))
    prefix = parent + "/" if parent else ""
    report.prefix = prefix
    entries: dict[str, bytes] = {}
    for path in all_paths:
        if path.startswith(prefix) and allowed_resource(path[len(prefix):]):
            entries[path[len(prefix):]] = files[path]
        else:
            report.skipped.append(path)
    paths = set(entries)
    report.permitted = sorted(paths)
    report.file_count = len(all_paths)
    report.expanded_bytes = sum(len(files[path]) for path in all_paths)

    schemas: dict[str, yaml.Node] = {}
    configs: dict[str, yaml.Node] = {}
    headers: dict[str, yaml.Node] = {}
    warnings = ["global default.custom.yaml, installation state and other frontends' settings are never imported"]
    for path in sorted(paths):
        if not path.endswith(".yaml") or path.endswith(".dict.yaml"):
            continue
        data = entries[path]
        if len(data) > MAX_CONFIG_BYTES:
            raise Fatal(f"{path}: configuration exceeds 512 KB")
        normalized, warning = normalize_config(data, path)
        if warning:
            warnings.append(warning)
        node = compose_config(normalized, path)
        configs[path] = node
        if path.endswith(".schema.yaml"):
            schema_id = scalar(child(child(node, "schema"), "schema_id"))
            if not schema_id or not valid_id(schema_id) or path != schema_id + ".schema.yaml":
                raise Fatal(f"{path}: schema_id is missing, illegal or differs from the file name")
            schemas[schema_id] = node
    for path in sorted(paths):
        if not path.endswith(".dict.yaml"):
            continue
        node = compose_dictionary_header(entries[path], path)
        name = scalar(child(node, "name"))
        if not name or not valid_id(name):
            raise Fatal(f"{path}: dictionary name is missing or illegal")
        file_id = path[: -len(".dict.yaml")]
        if file_id != name:
            warnings.append(f"{path}: internal name is {name}; references are checked by file name")
        headers[file_id] = node

    for path, node in sorted(configs.items()):
        for key, value in scalar_fields(node):
            if not value:
                continue
            leaf = ([part for part in key.split("/") if part] or [key])[-1]
            if leaf in ("dictionary", "prism", "user_dict", "import_preset") and not valid_id(value):
                raise Fatal(f"{path}: illegal {leaf} reference {value!r}")
            if leaf == "opencc_config":
                check_relative_path(value)
            if leaf in ("__include", "__patch"):
                check_reference(value, paths, path)

    preferred = set()
    schema_list = child(configs.get("default.yaml"), "schema_list")
    if isinstance(schema_list, yaml.SequenceNode):
        preferred = {scalar(child(item, "schema")) for item in schema_list.value}

    for schema_id in sorted(schemas):
        node = schemas[schema_id]
        name = scalar(child(child(node, "schema"), "name")) or schema_id
        if not name or len(name) > 120:
            raise Fatal(f"{schema_id}: scheme name must be 1–120 characters")
        dependencies = string_list(child(child(node, "schema"), "dependencies"))
        issues: list[str] = []
        notes: list[str] = []
        for dependency in dependencies:
            if not valid_id(dependency) or dependency not in schemas:
                issues.append(f"missing dependency scheme {dependency}.schema.yaml")
        patch = configs.get(schema_id + ".custom.yaml")
        overridden = scalar(child(child(patch, "patch"), "schema/schema_id"))
        if overridden is not None and overridden != schema_id:
            issues.append("custom patch may not change schema_id")
        for inspected in (node, patch):
            if inspected is None:
                continue
            main_dictionary = scalar(child(child(inspected, "translator"), "dictionary"))
            for key, value in scalar_fields(inspected):
                if key == "dictionary" or key.endswith("/dictionary"):
                    if not value:
                        continue
                    if not valid_id(value):
                        raise Fatal(f"{schema_id}: illegal dictionary reference {value!r}")
                    if value not in headers:
                        if key.startswith("translator/") or main_dictionary == value:
                            issues.append(f"missing main dictionary {value}.dict.yaml")
                        else:
                            notes.append(f"auxiliary dictionary {value} is not in the package; verify the lookup that uses it")
                    else:
                        _check_dictionary(value, headers, issues)
                if key == "import_preset" or key.endswith("/import_preset"):
                    if not valid_id(value):
                        raise Fatal(f"{schema_id}: illegal preset reference {value!r}")
                    if value != "default" and value + ".yaml" not in paths:
                        issues.append(f"missing preset {value}.yaml")
                if key in ("prism", "user_dict") or key.endswith(("/prism", "/user_dict")):
                    if value and not valid_id(value):
                        raise Fatal(f"{schema_id}: illegal prism or user dictionary id {value!r}")
                if key == "opencc_config" or key.endswith("/opencc_config"):
                    if value:
                        check_relative_path(value)
                if key in ("__include", "__patch") or key.endswith(("/__include", "/__patch")):
                    check_reference(value, paths, schema_id)
                if value.startswith("lua_") and "@" in value:
                    parts = value.split("@")
                    if len(parts) > 1 and parts[1].startswith("*"):
                        module_path = lua_path(parts[1][1:])
                        if module_path not in paths:
                            issues.append(f"missing Lua component {module_path}")
                    elif "rime.lua" not in paths:
                        issues.append(f"legacy Lua component {value} needs rime.lua in the package")
        report.schemes.append(Scheme(
            id=schema_id, name=name, dependencies=dependencies, warnings=_unique(notes),
            blocking=_unique(issues), recommended=schema_id in preferred,
        ))

    unsupported: list[str] = []
    for path in sorted(paths):
        if not path.endswith(".lua"):
            continue
        data = entries[path]
        if len(data) > MAX_LUA_BYTES:
            raise Fatal(f"{path}: Lua file exceeds 1 MB")
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            raise Fatal(f"{path}: Lua file is not UTF-8") from None
        for module, optional in lua_requirements(text):
            dependency = lua_path(module)
            if dependency in paths:
                continue
            if optional:
                warnings.append(f"{path}: optional module {module} is not in the package")
            else:
                raise Fatal(f"{path}: required Lua module is missing: {dependency}")
        if desktop_only_lua(text):
            unsupported.append(f"{path}: launches desktop programs or shell commands; unavailable in the iOS keyboard")
    for path in sorted(paths):
        if not (path.startswith("opencc/") and path.endswith(".json")):
            continue
        data = entries[path]
        if len(data) > MAX_CONFIG_BYTES:
            raise Fatal(f"{path}: configuration exceeds the read limit")
        try:
            document = json.loads(data)
        except (ValueError, UnicodeDecodeError):
            raise Fatal(f"{path}: invalid OpenCC configuration") from None
        for reference in _json_file_references(document):
            local = "/".join(part for part in reference.split("/") if part != ".")
            check_relative_path(local)
            resolved = (path.rsplit("/", 1)[0] + "/" + local) if "/" in path else local
            if resolved not in paths:
                warnings.append(f"{path}: references {reference}, which is not in the package")
    if report.skipped:
        warnings.append(f"{len(report.skipped)} files outside the import allowlist are not deployed")
    if any(path.endswith(".lua") for path in paths):
        warnings.append("scheme has Lua input logic; it runs in the restricted runtime")
    report.warnings = _unique(warnings)
    report.unsupported = _unique(unsupported)


def _check_dictionary(dictionary: str, headers: dict[str, yaml.Node], issues: list[str]) -> None:
    visited: set[str] = set()

    def visit(current: str, stack: tuple[str, ...]) -> None:
        if current in stack or len(stack) > 32:
            raise Fatal(f"dictionary imports are cyclic or too deep at {current}")
        if current in visited:
            return
        visited.add(current)
        header = headers.get(current)
        if header is None:
            issues.append(f"missing dictionary {current}.dict.yaml")
            return
        for dependency in string_list(child(header, "import_tables")):
            if not valid_id(dependency):
                raise Fatal(f"{current}.dict.yaml: illegal import_tables entry {dependency!r}")
            visit(dependency, stack + (current,))

    visit(dictionary, ())


def dependency_order(report: Report, selected: list[str]) -> list[str]:
    """Deployment order the importer derives for the selected schemes."""
    by_id = {scheme.id: scheme for scheme in report.schemes}
    order: list[str] = []
    visited: set[str] = set()

    def visit(scheme_id: str, ancestors: frozenset[str]) -> None:
        if scheme_id in ancestors or len(ancestors) >= 32:
            raise Fatal(f"scheme dependencies are cyclic or deeper than 32 at {scheme_id}")
        if scheme_id in visited:
            return
        scheme = by_id.get(scheme_id)
        if scheme is None:
            raise Fatal(f"missing dependency scheme {scheme_id}")
        if scheme.blocking:
            raise Fatal(f"dependency scheme {scheme_id}: " + "; ".join(scheme.blocking))
        for dependency in scheme.dependencies:
            visit(dependency, ancestors | {scheme_id})
        visited.add(scheme_id)
        order.append(scheme_id)

    for scheme_id in selected:
        visit(scheme_id, frozenset())
    return order


def check_archive_structure(data: bytes) -> None:
    """Central-directory checks the importer performs before opening the ZIP."""
    if not 22 <= len(data) <= MAX_ARCHIVE_BYTES:
        raise Fatal("ZIP is empty, damaged or larger than 64 MB")
    end = None
    for offset in range(len(data) - 22, max(0, len(data) - 65_557) - 1, -1):
        if data[offset:offset + 4] == b"PK\x05\x06":
            comment = struct.unpack_from("<H", data, offset + 20)[0]
            if offset + 22 + comment == len(data):
                end = offset
                break
    if end is None:
        raise Fatal("ZIP end-of-directory record not found")
    disk, cd_disk, here, count, length, start = struct.unpack_from("<HHHHII", data, end + 4)
    if (
        disk or cd_disk or here != count or count > MAX_ENTRIES
        or start == 0xFFFFFFFF or length == 0xFFFFFFFF or start + length != end
    ):
        raise Fatal("multi-volume, ZIP64, over-512-entry or irregular ZIP directories are not supported")
    cursor = start
    for _ in range(count):
        if cursor + 46 > end or data[cursor:cursor + 4] != b"PK\x01\x02":
            raise Fatal("ZIP entry is damaged")
        if struct.unpack_from("<H", data, cursor + 8)[0] & 1:
            raise Fatal("ZIP entry is encrypted")
        name, extra, comment = struct.unpack_from("<HHH", data, cursor + 28)
        cursor += 46 + name + extra + comment
    if cursor != end:
        raise Fatal("ZIP directory count is inconsistent")


def inspect_archive(path: Path) -> Report:
    report = Report()
    try:
        data = path.read_bytes()
        check_archive_structure(data)
        files: dict[str, bytes] = {}
        seen: set[str] = set()
        total = 0
        with zipfile.ZipFile(path) as archive:
            infos = archive.infolist()
            if len(infos) > MAX_ENTRIES:
                raise Fatal("ZIP has more than 512 entries")
            for info in infos:
                check_relative_path(info.filename)
                key = unicodedata.normalize("NFC", info.filename.strip("/")).lower()
                if key in seen:
                    raise Fatal(f"duplicate ZIP path: {info.filename}")
                seen.add(key)
                if (info.external_attr >> 16) & 0o170000 == 0o120000:
                    raise Fatal(f"ZIP contains a symbolic link: {info.filename}")
                if (
                    info.file_size > MAX_FILE_BYTES or total + info.file_size > MAX_EXPANDED_BYTES
                    or info.compress_size > len(data)
                    or info.file_size > max(1, info.compress_size) * MAX_COMPRESSION_RATIO
                ):
                    raise Fatal(f"ZIP exceeds expanded-size or compression-ratio limits at {info.filename}")
                total += info.file_size
                if not info.is_dir():
                    files[info.filename] = archive.read(info)
        report = inspect_files(files)
        report.file_count = len(files)
        report.expanded_bytes = total
    except Fatal as error:
        report.fatal = str(error)
    except zipfile.BadZipFile as error:
        report.fatal = f"unreadable ZIP: {error}"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("archive", type=Path, nargs="+")
    parser.add_argument("--json", action="store_true", help="print machine-readable reports")
    arguments = parser.parse_args(argv)
    failed = False
    for archive in arguments.archive:
        report = inspect_archive(archive)
        usable = report.importable
        failed |= not usable
        if arguments.json:
            payload = report.as_dict()
            payload["archive"] = archive.name
            payload["sha256"] = hashlib.sha256(archive.read_bytes()).hexdigest()
            print(json.dumps(payload, ensure_ascii=False))
            continue
        print(f"{archive.name}: " + (f"REJECTED — {report.fatal}" if report.fatal else f"{len(usable)}/{len(report.schemes)} schemes importable"))
        for scheme in report.schemes:
            mark = "ok " if not scheme.blocking else "BLOCKED"
            print(f"  [{mark}] {scheme.id}  {scheme.name}" + ("  (recommended)" if scheme.recommended else ""))
            for issue in scheme.blocking:
                print(f"        blocking: {issue}")
            for note in scheme.warnings:
                print(f"        note: {note}")
        for warning in report.warnings + report.unsupported:
            print(f"  warning: {warning}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
