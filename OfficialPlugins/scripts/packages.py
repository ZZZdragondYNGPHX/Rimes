#!/usr/bin/env python3
# Copyright 2026 scholay
# SPDX-License-Identifier: Apache-2.0
"""Validate and build immutable, deterministic official plugin packages."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)")
IDENTIFIER = re.compile(r"[a-z][a-z0-9]*(?:[.-][a-z0-9]+)+")
PLATFORMS = {"macos", "ios", "android", "windows"}
MAX_PACKAGE_BYTES = 256 * 1024


def canonical(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n").encode("utf-8")


def require(ok: bool, message: str) -> None:
    if not ok:
        raise ValueError(message)


def version(value: object) -> tuple[int, int, int]:
    require(isinstance(value, str) and VERSION.fullmatch(value) is not None, "invalid semantic version")
    return tuple(map(int, value.split(".")))


def text(value: object, maximum: int = 4096) -> bool:
    return isinstance(value, str) and bool(value.strip()) and "\0" not in value and len(value.encode("utf-8")) <= maximum


def safe_path(root: Path, value: str) -> Path:
    require(isinstance(value, str) and bool(value) and "\\" not in value, "invalid source path")
    parts = PurePosixPath(value)
    require(not parts.is_absolute() and all(p not in (".", "..", "") for p in value.split("/")), "unsafe source path")
    current = root
    for part in parts.parts:
        current /= part
        require(not current.is_symlink(), "source path must not contain symlinks")
    require(current.resolve().is_relative_to(root.resolve()), "source path escapes repository")
    return current


def validate(manifest: dict) -> None:
    require(isinstance(manifest, dict), "manifest must be an object")
    require(manifest.get("schemaVersion") == 2, "unsupported package schema")
    require(manifest.get("sdkVersion") == 1, "unsupported host SDK")
    require(manifest.get("runtime") == "host-interpreted", "unsupported runtime")
    require(isinstance(manifest.get("id"), str) and IDENTIFIER.fullmatch(manifest["id"]) is not None, "invalid plugin ID")
    version(manifest.get("version"))
    version(manifest.get("minimumHostVersion"))
    for key in ("nameZH", "nameEN", "summaryZH", "summaryEN", "notice"):
        require(text(manifest.get(key)), f"invalid {key}")
    require(manifest.get("license") == "Apache-2.0", "missing package license")
    platforms = manifest.get("platforms")
    require(isinstance(platforms, dict) and bool(platforms) and set(platforms) <= PLATFORMS, "invalid platforms")
    for platform, entry in platforms.items():
        require(isinstance(entry, dict) and entry.get("distribution") in ("bundled", "download"), f"invalid distribution: {platform}")
        require(text(entry.get("legacyID"), 128), f"missing legacy identity: {platform}")
    contribution = manifest.get("contribution")
    require(isinstance(contribution, dict), "invalid contribution")
    kind, capabilities, keys, option_keys = {
        "ai.prompt.v1": ("buffer", ["buffer.read", "ai.generate"], {"type", "instructions"}, set()),
        "ai.channel.v1": ("buffer", ["buffer.read", "ai.generate"], {"type", "options"}, {"channel"}),
        "translation.v1": ("buffer", ["buffer.read", "translation.generate"], {"type", "options", "instructions"}, {"source", "target"}),
        "stream.pinyin.v1": ("buffer", ["buffer.read", "ai.generate"], {"type", "options"}, {"maxCandidates"}),
        "reference.search.v1": ("buffer", ["buffer.read", "ai.generate", "reference.search"], {"type", "options"}, {"citationStyle"}),
        "music.keyboard.v1": ("buffer", ["audio.play"], {"type", "options"}, {"layout", "tracks"}),
        "morse.v1": ("buffer", ["buffer.write", "audio.play"], {"type", "options"}, {"alphabet", "key"}),
        "host.module.v1": (manifest.get("kind"), [str(manifest.get("kind")) + ".navigate"], {"type", "options"}, {"host", "module", "filters", "actions"}),
        "metrics.v1": ("extension", ["metrics.aggregate"], {"type", "options"}, {"events", "storage"}),
        "typing.practice.v1": ("extension", ["metrics.aggregate"], {"type", "options"}, {"source", "storage"}),
        "input.chord.v1": ("extension", ["input.chord"], {"type", "options"}, {"schema", "keymap"}),
    }.get(contribution.get("type"), (None, None, None, None))
    require(kind is not None and manifest.get("kind") == kind, "unsupported plugin kind or interpreter")
    require(manifest.get("capabilities") == capabilities, "unsupported capabilities")
    require(set(contribution) == keys, "invalid contribution")
    if "instructions" in contribution:
        instructions = contribution["instructions"]
        require(isinstance(instructions, dict) and "default" in instructions and set(instructions) <= {"default", "image"}, "invalid prompt modes")
        require(all(text(v, 32768) for v in instructions.values()), "invalid prompt contents")
    if "options" in contribution:
        options = contribution["options"]
        require(isinstance(options, dict) and set(options) == option_keys and all(text(v, 256) for v in options.values()), "invalid interpreter options")
        if contribution["type"] == "host.module.v1":
            require(kind in {"capsule", "mailbox"} and options["host"] == kind, "invalid host module")
            modules = {"capsule": {"temporary", "capture", "notes", "resources", "passwords"}, "mailbox": {"terminal", "chat", "inbox"}}
            require(options["module"] in modules[kind], "unknown host module")
            require(set(options["actions"].split(",")) <= {"open", "search"}, "unknown host action")
            require(set(options["filters"].split(",")) <= {"all", "text", "image", "video", "bullet", "richText", "reference", "document", "project", "skills", "login", "key", "other", "unread", "attention", "archived"}, "unknown host filter")
    require(len(canonical(manifest)) <= MAX_PACKAGE_BYTES, "package too large")


def manifests(root: Path) -> list[dict]:
    result = []
    seen = set()
    for path in sorted((root / "plugins").glob("*/plugin.json")):
        safe_path(root, path.relative_to(root).as_posix())
        data = path.read_bytes()
        require(len(data) <= MAX_PACKAGE_BYTES, "package too large")
        item = json.loads(data)
        validate(item)
        require(item["id"] not in seen, "duplicate plugin ID")
        seen.add(item["id"])
        result.append(item)
    require(bool(result), "no plugin packages found")
    return result


def native_sources(root: Path, plugin_ids: set[str]) -> tuple[dict, dict[str, bytes]]:
    source_map = json.loads((root / "source-map.json").read_bytes())
    require(source_map.get("schemaVersion") == 1, "invalid source map")
    destinations = set()
    files = {}
    records = []
    for entry in source_map["files"]:
        source, destination = entry["source"], entry["destination"]
        require(source.startswith("native/"), "native sources must live under native/")
        require(destination.startswith(("Sources/", "Shared/Sources/", "platforms/")), "invalid host destination")
        safe_path(root, destination)
        require(destination not in destinations, "duplicate host destination")
        require(bool(entry["plugins"]) and set(entry["plugins"]) <= plugin_ids, "source references unknown plugin")
        path = safe_path(root, source)
        require(path.is_file(), f"missing native source: {source}")
        data = path.read_bytes()
        files[source] = data
        records.append({**entry, "sha256": hashlib.sha256(data).hexdigest()})
        destinations.add(destination)
    result = {**source_map, "files": records}
    for name in ("LICENSE", "NOTICE", "LICENSES/MIT-legacy.txt", "THIRD_PARTY_NOTICES.md"):
        files[name] = safe_path(root, name).read_bytes()
    files["source-map.json"] = canonical(result)
    return result, files


def build(root: Path, output: Path, release_version: str) -> dict:
    version(release_version)
    packages = manifests(root)
    _, sources = native_sources(root, {p["id"] for p in packages})
    output.mkdir(parents=True, exist_ok=True)
    artifacts = {}
    entries = []
    for package in packages:
        package = {**package, "licenseText": (root / "LICENSE").read_text(),
                   "notice": (root / "NOTICE").read_text()}
        validate(package)
        asset = f"preset-plugin-{package['id']}-{package['version']}.json"
        data = canonical(package)
        (output / asset).write_bytes(data)
        digest = hashlib.sha256(data).hexdigest()
        artifacts[asset] = digest
        entries.append({**package, "sha256": digest, "downloadAssetName": asset,
                        "downloadURL": f"https://github.com/scholay/rimes-plugins/releases/download/v{release_version}/{asset}"})
    catalog = {"schemaVersion": 1, "releaseVersion": release_version, "plugins": entries}
    (output / "catalog.json").write_bytes(canonical(catalog))
    bundle_name = f"native-sources-{release_version}.zip"
    with zipfile.ZipFile(output / bundle_name, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(sources.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    for name in ("catalog.json", bundle_name):
        artifacts[name] = hashlib.sha256((output / name).read_bytes()).hexdigest()
    (output / "SHA256SUMS").write_text("".join(f"{digest}  {name}\n" for name, digest in sorted(artifacts.items())))
    return catalog


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    parser.add_argument("--version", default=(ROOT / "VERSION").read_text().strip())
    args = parser.parse_args()
    catalog = build(ROOT, args.output, args.version)
    print(f"Validated and built {len(catalog['plugins'])} packages and their native source bundle.")
