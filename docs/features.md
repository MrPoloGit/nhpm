# nhpm: Features

Part of the [nhpm development plan](nhpm-plan.md). This document is a living checklist of
what nhpm does and how each feature area works, tracked against the actual code. For code
style see [style.md](style.md); for source layout see [repo-structure.md](repo-structure.md);
for on-disk file formats see [file-formats.md](file-formats.md); for security considerations
see [security.md](security.md); for the test plan see [testing.md](testing.md).

`[x]` means implemented and verified against the current source; `[ ]` means not yet done (or
only partially). Items with no checkbox are reference material, not features to tick off.

## Contents

1. [Goals and non-goals](#1-goals-and-non-goals)
2. [Concepts and overview](#2-concepts-and-overview)
3. [Talking to the Patch Database](#7-talking-to-the-patch-database)
4. [The patch library and patch versions](#8-the-patch-library-and-patch-versions)
5. [NetHack versions and base sources](#9-nethack-versions-and-base-sources)
6. [Instances and how they are saved](#10-instances-and-how-they-are-saved)
7. [Quilt integration](#11-quilt-integration)
8. [Making patches](#12-making-patches)
9. [Terminal interface (ncurses)](#13-terminal-interface-ncurses)
10. [Command-line mode](#14-command-line-mode)
11. [Appendix: site reference](#appendix-a-site-reference)

Section numbers below match the ones used in the development plan (they skip around other
docs' sections), so cross-references between this document and the plan stay correct.

---

## 1. Goals and non-goals

### Goals

- [x] **Search and download** patches from the NetHack Patch Database (nhpatchdb.alt.org).
- [x] **Keep a local library** of downloaded patches that you can search, including several
      versions of the same patch, and **delete versions** you no longer want.
- [x] **Keep untouched NetHack sources** for each version you use (3.4.3, 3.6.7, 3.7...).
      Choosing a NetHack version means choosing one of these when you create an instance.
- [x] **Save instances**: named setups combining a NetHack version, an ordered list of
      patches (each enabled or disabled, at a specific version), build settings and a private
      folder for save games. Instances are saved automatically, survive restarts, and can be
      cloned, snapshotted, restored, exported and imported.
- [x] **Enable, disable and reorder patches** in an instance, with Quilt doing the actual
      applying and removing, conflicts detected before they happen, and no half-patched
      states.
- [x] **Build and launch** an instance from inside the app.
- [x] **Create a combined patch** from an instance: one plain `.diff` with all its enabled
      patches merged, verified to apply cleanly to clean NetHack source.
- [x] **Create your own patches easily** from code you changed, either inside an instance or
      in any modified NetHack folder (including git checkouts), with build output filtered
      out, a file-by-file review, and the result saved to the library like any downloaded
      patch.
- [x] A **normal full-screen terminal application** built with ncurses: arrow keys,
      function-key bar, incremental search, colors, mouse, clean resizing. (The function-key
      bar itself was later dropped in favor of plain letters -- see 13.9/`ui-progress.md` --
      everything else holds.)

### Non-goals (for version 1)

Deliberately out of scope, not tracked as pending work:

- Porting patches between NetHack versions automatically.
- Uploading patches or posting comments/ratings to the Patch Database.
- Windows support without a POSIX layer (WSL, Cygwin and MSYS2 are fine).
- Replacing Quilt. Every instance's source folder stays a valid Quilt setup that can also be
  managed by hand.

---

## 2. Concepts and overview

| Concept | What it is | Example |
|---|---|---|
| **Index** | Cached list of every patch on the site | 323 entries: ID, name, author, target version, rating |
| **Library** | Patch files you downloaded, several versions each | Color Alchemy 1.1 and 1.2 |
| **Base source** | An untouched NetHack source tree for one version, kept read-only | NetHack 3.6.7 |
| **Instance** | A saved, named setup: one base source + an ordered patch list + build settings + its own install and save-game folder | "367-colorful" = 3.6.7 + menucolors + color-alchemy |
| **Snapshot** | A saved copy of an instance's settings at a point in time | "before trying building patch" |
| **Combined patch** | One plain diff made from an instance's enabled patches | `367-colorful-combined.diff` |
| **Local patch** | A patch you made yourself, stored in the library with ID `local-...` | `local-healer-alchemy` 1.0 |

```
                  +---------------------------------+
                  |  NetHack Patch Database (web)    |
                  +-----------------+-----------------+
                                    | HTTPS (libcurl)
                                    v
   +--------------+      +-------------------+      +------------------------+
   | index cache  |<-----|   site module     |----->|  patch library         |
   | (search)     |      | (fetch + parse)   |      |  (all versions)        |
   +--------------+      +-------------------+      +-----------+------------+
                                                                 | copy on enable
   +--------------+  copy on create             +----------------v-------------+
   | base sources |----------------------------->|  INSTANCE                   |
   | 3.4.3 3.6.7  |                               |  instance.conf  (saved)   |
   | 3.7.0 ...    |                               |  src/  (Quilt stack)      |
   +--------------+                               |  install/ (game + saves) |
                                                   |  snapshots/              |
                                                   +----------------+----------+
                     +------------------+                          | make, run
                     |  ncurses UI      |<-------------------------+
                     +------------------+
```

Downloading a patch puts it in the library. Enabling it in an instance copies it into that
instance's Quilt stack. Creating an instance copies a base source. So:

- you download a patch once and can use it in many instances,
- every instance is independent: changing or deleting one never affects another,
- deleting something from the library or a base source never breaks an existing instance.

---

## 7. Talking to the Patch Database

The site has no API, so nhpm reads its HTML. These details were checked against the live
site in September 2026.

### 7.1 Endpoints

| What | Request | Notes |
|---|---|---|
| Browse list | `GET /?browse` | about 20 patches per page, 17 pages at the time of writing |
| Other pages | `GET /?browse&page=N` | 0-based: `page=1` is page 2 |
| Sorted list | `GET /?browse&sort=name` | `name`, `author`, `ver`, `rating` |
| Patch details | `GET /?ID` | e.g. `/?485` |
| Hosted download | `GET /?download=ID` | only when the site hosts the file |
| Search | `POST /?search` | `searchbar` (max 30 chars), `sort`, `revsort=on`; GET parameters are ignored |

- Pages are encoded as **windows-1252**; libxml2 converts to UTF-8 when told so.
- The site is behind **Cloudflare**: send a descriptive `User-Agent`, keep requests slow,
  and treat an unexpected challenge page as a temporary network error.
- The page count is read from `<select name="pageselect">`, not hard-coded.

### 7.2 Strategy: download the whole index, search locally

- [x] **Refresh index:** fetch every browse page one second apart, parse rows, write
      `index.tsv`. Done on first run, on request, and offered when the index is older than
      `index_max_age_days`.
- [x] **Search** runs locally over the index, as you type.
- [x] **Detail pages** are fetched only when you open or download a patch, and cached.

This gives instant search, filtering by NetHack version (which the site can't do) and
offline use. The site's POST search is not used.

### 7.3 Parsing

libxml2's HTML parser (`htmlReadMemory`, encoding `windows-1252`) plus XPath.

- [x] **Browse rows:** `//table[@class='patchbrowse']//tr[td]` (`src/site.c`)

| Field | XPath (relative to row) | Example |
|---|---|---|
| ID | `td[2]/a/@href`, strip `/?` | `327` |
| Name + version | `td[2]/a` | `64bit 1.0.16` |
| Author | `td[3]` | `Clive Crous` |
| Target | `td[4]` | `NetHack 3.4.3` |
| Rating | `.//span[@class='ratingtext']` (missing means not rated) | `100%` |
| Short description | `td[6]` | `Fixes NetHack for 64bit` |

- [x] **Detail page:** rows of `//table[@class='patchshow']//tr`, label in
      `td[@class='rowname']`: `Name`, `Rating`, `Author`, `For`, `Description`, an unlabeled
      long description (`<br>` line breaks), `Download`, `Added`, optional `Changed`.
- [x] Download row parsed both ways: hosted (`<a href="/?download=485">Get it from us</a>`)
      and external URL (`<a href="https://bilious.alt.org/~paxed/nethack/nh343-msgtype.diff">`).
- [x] Comments (`table.commentshow`) shown read-only, because they often contain working
      links for dead downloads.
- [x] **Splitting name and version:** if the last word of the name matches
      `^v?[0-9]+(\.[0-9]+)*[a-z]?$` (case-insensitive) it is the version (`Dumplog 1.0.0`,
      `Categories v5.2`, `Building V1.1`); otherwise the version is `unversioned`
      (`initpoly`) and the `Changed` date tells downloads apart.

### 7.4 Downloading a patch

- [x] 1. Read the Download link from the (cached) detail page.
- [x] 2. Hosted: `https://nhpatchdb.alt.org/?download=ID`; external: use as-is.
- [x] 3. On failure, if `wayback_fallback = yes`, try
      `https://web.archive.org/web/2id_/<url>` (`id_` returns the raw file). Records
      `via_wayback = yes` (`src/library.c`).
- [x] 4. **Check it's a diff** (unified, context or plain hunk headers). Rejects HTML pages
      and files with no hunks (`lib_looks_like_diff()`).
- [x] 5. **Archives** (`.gz`, `.tar.gz`/`.tgz`, `.zip`) detected by magic bytes and extracted
      to a temporary folder; with several diffs inside, the user picks one.
- [x] 6. Normalize CRLF to LF, store as `patch.diff`, write `version.meta` with its SHA-256.
- [x] 7. Refuse files over `max_download_mb`.

### 7.5 Being polite to the site

- [x] One request at a time (no threads), `request_delay_ms` (default 1000ms) between index
      pages (`src/index.c`), detail pages cached 7 days, `User-Agent: nhpm/<version>
      (+<project URL>)`.

### 7.6 Network module (`net.c`)

- [x] `net_get()`, `net_post()`, `net_download()`, `net_resp_free()` implemented as specified
      in `src/net.h`.

```c
typedef struct { char *data; size_t len; long http_code; char *final_url; } net_resp;
/* Called during transfers; return true to cancel. */
typedef bool (*net_progress_fn)(void *user_data, double done_bytes, double total_bytes);

nhp_err net_get(const char *url, net_resp *out, net_progress_fn progress, void *user_data);
nhp_err net_post(const char *url, const char *form, net_resp *out,
                 net_progress_fn progress, void *user_data);
nhp_err net_download(const char *url, const char *path, size_t max_bytes,
                     net_progress_fn progress, void *user_data);
void net_resp_free(net_resp *r);
```

- [x] Redirects followed, 20s timeout, size limits, TLS verification always on. The progress
      callback lets the UI redraw a progress bar and cancel on Esc (section 13.6).

---

## 8. The patch library and patch versions

A library entry is one patch from the site (by ID), with one or more **downloaded versions**
under it, each in its own folder. This allows keeping an old version that works while trying
a new one, different instances using different versions, and deleting versions you don't want.

| Operation | Behavior | Status |
|---|---|---|
| List | patches with versions nested, target NetHack version, which instances use each version | [x] `lib_list()` |
| Search | incremental, same syntax as the Database screen (13.10) | [x] |
| View | details, `version.meta`, diff in the pager | [x] `lib_version_diff_path()` |
| Delete version | removes that version's folder, after confirmation | [x] `lib_delete_version()` |
| Delete patch | removes all versions, after confirmation | [x] `lib_delete_patch()` |
| Verify | re-hash files, compare with `sha256` | [x] `lib_verify_version()` |
| Check for update | re-read the detail page; mark and offer newer versions | [x] `lib_fetch_detail()` |
| Import local file | add a `.diff` from disk that isn't on the site (IDs start with `local-`) | [x] `lib_import_local()` |

- [x] **Deleting a version an instance uses:** nhpm scans every `instance.conf` and lists the
      instances using it. Instances keep their own copy of the patch, so they keep working;
      the warning only says that re-adding that version later means downloading it again.

---

## 9. NetHack versions and base sources

### 9.1 How the NetHack version is chosen

- [x] The NetHack version is a property of each **instance**. When creating an instance you
      pick a **base source**: an untouched copy of one NetHack version that nhpm keeps
      read-only. So "which NetHack version am I using" is answered by "which instance is
      active", and the header always shows it.

### 9.2 Getting base sources (F7 Sources screen)

| From | How | Status |
|---|---|---|
| Local folder | copy a NetHack source folder the user already has | [x] `bsrc_add_from_folder()` |
| Tarball | extract `nethack-367-src.tgz` or similar (path checks on every entry) | [x] `bsrc_add_from_tarball()` / `bsrc_extract_tarball_checked()` |
| Git | `git clone --depth 1 --branch <tag>` for a release tag, or a branch for development versions | [x] `bsrc_add_from_git()` |
| Known versions | a hand-verified catalog (3.4.3, 3.6.7, 5.0.0) that downloads/clones and registers in one step | [x] `bsrc_add_known()` (not in the original spec; added since, see `ui-progress.md` F7) |

nhpm does not ship NetHack sources, and the git option depends on the user's network. In
every case the version is read from the source itself, the files are copied into
`sources/<version>/tree/` and made read-only.

### 9.3 Reading the version

From `include/patchlevel.h`:

```c
#define VERSION_MAJOR 3
#define VERSION_MINOR 6
#define PATCHLEVEL 7
```

- [x] If those can't be found, the user types the version in, and it's stored in
      `source.meta`.
- [x] Variants (SLASH'EM, UnNetHack, EvilHack...) can be imported as base sources too; they
      are marked `variant` and compatibility with Database patches is shown as unknown.

### 9.4 Compatibility marks

The site's `For` field (`NetHack 3.6.0`) is compared with the active instance's version:

| Mark | Meaning | Example (instance on 3.6.7) |
|---|---|---|
| `=` green | exact version | patch for 3.6.7 |
| `~` yellow | same major.minor series | patch for 3.6.0 |
| `!` red | different series, usually needs porting | patch for 3.4.3 |
| `?` grey | not vanilla, or unknown | patch for "SLASH'EM" |

- [x] Marks computed as above, shown on Database/Library/Patches.
- [x] These are hints; the real answer is a dry run against the instance (11.4). The
      Database screen shows `=` and `~` by default, with one key to show all.

### 9.5 Removing a base source

- [x] Allowed only when no instance was created from it, or after confirmation if some were
      (instances have their own copy of the source, so they keep working; only "rebuild from
      clean source" (10.5) needs the base). `bsrc_remove()`.

---

## 10. Instances and how they are saved

### 10.1 What an instance is

An instance is a named, saved NetHack setup. It contains:

- the **base source** it was created from (NetHack version),
- an **ordered patch list**: each entry is a library patch at a specific version, enabled
  or disabled, with its strip level and whether it was hand-edited,
- **build settings**: build recipe, extra `make` variables, game name,
- its own **install folder**, so each instance's save games, bones files and high scores
  are kept apart,
- **notes** and timestamps.

Two kinds:

| Kind | Source folder | Use | Status |
|---|---|---|---|
| **Managed** (default) | `instances/<name>/src/`, copied from a base source | normal use | [x] |
| **Linked** | an existing folder elsewhere, e.g. your own git checkout | developers who already have a tree | [x] `inst_link()` |

Linked instances work the same, except nhpm never deletes or recreates their source
folder, and "rebuild from clean source" is not available.

### 10.2 The instance file (`instance.conf`)

- [x] The single source of truth for an instance, with every field below implemented
      exactly as shown (`src/instance.c`):

```ini
# nhpm instance: edited by nhpm; safe to read, careful when editing
format      = 1

[instance]
name           = 367-colorful
kind           = managed               # or: linked
source         = 3.6.7                 # base source this was created from
source_path    = src                   # relative = inside the instance folder
created        = 2026-11-15T13:55:02Z
modified       = 2026-11-15T20:14:03Z  # bumped on every save, for any reason
stack_modified = 2026-11-15T20:10:00Z  # bumped only when the patch stack itself changes
last_built     = 2026-11-15T20:20:41Z
last_played    = 2026-11-15T21:02:10Z
notes          = Vanilla plus colors. Good for streaming.

[build]
recipe      = nethack-3.6-linux        # see 10.6
make_vars   = CFLAGS+=-O2
install_dir = install
game_name   = nethack

[patch]
id       = local-menucolors
version  = 1.5
enabled  = yes
strip    = 1
sha256   = 81be...
modified = no

[patch]
id       = 498
version  = 1.2
enabled  = yes
strip    = 1
sha256   = 2c4d...
modified = no

[patch]
id       = 508
version  = V1.1
enabled  = no
strip    = 0
sha256   = d03e...
modified = no
```

- [x] Repeated `[patch]` sections, in order, are the patch stack.
- [x] `modified = yes` means the patch was hand-fixed after a conflict (11.6); the instance's
      copy then differs from the library and is never overwritten by library updates.
- [x] `sha256` is of the instance's copy, so nhpm can tell if it was changed outside the app.
- [x] `format` allows changing the file layout later with automatic upgrades.

### 10.3 When instances are saved

- [x] There is no "Save" button to forget. nhpm saves **automatically after every change**
      that succeeds: enabling, disabling, moving, adding or removing a patch, editing
      settings, renaming, building, playing.
- [x] Each save: copies the current `instance.conf` to `instance.conf.bak`, writes the new
      file to a temporary file, `fsync`s it, and renames it into place (`inst_save()`).
- [x] Changes that fail are rolled back (11.5) and never saved. The patch stack in `src/`
      (Quilt's `series` and `patches/`) is kept in step with `instance.conf`; the order of
      operations is "change the source folder with Quilt first, save `instance.conf` only if
      that succeeded".

### 10.4 Loading and checking an instance

- [x] When an instance is opened (at startup, or when switched to), nhpm compares
      `instance.conf` with the actual source folder: `series` matches the enabled patches in
      order, `quilt applied` shows them all applied, each patch file's hash matches
      `sha256`. (`stack_check_consistency()`.)
- [x] If something differs, nhpm shows what's different and offers **Adopt** (`stack_adopt()`),
      **Restore** (`stack_restore()`) or **Open read-only**. It never silently changes either
      side.

### 10.5 Instance operations

| Operation | What happens | Status |
|---|---|---|
| **New** | pick name, base source (= NetHack version), build recipe; copy the base source into `src/`, set up Quilt, write `instance.conf`, and switch to it | [x] `inst_new()` |
| **Clone** | copy the whole instance under a new name (source, patches, settings); optionally without save games | [x] `inst_clone()` |
| **Rename** | renames the folder and updates `instance.conf`; refused while the instance is locked | [x] `inst_rename()` |
| **Delete** | after confirmation; asks separately whether to keep the save games (moves `install/` to `~/.local/share/nhpm/orphaned-saves/<name>-<date>/`) | [x] `inst_delete()` |
| **Switch** | make another instance active; remembered for next start | [x] `cfg_set_active_instance()` |
| **Snapshot** | save a copy of `instance.conf` into `snapshots/` with a timestamp and optional label | [x] `inst_snapshot_create()` |
| **Restore snapshot** | take a snapshot before restoring (so it can be undone), then pop all, rewrite the stack from the snapshot, push; missing patch versions are fetched from the library or flagged | [x] `stack_restore_snapshot()` |
| **Rebuild from clean source** | for managed instances: replace `src/` with a fresh copy of the base source and re-apply the stack; fixes a tree damaged by hand edits | [x] |
| **Export** | write `<name>.nhinst`, a `.tar.gz` holding `instance.conf` and the instance's copies of its patches (not the NetHack source, not save games) | [x] `inst_export()` |
| **Combined patch** | write one plain diff of all enabled patches, for people without nhpm | [x] `mkp_combined()` (section 12.2) |
| **Import** | read a `.nhinst`: choose a name, pick a base source of the matching version (or import one), take patches from the library when the hash matches, otherwise use the embedded copies | [x] `inst_import()` |

Export/import is how you share a setup ("here's my 3.6.7 build with these 6 patches") or move
it to another computer.

### 10.6 Building and playing an instance

NetHack's build setup differs between versions, so builds use **recipes**: small built-in
command lists, chosen per instance and editable.

| Recipe | Commands (run in `src/`) | Status |
|---|---|---|
| `nethack-3.4-unix` | `sh sys/unix/setup.sh`, then `make all`/`make install`, both `PREFIX=<install_dir> CC='cc -std=gnu89 -DLINUX' WINTTYLIB=-lncurses CHOWN=true CHGRP=true` | [x] verified end to end |
| `nethack-3.6-linux` | `sh sys/unix/setup.sh sys/unix/hints/linux`, then `make all`/`make install`, both `PREFIX=<install_dir> CC='cc -std=gnu89'` | [x] verified end to end |
| `nethack-5.0-linux` | `sh sys/unix/setup.sh sys/unix/hints/linux.500`, `make fetch-lua`, then `make all`/`make install`, both `PREFIX=<install_dir>` -- no `CC` fixup | [x] verified end to end |
| `custom` | whatever the user types, via `custom_commands` | [x] `run_custom_recipe()`, `$PREFIX` substituted |

There is deliberately no `nethack-3.7-linux` recipe: NetHack's 3.7 line never had its own
release -- it shipped as 5.0.0 instead (which `nethack-5.0-linux` targets), so a 3.7-specific
recipe would have no real source to build or verify against. If a genuine 3.7 source ever
surfaces, `custom` with the same hints-file/`fetch-lua`/`CC` shape as `nethack-3.6-linux`
covers it without needing a new built-in recipe.

`PREFIX` has to be passed to the build step too, not just install: `HACKDIR` (the data
directory the game looks for at runtime) is derived from `PREFIX` and baked into the compiled
binary at build time, so a `make install PREFIX=X` after a plain `make all` produces a binary
that still looks for the old (or default, `~/nh/install`) location.

The `CC` fixup works around a toolchain-vs-vintage-source mismatch, not a choice: current
GCC/glibc reject NetHack's own old K&R-style empty-parameter externs (`srand48()`, `sleep()`,
...) in `include/system.h` as conflicting redeclarations of the real ones in
`<stdlib.h>`/`<unistd.h>`. `CC='cc -std=gnu89 ...'` (not `CFLAGS`, so it layers on top of a
hints file's own `CFLAGS` instead of replacing it) relaxes that back to pre-C99 rules, which is
all `nethack-3.6-linux` needs -- its hints file already sets everything else a modern Linux
build needs. `nethack-3.4-unix` needs three more, because 3.4.3 predates
the per-OS hints-file mechanism those recipes use: `-DLINUX` (`win/tty/termcap.c` has its own
redundant `extern char *tparm()`, guarded by `#ifndef LINUX`, that conflicts with `curses.h`'s
real variadic one -- unlike the empty-parameter externs, gnu89 doesn't relax this one), both of
which `sys/unix/hints/linux` sets for 3.6+, as does `WINTTYLIB` (the default `-ltermlib`
doesn't exist on modern systems; `Makefile.src`'s own comments name `-lncurses` as the Linux
choice), and `CHOWN`/`CHGRP` (both default to setting up a shared, multi-user `games`-group
install that `Makefile.top` documents overriding to `true`; nhpm always installs into one
instance's own private folder, so that setup is never wanted).

`nethack-5.0-linux` needs none of this: NetHack 5.0 (the rewrite the 3.7 line actually shipped
as) is actively maintained and its `linux.500` hints file already targets current glibc
directly -- `include/integer.h`'s own C99 detection (`__STDC_VERSION__ >= 199901L`) is why
forcing `-std=gnu89` here would actually break the build instead of fixing it: it would hide
that macro and send NetHack down its own pre-C99 `int64_t` fallback, which then conflicts
with glibc's real one the same way the vintage recipes conflict without the fixup. It still
needs `make fetch-lua` first, same as 3.7 would have: that's where NetHack grew embedded Lua
scripting (for the dungeon/level compilers).

For `custom`, the commands actually run come from a separate `custom_commands` key in
`instance.conf` (`;`-separated, run in `src/` in order), not from the recipe name. Each is
split into an argument list the same whitespace-only way `make_vars` is (no quoting) and run
through `proc_run_streaming()` like every other recipe step (6.8: never a real shell, so no
`&&`/`|`/`$(...)`), stopping at the first that fails. Every literal `$PREFIX` in
`custom_commands` is substituted with the instance's actual install path first
(`str_replace_all()`, util.c) -- the same value the built-in recipes already pass as
`PREFIX=<install_dir>` -- so a command needing it doesn't have to spell out an absolute path
by hand: `make install PREFIX=$PREFIX`. An empty `custom_commands` is why `build_run()`
returns `NHP_EINVAL` for `custom`. Setting `custom_commands` itself can be done by
hand-editing `instance.conf` or from the Build screen's own `c` key (`ui-progress.md` F6).

`nethack-3.6-linux`, `nethack-3.4-unix` and `nethack-5.0-linux` are all verified end to end
against their real sources (NetHack-3.6.7_Released, the official 3.4.3 tarball, and
NetHack-5.0.0_Released): the built `nethack` binary installs to `<install_dir>/games/nethack`
(a wrapper script that `cd`s into `<install_dir>/games/lib/nethackdir` before running the real
binary; Play runs this wrapper, not the binary directly), and a captured run through character
creation completes cleanly on all three.

- [x] Build output streams into the Build screen and is saved to `build.log`; `s` there
      exports a copy to a file you name, for sharing a failure without digging through the
      instance's folder.
- [x] The install goes into the instance's own `install/` folder, so save games, bones and
      scores stay with the instance.
- [x] **Play** (`p` on the Instances screen -- not global, and not on Build even right after a
      build succeeds there) suspends the interface (13.7), runs the installed game, and
      returns to nhpm when the game exits. `last_played` is updated.
- [x] The instance list shows whether the last build is out of date (`stack_modified` newer
      than `last_built` -- deliberately not the instance's general `modified` timestamp,
      which is bumped on every save for any reason, including just playing the game).

### 10.7 Saving the app's own state

nhpm remembers in `~/.config/nhpm/ui-state`:

```ini
active_instance = 367-colorful
last_screen     = p
db_filter       = v:3.6
db_sort         = rating:desc
library_folded  = 485,397
```

- [x] `active_instance` is persisted and read back (`cfg_get_active_instance()` /
      `cfg_set_active_instance()`), so reopening nhpm puts you back on the instance you had
      active.
- [x] `last_screen` is persisted (a screen's `nav_letter`, not its name -- `cfg_get_ui_state()`/
      `cfg_set_ui_state()` in `config.c`, written by `ui.c`'s `switch_to()` on every screen
      change and read back by `initial_screen_index()` at startup), so reopening nhpm lands on
      the screen you were last on instead of always Instances.
- [x] `db_filter`/`db_sort` (Database's own search text and sort column+direction) are
      persisted the same way, written by `scr_db.c` after a search commits or `s`/`S` is
      pressed, and restored once per process the first time the Database screen is entered.
- [x] `library_folded` -- the Library screen's own **f** key collapses the selected row's
      patch down to just its named row (appending `(+)` to the version column, same glyph
      Database's own "all versions off" uses), comma-separated ids persisted the same way as
      `db_filter`/`db_sort` and restored once per process on first entry
      (`restore_folded_once()`, `scr_library.c`). Unfolded (every version shown) is still the
      default, matching the 13.9 F4 mockup.

This file is a convenience only; if it is missing or broken, nhpm starts with defaults.

### 10.8 Locking

- [x] Each instance has a `lock` file held with `flock()` while nhpm has it open for changes.
      A second nhpm opening the same instance gets it read-only, with a message showing the
      other process's PID (`inst_lock_acquire()`).

---

## 11. Quilt integration

### 11.1 Principles

- [x] nhpm changes an instance's source folder only through `quilt` and by editing `series`
      while the affected patches are popped. It never edits `.pc/`.
- [x] The source folder is always a valid Quilt setup.
- [x] Every multi-step change is all or nothing.
- [x] `instance.conf` is saved only after the folder change succeeded (10.3).

### 11.2 Running quilt

- [x] `proc.c` runs commands with `fork` + `execvp` and an argument array (never a shell), in
      the instance's source folder, capturing stdout and stderr.
- [x] Environment set exactly as specified: `QUILT_PATCHES=patches`, `QUILT_SERIES=series`,
      `QUILT_PC=.pc`, `LC_ALL=C`, `QUILTRC=-` (`src/quilt.c`).

`QUILTRC=-` keeps quilt from reading the user's `~/.quiltrc` or any system-wide default one,
so nhpm's behavior doesn't depend on local quilt configuration (color codes in output,
`refresh --backup` leaving stray `<patch>~` files, custom push/pop arguments, etc.).

- [x] Commands used: `quilt series`, `quilt applied`, `quilt push [name|-a]`,
      `quilt pop [name|-a]`, `quilt push -f`, `quilt refresh`, `quilt files <name>`.

### 11.3 Enabled vs disabled

- [x] Quilt has no "disabled" state. Enabled patches are normal `series` lines
      (`498-color-alchemy.diff -p1`). Disabled patches stay in `patches/`, and their line is
      a comment quilt ignores: `# nhpm-disabled: 508-building.diff -p0`. The full order,
      including disabled patches, is also in `instance.conf`.

### 11.4 Strip level and dry runs

- [x] Patches use `-p0`, `-p1` or `-p2` inconsistently. When a patch is first enabled, nhpm
      tries, with the stack applied up to just before it:

```
patch --dry-run --batch --force -pN -d <src> -i <patch.diff>     for N = 1, 0, 2
```

- [x] Picks the level with the fewest failed hunks (ties: `-p1`), throwing out levels where
      every file is "can't find file to patch". The result is stored as `strip` in
      `instance.conf`. Output is parsed for `Hunk #N FAILED`, `succeeded ... with fuzz` and
      `can't find file`.

### 11.5 Operations (in `stack.c`)

Series `P1 … Pn`, target `Pk`.

- [x] **Enable `Pk`:** remember the top applied patch, `quilt pop` down to `Pk-1`, dry run
      `Pk` (open the conflict dialog on failure, 11.6), uncomment and `quilt push Pk`, push
      each later enabled patch; on a later failure, pop back, re-comment `Pk`, push back to
      the remembered top and report "enabling Pk breaks Pm"; save `instance.conf` on success.
      (`stack_enable()`.)
- [x] **Disable `Pk`:** pop to `Pk-1`, comment its line, push the rest; undo and report if a
      later patch depended on it. (`stack_disable()`.)
- [x] **Move:** pop below both positions, swap lines, push; undo on failure.
      (`stack_move()`.)
- [x] **Add from library:** rejected (`NHP_EEXIST`, nothing touched) if the id is already in
      the stack, enabled or not -- otherwise copy `patch.diff` to `patches/<id>-<slug>.diff`,
      add a disabled entry, then Enable. (`stack_add()`.)
- [x] **Remove:** disable if needed, then delete the line, the file and the entry.
      (`stack_remove()`.)

### 11.6 Conflicts

```
 ┌──────── guiding-candle 1.1 does not apply cleanly ────────┐
 │  src/light.c     hunk 2 of 3 FAILED                       │
 │  src/objects.c   hunk 5 of 5 FAILED                       │
 │  src/apply.c     3 hunks OK                               │
 │                                                           │
 │  [F]orce   apply what fits, then fix each .rej by hand    │
 │  [V]iew    the failing hunks                              │
 │  [C]ancel  leave the instance as it was                   │
 └───────────────────────────────────────────────────────────┘
```

- [x] **Force** backend: takes a snapshot (`stack_force_begin()` -> `inst_snapshot_create()`),
      runs `quilt push -f`, leaving failed hunks as `.rej` files next to each source file.
      `stack_force_finish()` then runs `quilt refresh` and marks the patch `modified = yes` in
      `instance.conf`.
- [x] **[F]orce/[V]iew/[C]ancel** (`ui_resolve_conflict()` in `ui.c`): a popup menu
      (`ui_show_popup_menu()`) offering the three choices, looping back to itself after
      **[V]iew** so the user can still decide afterward. **[F]orce** runs
      `stack_force_begin()`, hands `$EDITOR` every file with failed hunks plus its `.rej`
      (`ui_run_editor()`, the interface suspended per 13.7 -- clean files in the same patch
      aren't opened, nothing to fix there), then `stack_force_finish()`. **[C]ancel** (or Esc)
      leaves the instance exactly as it was, since a conflict is always rolled back by the
      backend before it's reported (11.5) -- there's nothing left to undo.
- [x] Offered only on the patch actually being enabled (`conflict->is_requested_patch`): the
      Patches screen's **Space** (enabling, not disabling) and Database's **D**/Library's
      **a** (which add-then-enable, 11.5). A later patch broken by the attempt
      (`!is_requested_patch`) falls back to the plain `ui_show_conflict()` detail view, since
      forcing *that* patch wouldn't fix what actually went wrong; so do Disable/Move/Remove's
      own conflicts, which the spec never ties to Force in the first place.

### 11.7 Checking disabled patches without applying them (`c`)

- [x] The Patches screen's **c** fills in each disabled patch's Status column with a
      hunk-failure count, without touching the stack. Since disabled patches are never
      applied, the source tree as it stands already reflects every *enabled* patch;
      `stack_check_disabled()` just dry-runs each disabled slot against that tree at its
      known `strip` (detecting one first if it has none yet, 11.4), in patch-list order, and
      returns a `failed_hunks` count per slot. Nothing is pushed, popped, or written -- it's
      read-only. The CLI exposes it as `nhpm status --check`, listing each disabled patch
      with either "applies cleanly" or "N of M hunks would fail".

### 11.8 Undo: an automatic snapshot per stack change

- [x] Several operations already take a labeled instance snapshot before a risky step (Force
      above; starting an edit or a new patch, 12.4). Patches' **t**/**u** extend this to
      every operation in 11.5 (Enable, Disable, Move, Add, Remove): each takes a snapshot
      labeled `auto-<operation>-<patch>` right before it runs -- dashes, not `auto:
      <operation> <patch>`, because a snapshot's label is never stored verbatim;
      `inst_snapshot_create()` only keeps it as part of the filename, sanitized to
      alnum-and-single-dashes, and that's what's read back.
- [x] **t** takes one manually, with a label you type, same as Instances' Snapshots dialog
      (10.2).
- [x] **u** finds the single most recent snapshot whose label starts with `auto-` (not just
      the most recent snapshot overall, so an earlier manual one isn't silently consumed) and
      restores it the same way the Snapshots dialog's Enter does: `inst_snapshot_restore()` to
      put the old patch list back in `instance.conf`, then `stack_restore()` (10.4) to re-sync
      the actual Quilt folder to match it. (`stack_undo()`.) The CLI exposes it as `nhpm
      undo`.

### 11.9 Files touched by more than one patch

- [x] The Patches screen's details area flags files more than one *enabled* patch touches, so
      a conflict a dry run wouldn't catch (both patches apply cleanly alone, but each expects
      to be the only one editing that file) is visible before it bites.
      `stack_find_shared_files()` collects `files_touched` (section 8's library metadata) for
      every enabled slot's library version, and returns the paths that show up for more than
      one. Best-effort like `files_touched` itself: a hand-edited or freshly captured patch
      with no library version yet contributes nothing. The CLI exposes it as part of `nhpm
      status --check` (11.7), listing each shared file with the patches that touch it.

---

## 12. Making patches

Besides using other people's patches, nhpm produces patches of its own, in two ways:

| Feature | Input | Output | Status |
|---|---|---|---|
| **Combined patch** (12.2) | an instance | **one** `.diff` containing all its enabled patches, applicable to clean NetHack source | [x] |
| **Your own patch** (12.3-12.6) | code you changed, inside an instance or in any NetHack folder | a normal patch file, saved to the library and usable like a downloaded one | [x] |

A combined patch is different from **exporting an instance** (10.5): the export is nhpm's
own `.nhinst` package (settings plus the separate patches, only useful to another nhpm);
the combined patch is a plain diff anyone can apply with `patch -p1`, no nhpm needed.

All generated patches use the same format: unified diff, `a/` and `b/` path prefixes (apply
with `-p1`), LF line endings, files in sorted order, and a header block at the top. `patch`
ignores text before the first `---` line, so the header is safe:

```
nhpm combined patch
Name:      367-colorful
NetHack:   3.6.7
Created:   2026-11-15T21:40:12Z by nhpm 0.1
Apply:     cd nethack-3.6.7 && patch -p1 < 367-colorful-combined.diff

Contains, in order:
  1. menucolors 1.5            (local)            sha256 81be...
  2. Color Alchemy 1.2         (nhpatchdb #498)   sha256 2c4d...
  3. Dumplog (New) 1.0.0       (nhpatchdb #485)   sha256 77a0...

diff -Naur a/src/potion.c b/src/potion.c
--- a/src/potion.c
+++ b/src/potion.c
@@ ...
```

### 12.1 Telling real changes apart from build output

Diffing a NetHack folder naively would pick up object files, compiled programs and files the
build generates. nhpm avoids that in three ways:

- [x] 1. **Ignore list:** patterns that are never part of a patch. A general list (`*.o`,
      `*.a`, `*.obj`, `*~`, `*.orig`, `*.rej`, `.pc/`, `patches/`, `series`, `.nhpm/`,
      `.git/`) plus a per-version list stored with each build recipe (10.6), e.g. for 3.6:
      the Makefiles copied by `setup.sh`, `include/onames.h`, `include/pm.h`,
      `include/date.h`, `src/monstr.c`, `src/vis_tab.c`, `util/makedefs`, `util/lev_comp`,
      `dat/*.lev`, `dat/data`, `dat/nhdat`. The user can add patterns in `config`
      (`ignore = ...`) or per instance. (`mkp_is_build_output()`, `mkp_is_ignored()`.)
- [x] 2. **Binary files are skipped** (a NUL byte in the first 8 KB) and listed separately.
      (`mkp_looks_binary()`.)
- [x] 3. **New files need a tick:** modified and deleted files are selected by default; files
      that don't exist in the original are shown but **unticked** unless they look like
      source (`.c`, `.h`, `.des`, `.lua`, `.txt` in `dat/` or `doc/`). The user always sees
      and confirms the file list (12.5). (`mkp_looks_like_source()`.)

Where possible nhpm also avoids build output entirely by working on clean copies
(combined patch) instead of the instance's working folder.

### 12.2 Combined patch

**What it is:** a single diff from clean NetHack source to "clean source with all of this
instance's enabled patches applied, in order". Disabled patches, build output and any
untracked edits in the working folder are **not** included.

**How it's made** (`mkp_combined()` in `mkpatch.c`), using two temporary copies so the
instance itself is never touched:

- [x] 1. **A = clean source.** Managed instance: copy of its base source. Linked instance:
      copy of its source folder with `quilt pop -a` run in the copy.
- [x] 2. **B = A + patches.** Copy A, then apply each enabled patch from the instance's
      `patches/` with its stored strip level (`patch -p<strip> --batch`), in order. Any
      failure stops with the same message as 11.5 (it shouldn't happen, since the instance
      applies).
- [x] 3. `diff -Naur --exclude=...` between A and B (run as `diff -Naur a b` from a temporary
      parent folder so paths come out as `a/...` and `b/...`), filtered by the ignore list.
- [x] 4. Write the header and the diff.
- [x] 5. **Verify:** dry-run the result with `patch -p1 --dry-run` on another clean copy of
      A; it must apply with zero failures and zero fuzz, or nhpm reports a bug instead of
      handing out a broken file.
- [x] 6. Delete the temporary copies.

**Where it goes:** the dialog offers:

- [x] **Save to file** (default
      `~/.local/share/nhpm/exports/<instance>-combined-<date>.diff`, or any path),
- [x] **Add to library** as a local patch (`local-<instance>-combined`, new version each
      time),
- [x] **Flatten into a new instance:** create a new instance on the same base source whose
      stack is just this one combined patch. Useful as a stable baseline to put further
      patches on.

- [x] Options in the dialog: include/exclude individual enabled patches (default: all), and
      an optional "also include my uncommitted edits" (adds 12.3's captured changes as a last
      step).

### 12.3 Your own patch, inside an instance (recommended)

The usual way: make an instance, change the code in its `src/` folder with any editor, then
let nhpm turn the changes into a patch.

- [x] **Step 1: change the code.** Edit files in the instance's `src/` folder however you
      like: your editor, an IDE, `sed`. Or from the Patches screen press **o** to pick a file
      and open it in `$EDITOR`. You don't need to tell Quilt anything first.
- [x] **Step 2: capture changes (`w` on the Patches screen).** `mkp_changes()` builds the
      expected state in a temporary copy (clean source plus the currently applied patches),
      diffs it against the real `src/` (filtered as in 12.1), and shows the Changes screen
      (12.5) with every changed file.
- [x] **Step 3: save as a patch (`mkp_capture()`).** Ticks files, fills in name/version
      (default `1.0`)/author/target/description, then: takes an automatic snapshot and backs
      up the changed files to `instances/<name>/backup/<date>/`; writes the diff (selected
      files only) with a header; puts the working folder back to the expected state by
      reverse-applying that diff (`patch -R -p1`, unselected changes stay in place); adds the
      patch to the top of the stack via `quilt import` then `quilt push`; saves the patch
      into the library as `local-<slug>` version `1.0` and records it in `instance.conf`. If
      any step fails, the backup and snapshot are restored and nothing is saved.

Steps 3-4 are why nhpm doesn't simply `quilt add` files after the fact: Quilt only
records a file's original contents if it is added *before* editing. Reverse-apply then
import gives the same result safely, whatever editor you used.

- [x] **Shortcut (`N` New patch):** creates an empty patch at the top of the stack right away
      (`quilt new`/`stack_new_patch()`), and files you open with **o** are added to it first
      (`quilt edit`/`stack_track_file()`). When done, **R** runs `quilt refresh` and saves to
      the library. This is plain Quilt workflow for people who prefer it; capture (`w`) works
      either way.

### 12.4 Updating your own patch

To change a patch you made earlier, **E** (Edit patch) on it in the Patches screen:

- [x] 1. nhpm pops the stack until that patch is on top (snapshot first).
      (`stack_edit_begin()`.)
- [x] 2. You edit files; files opened with **o** are tracked automatically (`quilt edit`), and
      capture (`w`) finds other edits and adds them to this patch.
- [x] 3. **R** (refresh) runs `quilt refresh`, pushes the patches above it back on (with the
      usual conflict handling), and saves the result as a **new version** in the library (1.0
      to 1.1, editable), keeping the old version. (`stack_edit_refresh()`, `lib_add_version()`.)

- [x] The same works for downloaded patches: editing one makes a local copy marked
      `modified = yes`, and with **L** you can save it to the library as your own variant
      (e.g. `local-498-color-alchemy-my` 1.2-1). (`lib_fork_as_local()`.)

### 12.5 The Changes screen

```
 Changes in 367-colorful: 5 files (2 hidden as build output)
      Status  File                         +/-
 >  [x]  M    src/potion.c                 +24 -3
    [x]  M    src/objects.c                 +6 -1
    [x]  A    dat/alchemy.txt              +40
    [ ]  A    src/potion.c.save            +812     backup file?
    [ ]  B    win/share/tiles.bmp          binary, skipped
 ──────────────────────────────────────────────────────────────────────────
  @@ -1210,7 +1210,9 @@ mixtype(o1, o2)
  -        return POT_WATER;
  +        if (Role_if(PM_HEALER))
  +            return POT_HEALING;
 Space tick  Enter full diff  h show hidden  i ignore pattern  S Save as patch  Esc back
```

- [x] Status: `M` modified, `A` added, `D` deleted, `B` binary (can't be included).
- [x] **Enter** shows the file's full diff in the text view.
- [x] **i** adds the selected file's pattern to the ignore list.
- [x] **h** shows the files hidden as build output, in case one was hidden by mistake.
- [x] Per-row `+/-` line counts and the Details pane's first-hunk snippet: one combined diff
      of every non-binary file is parsed once per dialog open (`parse_diff_stats()`,
      `scr_changes.c`), splitting on each file's own `diff -Naur a/X b/X` header -- never
      recomputed per row or per keystroke, since `full_diff()` itself rebuilds a whole
      comparison tree per call (12.3). Shown in `app->layout.details`, the same pane every
      other screen uses, refreshed as the selection moves.

### 12.6 Your own patch, from any modified NetHack folder

For code you changed outside nhpm, e.g. a NetHack folder you've been hacking on for
years. From the Library screen, **m** (Make patch from folder):

- [x] 1. **Pick the modified folder.** nhpm reads its version from `patchlevel.h`.
- [x] 2. **Pick what it started from:** a base source of that version (default, if one
      exists); another folder or a tarball of clean source; **if the folder is a git
      repository:** a commit, tag or branch (e.g. `NetHack-3.6.7_Released`) -- nhpm then uses
      `git diff <ref>` (plus untracked files, listed as new) instead of comparing folders,
      which is faster and ignores whatever git ignores. (`mkp_from_folder_changes()`.)
- [x] 3. nhpm compares the two (filtered as in 12.1), shows the Changes screen, and you tick
      files and fill in the form as in 12.3.
- [x] 4. The patch is saved to the library as `local-<slug>` (`mkp_from_folder()`) and, if you
      want, added to an existing instance or used to create a new instance on the matching
      base source.

The modified folder is only read, never changed.

### 12.7 Sharing your patches

- [x] **Export patch** (**x** in the Library on a local patch, or after creating one) writes
      `<slug>-<version>.diff` to the exports folder or any path, header included.
      (`mkp_export_patch()`.)
- [x] The header's fields match what the NetHack Patch Database's "Add" form asks for (name,
      version, NetHack version, short and long description), so submitting it there is
      copy-and-paste. nhpm does not upload anything itself (section 1 non-goals).
- [x] **Show apply instructions** puts the matching `patch -p1 < file` command in a dialog so
      it can be copied into a README or forum post.

---

## 13. Terminal interface (ncurses)

The detailed, per-key, per-screen implementation checklist for the UI lives in
**[ui-progress.md](ui-progress.md)** and is kept current there, tracked against the design
below -- it is not duplicated here to avoid the two drifting apart. At a glance: every screen
(Help, Instances, Database, Library, Patches, Build, Sources, the Changes dialog) is done or
mostly done; the main open gap left is the automated tmux smoke test.

### 13.1 Why ncurses, and which parts

The interface is written directly against **ncursesw** (the wide-character build) plus the
**panel** library. The ncurses `menu` and `form` libraries are **not** used: they are awkward
for long, filterable, multi-column lists, and a small set of custom widgets gives better
control over the `htop` look. The panel library is used for dialogs that float over screens.

### 13.2 Startup and shutdown

```c
setlocale(LC_ALL, "");            /* before initscr, for UTF-8 */
initscr();
cbreak();                         /* keys arrive immediately, Ctrl-C still works */
noecho();
keypad(stdscr, TRUE);             /* arrow keys, F-keys, KEY_RESIZE */
curs_set(0);                      /* hide cursor except in input fields */
set_escdelay(25);                 /* Esc responds quickly */
mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);
if (has_colors()) { start_color(); use_default_colors(); init_theme(); }
```

- `atexit()` and handlers for SIGINT/SIGTERM call `endwin()`, so the terminal is never left
  broken; SIGINT during an operation is turned into "cancel" first.
- Fatal errors call `endwin()` before printing the message.
- Only the wheel is requested, not `BUTTON1_CLICKED`: nothing reads a click yet (clicking a
  row or the key bar is still open, `ui-progress.md`), and requesting every button would take
  click-drag away from the terminal's own text selection for no benefit until that's built.
  A plain drag still goes to nhpm instead of the terminal once wheel reporting is on (that's
  inherent to the terminal mouse-tracking protocol, not something nhpm can carve exceptions
  into); Shift+drag bypasses it and selects/copies normally, confirmed working in practice.

### 13.3 Screen structure

Each screen is a set of ncurses windows created with `newwin()`:

```
 ┌ header (1 line) ──────────────────────────────────────────────────────────┐
 │ nhpm 0.1 ─ 367-colorful (NetHack 3.6.7) ─ 6/8 on ─ built ✓ ─ index 3d   │
 ├ main list (rest of height minus 9) ──────────────────────────────────────┤
 │ column headers, then rows                                                │
 │ ...                                                                      │
 ├ details (6 lines) ───────────────────────────────────────────────────────┤
 │ description, files touched, warnings                                     │
 ├ status (1 line) ─────────────────────────────────────────────────────────┤
 │ messages / progress bar                                                  │
 └ key bar (1 line) ────────────────────────────────────────────────────────┘
  Tab/Left menu  Enter activate  n New  c Clone  r Rename  x Delete  ...
```

Drawing uses `wnoutrefresh()` on every window and one `doupdate()` per loop, so the screen
updates without flicker.

### 13.4 Main loop

```c
for (;;) {
    draw_current_screen();               /* only windows marked dirty */
    doupdate();
    wint_t ch;
    int r = wget_wch(stdscr, &ch);       /* blocks; timeout() set while busy */
    if (r == KEY_CODE_YES && ch == KEY_RESIZE) { relayout(); continue; }
    if (r == KEY_CODE_YES && ch == KEY_MOUSE) {
        getmouse(&event);                /* wheel only -- a click is still ignored */
        if (event.bstate & BUTTON4_PRESSED) ch = KEY_UP;
        else if (event.bstate & BUTTON5_PRESSED) ch = KEY_DOWN;
        else continue;
    }
    if (global_key(ch)) continue;        /* ?, q, p, Tab/Left, a nav letter */
    screen->handle_key(screen, ch);
}
```

Screens are a table of structs with `enter`, `leave`, `draw`, `handle_key` and `resize`
function pointers, so adding a screen means adding one file.

### 13.5 Widgets (`widgets.c`)

| Widget | Use |
|---|---|
| **List view** | scrolling multi-column list: selection bar, column widths that shrink sensibly, sort indicator, fold/unfold groups, incremental filter |
| **Text view** | scrolling text for details, diffs, build output; search within; color rules per line |
| **Input line** | one-line editor with cursor, Home/End, Ctrl-U/Ctrl-W, history, path Tab-completion |
| **Dialog** | panel-based box: message, buttons, optional list or input; centered, redrawn on resize |
| **Progress** | bar + text in the status line, with Esc to cancel |
| **Key bar** | `htop`-style labels, drawn per screen, clickable with the mouse |

Text is drawn with `mvwaddnwstr()` after converting UTF-8 to `wchar_t` and measuring with
`wcwidth()`, so names with accents or symbols line up in columns.

### 13.6 Long operations

Index refreshes, downloads, quilt operations and builds run in the foreground but keep the
interface alive:

- network: the libcurl progress callback redraws the progress bar and polls for Esc with
  `nodelay()` + `wget_wch()`,
- subprocesses (quilt, patch, make): output is read from pipes with `poll()` and appended to
  the status line or Build screen; Esc sends SIGINT to the child's process group.

No threads are needed, which keeps ncurses use simple and safe (ncurses is not thread-safe).

### 13.7 Handing the terminal to another program

The pager, the editor (conflict fixing), and **playing the game** all need the full terminal:

```c
def_prog_mode();       /* remember ncurses' terminal state */
endwin();              /* give the terminal back */
run_and_wait(argv);    /* less / $EDITOR / nethack */
reset_prog_mode();     /* take it back */
clearok(curscr, TRUE); /* force a full redraw */
refresh();
```

Since NetHack's tty interface is itself a curses-style program, this lets you play an
instance and come straight back to nhpm.

### 13.8 Colors and themes

Color pairs are defined once in `ui.c` by role, not by color:

| Role | Default |
|---|---|
| header / key bar | black on cyan |
| selected row | black on cyan (or reverse video) |
| enabled / applied / `=` | green |
| warning / fuzz / `~` | yellow |
| conflict / error / `!` | red |
| disabled / unknown / `?` | dim / grey |

- `use_default_colors()` keeps the terminal's own background.
- Monochrome terminals get bold/reverse/underline instead.
- `NO_COLOR` turns colors off.
- A `theme` key in `config` can choose between a few built-in themes.

### 13.9 Screens

**Common keys in every list**

| Key | Action |
|---|---|
| ↑ ↓ / k j, PgUp PgDn, Home End, mouse wheel | move |
| Enter | open / details |
| / | incremental search (Esc clears), n / N next/previous |
| s / S | next sort column / reverse |
| ? | help |
| Tab / Left, then a letter | focus the sidebar, then jump to a screen |
| q | quit (asks if something is running) |

Note: the original design's function-key bar (F1-F7/F9/F10) was dropped during
implementation -- not reliably delivered by every terminal/multiplexer/SSH path -- in favor of
plain-letter bindings (`?` help, `q` quit, Tab/Left then a screen's own letter). See
`ui-progress.md` for the full rationale and per-screen detail; this section keeps the screen
names (F2 Instances, etc.) only as stable labels.

**F2: Instances**

```
 Instances
   Name              Version  Patches  Built      Last played
 > 367-colorful      3.6.7    6/8      ✓          today 21:02      (active)
   343-classic       3.4.3    3/3      out of date 3 days ago
   370-dev           3.7.0    0/0      never      never            linked
 ───────────────────────────────────────────────────────────────────────────
  Vanilla plus colors. Good for streaming.
  Source: 3.6.7 (managed)  Recipe: nethack-3.6-linux  Saves: 4 games
 Enter activate  p Play  n New  c Clone  r Rename  x Delete  t Snapshots  e Export  i Import
 C Combined
```

The Snapshots dialog lists snapshots with date and label; **Enter** restores, **x** deletes,
**a** adds a new one.

**/** searches name/version/recipe/notes (plain words, all must match) and hides everything
that doesn't -- there's no J/K-style reordering here, so unlike Patches (13.9 below) a true
filter has no ambiguous "real position" to worry about.

**F3: Database**

```
 Patch Database (3.6.x compatible: 48 of 323)               filter: v:3.6  / candle
   Mk  ID   Name                      Ver    For     Rating  Lib
 >  ~  515  Guiding candle            1.1    3.6.1   -       ✓
    ~  494  Coloured Walls and Floor  v2     3.6.0   -
    =  517  Artifact sacrifice        1.1    3.6.2   -
 ────────────────────────────────────────────────────────────────────────
  Adds a magical candle that detects unseen in its light radius
  Author: ...  Added: ...  Download: external (bilious.alt.org)
 Enter details  d Download  D Download+add to instance  v All versions  R Refresh
```

**F4: Library**

```
 Library (14 patches, 17 versions)
   Name                       Version   For     Used in
 > Color Alchemy              1.2       3.6.0   367-colorful
                              1.1       3.6.0   -
   Dumplog (New)              1.0.0     3.6.0   -
   messagetype option         0.2       3.4.3   343-classic
 ────────────────────────────────────────────────────────────────────────
  Downloaded 2026-11-14 from nhpatchdb.alt.org   touches 4 files
 Enter view  a Add  x Del version  X Del patch  f Fold  u Updates  i Import  m Make from folder
```

**f** collapses the selected row's patch to just its named row (appending `(+)` to the
version column, same as Database's own "all versions off"), persisted as `library_folded`
(10.7). A patch with only one downloaded version has nothing to fold.

**F5: Patches (active instance)**

```
 Patches: 367-colorful (NetHack 3.6.7)                         6 of 8 enabled
   #  On   Patch                     Ver     For     Status
   1  [x]  menucolors                1.5     3.6.x   applied
   2  [x]  dumplog                   1.0.0   3.6.0   applied
   3  [ ]  autosave                  0.1     3.6.1   disabled
 > 4  [x]  color-alchemy             1.2     3.6.0   applied
   5  [x]  guiding-candle            1.1     3.6.1   applied (modified)
   6  [ ]  building                  V1.1    3.6.0   disabled · 2 hunks would fail
 ────────────────────────────────────────────────────────────────────────
  Colored potion mixing results. -p1. Touches: potion.c objects.c
 Space toggle  J/K move  d Diff  c Check  w Capture changes  N New  E Edit  C Combined
```

- **c** dry-runs every disabled patch and fills in the Status column.
- **w** captures your own edits in `src/` into a new patch (12.3), **N** starts a new empty
  patch, **E** edits an existing one (12.4), **o** opens a source file in `$EDITOR`,
  **R** refreshes the patch being edited, **C** makes a combined patch (12.2).
- Other keys: **x** remove, **t** snapshot, **u** undo last change (restores the automatic
  snapshot taken before it). The key bar shows the most used keys; **?** lists all.
- When the working folder has edits not yet captured, the header shows
  `● uncaptured changes` so they aren't forgotten.
- Files touched by more than one enabled patch are flagged in the details area.
- **/** searches id/name/version/target (plain words, all must match) and **highlights**
  matches rather than hiding the rest, since row position here is the real Quilt stack order
  J/K's "one slot" move depends on -- hiding rows could make J/K silently skip past several
  real positions at once. **n** jumps to the next match, wrapping; it's the only way to reach
  one (not an alias for `j` the way Database's own `n` is), since searching doesn't scroll.

**F6: Build**

Streams the recipe's output; `error:` lines red, `warning:` yellow; **e** jumps to the next
error; Esc stops; **b** rebuilds; **s** exports `build.log`. Play (**p**) is on Instances,
once the build has succeeded.

**Changes** (opened by **w** on the Patches screen or **m** in the Library): see 12.5.

**F7: Sources**

```
 Base sources
   Version   Origin                                   Instances
 > 3.6.7     tarball nethack-367-src.tgz              2
   3.4.3     folder ~/src/nh343                       1
 ────────────────────────────────────────────────────────────────────────
 a Add from folder  t Add from tarball  g Add from git tag  x Remove
```

**/** searches version/origin (plain words, all must match) and hides everything that
doesn't, same as Instances above; no `n`/`N` next-match keys, since `n` is already Known
version here and there's no second free letter to pair with it.

### 13.10 Search syntax (Database and Library)

Instances, Patches and Sources also have a `/` search (13.9), but a simpler one: plain words
against their own columns, case-insensitive, all must match -- no tokens, since none of
`v:`/`a:`/`r:`/`id:`/`lib:`/`in:`/`mk:` below mean anything on those screens. This section's
syntax is Database/Library-specific:

Plain words match name, author and description (case-insensitive, all must match), plus:

| Token | Meaning |
|---|---|
| `v:3.6` | target version starts with 3.6 |
| `a:paxed` | author contains "paxed" |
| `r:>=80` | rating at least 80% |
| `lib:yes` / `lib:no` | in the library or not |
| `in:367-colorful` | used by that instance (Library) |
| `id:485` | exact ID |
| `mk:!` (or `mk:=`/`mk:~`/`mk:?`) | by Mk glyph: conflict/exact/fuzzy/unknown (9.4); `mk:conflict` etc. also work |

### 13.11 Minimum size and accessibility

- Minimum 80×24; smaller shows "terminal too small" until resized.
- Everything works from the keyboard; the mouse is optional.
- Status changes are written as text, not only color (`applied`, `CONFLICT`), so the
  interface works without color and with screen readers that read terminal text.

---

## 14. Command-line mode

The same modules without the interface, for scripts and for testing. Every command below is
implemented in `src/main.c`.

```
nhpm                                   start the interface
nhpm refresh                           rebuild the index
nhpm search [query]                    search the index
nhpm info <id>                         patch details
nhpm download <id>                     download into the library
nhpm library [query]                   list the library
nhpm rm <id> [version]                 delete a version or a whole patch

nhpm source add <folder|tarball>       import a base source
nhpm source git <tag>                  import a base source from git
nhpm source list | rm <version>

nhpm inst new <name> --source 3.6.7    create an instance
nhpm inst link <name> <folder>         create a linked instance
nhpm inst list | use <name> | clone <a> <b> | rename <a> <b> | rm <name>
nhpm inst snapshot [label] | snapshots | restore <snapshot>
nhpm inst export <name> [file] | import <file>

nhpm enable <id>[@version]             in the active instance
nhpm disable <id>
nhpm move <id> up|down|<position>
nhpm check | status
nhpm build | play

nhpm combined [--out FILE] [--to-library] [--flatten NEW]
                                          one diff of the active instance's enabled patches
nhpm changes                           list uncaptured edits in the active instance
nhpm capture <name> [--version V] [--files F...] [--desc TEXT]
                                          save edits as a new patch at the top of the stack
nhpm mkpatch <folder> --from <base|folder|tarball|git-ref> <name> [--files F...]
                                          make a patch from any modified NetHack folder
nhpm export-patch <id>[@version] [FILE]
                                          write a library patch as a plain .diff with header
nhpm update-check                        check GitHub for a newer nhpm release (M8)
```

- [x] All of the above subcommands exist and are wired to the same backend the UI uses
      (`cmd_source`, `cmd_inst_*`, etc. in `src/main.c`).
- [x] Common options: `--inst <name>`, `--yes`, `--offline`, `-v`.
- [x] Exit codes: 0 success, 1 usage, 2 network (`NHP_ENET`), 3 conflict (`NHP_ECONFLICT`),
      4 build failed, 5 instance locked (`NHP_ELOCKED`) -- all confirmed returned from the
      relevant command handlers.

### 14.1 Checking for updates (`update.c`)

nhpm-plan.md's M8 milestone item. No JSON/XML dependency: GitHub's
`/<owner>/<repo>/releases/latest` URL 302s to a URL ending in `/releases/tag/<tag>`, so this
just follows that redirect (`net_get()`) and reads the tag back out of the final URL.

- [x] `nhpm update-check` always hits the network (`update_check(app, force=true, ...)`) and
      prints whether a newer release exists, caching the result to
      `$XDG_CACHE_HOME/nhpm/update-check` either way (including "no releases found", so a repo
      with none yet isn't re-checked every run).
- [x] `nhpm --version` prints a one-line notice if the **cache** (not a live check) says a
      newer version is available (`update_check_cached()`), so it stays instant and never
      touches the network itself.
- [x] Config: `check_updates` (default `yes`) and `update_check_max_age_days` (default `3`,
      same shape as `index_max_age_days`). Setting `check_updates = no` makes
      `update_check()` a no-op (`nhpm update-check` still forces one regardless -- it's an
      explicit request).
- [x] `update_version_compare()` compares two `MAJOR.MINOR[.PATCH]`-style strings numerically,
      a leading `v` skipped on either side, missing components reading as `0`.

---

## Appendix A: site reference

Reference HTML snapshots that section 7.3's parser is written against -- not features to
check off, but kept here so a future change in the site's markup can be diffed against what
nhpm currently expects. Checked against the live site, September 2026 (site version
"v0.27", 323 patches). As of this revision, `src/site.c`'s XPath expressions
(`//table[@class='patchbrowse']//tr[td]`, `//table[@class='patchshow']//tr`,
`(//select[@name='pageselect'])[1]/option`) still match the structure below.

**Browse page row:**

```html
<table class="patchbrowse"><tbody>
<tr><th>#</th><th><a href="/?browse&amp;sort=name">Name</a></th>
    <th><a href="/?browse&amp;sort=author">Author</a></th>
    <th><a href="/?browse&amp;sort=ver">For</a></th>
    <th><a href="/?browse&amp;sort=rating">Rating</a></th><th>Short description</th></tr>
<tr class="odd"><td>1</td>
    <td class="xlink" data-href="/?327"><a href="/?327">64bit 1.0.16</a></td>
    <td>Clive Crous</td><td>NetHack 3.4.3</td>
    <td><span class="notrated">Not&nbsp;rated</span></td>
    <td>Fixes NetHack for 64bit</td></tr>
<tr class="even"><td>2</td>
    <td class="xlink" data-href="/?340"><a href="/?340">Acid dropping bug fix 1.0</a></td>
    <td>Patric Mueller</td><td>NetHack 3.4.3</td>
    <td><span class="ratingbar"><span class="rating ratingw100"></span>
        <span class="ratingtext">100%</span></span></td>
    <td>Fixes random crashes when acid drops into water</td></tr>
```

**Page selector:**

```html
<form data-pagejump="1" class="pagejump" method="GET">
  <select name="pageselect">
    <option value="/?browse" selected>Page 1</option>
    <option value="/?browse&amp;page=1">Page 2</option>
    ...
    <option value="/?browse&amp;page=16">Page 17</option>
```

**Search form (POST only):**

```html
<form name="searchform" method="POST" action="/?search">
  <input type="text" name="searchbar" maxlength="30">
  <select name="sort">name | author | ver | rating | added | changed</select>
  <input type="checkbox" name="revsort">
```

The search results page was not examined; nhpm does not depend on it.

**Detail page (`/?485`):**

```html
<table class="patchshow"><tbody>
<tr class="odd"><td class="rowname">Name</td><td>(New) Dumplog 1.0.0</td></tr>
<tr class="odd"><td class="rowname">Rating</td><td><span class="notrated">Not&nbsp;rated</span></td></tr>
<tr class="odd"><td class="rowname">Author</td><td><a href="https://github.com/Nax">Nax</a></td></tr>
<tr class="odd"><td class="rowname">For</td><td>NetHack 3.6.0</td></tr>
<tr class="odd"><td class="rowname">Description</td><td>A rewrite of the famous dumplog patch...</td></tr>
<tr class="odd"><td class="rowname">&nbsp;</td><td>long description with <br> line breaks</td></tr>
<tr class="odd"><td class="rowname">Download</td><td><a href="/?download=485">Get it from us</a> (14.3 Kb)</td></tr>
<tr class="odd"><td class="rowname">Added</td><td>December 22, 2016 22:52</td></tr>
```

**Detail page with an external download (`/?397`):**

```
Download   <a href="https://bilious.alt.org/~paxed/nethack/nh343-msgtype.diff">...</a> (5.2 Kb)
Added      September 22, 2010 11:09
Changed    January 12, 2011 20:16
```

**Comments:** `table.commentshow`, with `td.commentuser`, `td.commentdate`, `td.commenttext`.

**Other facts:** encoding `windows-1252`; served through Cloudflare.
