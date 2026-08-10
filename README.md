# Prism Family Organizer Patch

Organizer tables and typed groups for the Prism Launcher family:

- Prism Launcher 11.0.3
- Freesm Launcher 2.2.0
- PineconeMC 11.0.3

The three builds use the same source-level Organizer API. Family-specific release assets contain only the final launcher executable because the projects do not expose a stable binary plugin ABI.

## Patch maintenance

Patched launchers contain **Settings → Organizer Patch**:

- **Check** reads this repository's GitHub Releases feed.
- **Update** downloads only the matching family asset, verifies GitHub's SHA-256 digest, extracts the embedded Organizer Patch Installer, and hands the replacement/restart transaction to it.
- **Latest** means the installed patch is current.
- **Remove** restores the pristine executable saved by the installer. Instances and `.pinecone-resource-groups.json` files are not removed.

Install, update, and removal are performed by one QtCore executable in [`src/OrganizerPatchInstaller.cpp`](src/OrganizerPatchInstaller.cpp). The same installer is available as a standalone release asset and is embedded byte-for-byte inside every patched family launcher, so no separately installed maintenance helper is required. It preserves the pristine launcher exactly once, validates absolute paths and SHA-256 digests, uses a same-directory rollback file, updates state atomically, and restarts only after the previous process releases its executable.

## Test release

`0.1.0-test.4` is the current disposable prerelease for validating the embedded two-in-one install/update engine, removal, automatic restart, rollback path, and theme-aware Organizer branding. Its family launchers explicitly register the Organizer branding and embedded-installer Qt resources during startup. Family `.exe` files remain replacement payloads; `prism-family-organizer-patch-installer-windows-x64.exe` is the shared installer engine. A final release will add the interactive discovery/package layer around this engine.

This repository and the integrated source are licensed under GPL-3.0-only.
