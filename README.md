# Prism Family Organizer Patch

Organizer tables and typed groups for the Prism Launcher family:

- Prism Launcher 11.0.3
- Freesm Launcher 2.2.0
- PineconeMC 11.0.3

The three builds use the same source-level Organizer API. Family-specific release assets contain only the final launcher executable because the projects do not expose a stable binary plugin ABI.

## Patch maintenance

Patched launchers contain **Settings → Organizer Patch**:

- **Check** reads this repository's GitHub Releases feed.
- **Update** downloads only the matching family asset and verifies GitHub's SHA-256 digest before installation.
- **Latest** means the installed patch is current.
- **Remove** restores the pristine executable saved by the installer. Instances and `.pinecone-resource-groups.json` files are not removed.

Executable replacement/restoration is performed after the launcher exits by the small QtCore helper in [`src/OrganizerPatchMaintenance.cpp`](src/OrganizerPatchMaintenance.cpp). It uses a same-directory rollback file and restarts the launcher only after hash verification.

## Test release

`0.1.0-test.1` is a disposable prerelease for validating the update feed and rollback path. Its raw `.exe` files are maintenance payloads, not complete portable launcher archives. A final release will replace it with the complete installer/package.

This repository and the integrated source are licensed under GPL-3.0-only.
