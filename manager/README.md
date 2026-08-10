# Organizer Patch Manager

True-dark Flutter installer and updater for Prism Family Organizer Patch on Windows.

The manager detects PineconeMC, Prism Launcher, and Freesm Launcher executables; reads the public GitHub Releases feed; verifies every downloaded family payload with SHA-256; and provides Install, Reinstall, Update, and Remove. Replacement uses a same-directory rollback and preserves the pristine launcher exactly once under `.organizer-patch/original/`.

Build with `flutter build windows --release`. The complete `build/windows/x64/runner/Release` directory is the distributable application and must be packaged as one ZIP.
