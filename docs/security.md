# nhpm: Security

Part of the [nhpm development plan](nhpm-plan.md). See [features.md](features.md) for the
features referenced below (making patches, capturing changes, importing instances).

---

## 16. Security

Patches are other people's source code, and site text is written by anyone.

- **Show before applying:** the first time a patch is enabled, nhpm offers to show the
  diff. Downloading never applies anything.
- **Path checks:** file names in a diff (after stripping `-p`) must stay inside the source
  folder: no absolute paths, no `..`.
- **Archive checks:** the same for every entry in a tarball, zip or `.nhinst`.
- **Made patches stay inside the source:** generated diffs are checked with the same path
  rules before saving, never follow symlinks out of the folder, and skip special files.
- **Capturing changes is reversible:** a snapshot and a backup of every changed file are
  taken before the working folder is touched ([features.md](features.md), section 12.3).
- **Imported instances** are treated like downloads: their patches are shown before the
  first build, and their build recipe is shown and must be confirmed if it is `custom`.
- **Terminal safety:** control characters and escape sequences are stripped from all site
  text and imported names before drawing.
- **Safe names:** folder names are built from IDs and `[a-z0-9-]` slugs; instance names are
  limited to `[A-Za-z0-9._-]`.
- **No shell:** every external command uses `execvp` with an argument list.
- **HTTPS with verification** always.
- **Size limits** on downloads, pages and imports.

---
