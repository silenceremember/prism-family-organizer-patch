# Prism Family Organizer Patch

Organizer tables and typed groups for the Prism Launcher family:

- Prism Launcher 11.0.3
- Freesm Launcher 2.2.0
- PineconeMC 11.0.3

The three builds use the same source-level Organizer API. Family-specific release assets contain only the final launcher executable because the projects do not expose a stable binary plugin ABI.

## Patch maintenance

Patched launchers contain **Settings → Organizer Patch**:

- **Check** reads this repository's GitHub Releases feed.
- **Manage** downloads and verifies the separate graphical patcher from [`prism-family-organizer-patcher`](https://github.com/silenceremember/prism-family-organizer-patcher), then closes the launcher and opens it.
- The Flutter manager displays installed/latest versions, downloads only the matching family asset, shows exact progress, and owns **Install**, **Reinstall**, **Update**, and **Remove**.
- Closing or completing the manager returns to the launcher. Removal restores the pristine executable; instances and `.pinecone-resource-groups.json` files are not removed.

Install, update, and removal are performed by the self-contained Flutter application maintained in the separate [`prism-family-organizer-patcher`](https://github.com/silenceremember/prism-family-organizer-patcher) repository. Its verified Windows bundle is downloaded on demand and extracted to a temporary directory, so no maintenance helper is embedded in or persisted beside a family launcher. It preserves the pristine launcher exactly once, validates target paths and SHA-256 digests, uses a same-directory rollback file, updates state atomically, and restarts only after the previous process releases its executable.

## Test release

`0.1.0-test.7` is the current disposable patch prerelease. It contains only the three family `.exe` replacement payloads. The independently versioned Flutter patcher and its Windows ZIP are published exclusively in the patcher repository.

This repository and the integrated source are licensed under GPL-3.0-only.
