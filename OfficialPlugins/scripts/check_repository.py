#!/usr/bin/env python3
# Copyright 2026 scholay
# SPDX-License-Identifier: Apache-2.0
"""Check repository documentation and license materials without dependencies."""

import hashlib
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    required = (
        "README.md", "LICENSE", "NOTICE", "LICENSING.md", "ATTRIBUTION.md",
        "THIRD_PARTY_NOTICES.md", "CONTRIBUTING.md", "CONTRIBUTORS.md",
        "AGENTS.md", "plugins/README.md",
    )
    errors = []
    for name in required:
        path = root / name
        if not path.is_file() or not path.read_bytes().strip():
            errors.append(f"Missing or empty file: {name}")

    license_path = root / "LICENSE"
    expected = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
    if license_path.is_file():
        if hashlib.sha256(license_path.read_bytes()).hexdigest() != expected:
            errors.append("LICENSE must contain the unmodified Apache-2.0 text.")

    # Check inline relative links in the repository's maintained documents.
    # External URLs and in-page fragments do not require network access.
    for name in required:
        if not name.endswith(".md") or not (root / name).is_file():
            continue
        path = root / name
        for target in re.findall(r"\[[^\]]*\]\(([^\s)]+)\)", path.read_text(encoding="utf-8")):
            parsed = urlsplit(target)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            destination = path.parent / unquote(parsed.path)
            if not destination.exists():
                errors.append(f"Broken local link in {name}: {target}")

    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print("Repository documents, local links and Apache-2.0 license verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
