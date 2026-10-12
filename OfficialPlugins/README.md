# Reviewed official plugin source snapshot

Derived from [scholay/rimes-plugins](https://github.com/scholay/rimes-plugins).
The exact upstream revision and local modifications are recorded in
[UPSTREAM.json](UPSTREAM.json). This Linux fork removes macOS/iOS native
sources and mappings; Android/Windows and platform-neutral adapters remain
for future work. Linux advanced plugin integration is not implemented.

`plugins.lock.json` in the parent project pins the retained input hashes.
Run `python3 scripts/prepare-official-plugins.py` from the parent project to
prepare future Android/Windows builds. This directory is a tracked snapshot,
not a Git submodule. Preserve LICENSE, NOTICE and third-party notices.
