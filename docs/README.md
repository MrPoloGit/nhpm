# nhpm: User guide

How to install and use nhpm, the terminal patch manager for NetHack. For what nhpm is and why
it exists, see the [root README](../README.md); for how it's built internally, see
[features.md](features.md), [nhpm-plan.md](nhpm-plan.md) and [repo-structure.md](repo-structure.md).

## Contents

1. [Installing](#1-installing)
2. [Concepts](#2-concepts)
3. [Quickstart](#3-quickstart)
4. [The ncurses interface](#4-the-ncurses-interface)
5. [Command-line mode](#5-command-line-mode)
6. [Search syntax (Database and Library)](#6-search-syntax-database-and-library)
7. [Build recipes](#7-build-recipes)
8. [Configuration](#8-configuration)
9. [Where nhpm keeps its files](#9-where-nhpm-keeps-its-files)

---

## 1. Installing

**From a release:** download the `nhpm-<version>-<platform>.tar.gz` asset for your OS
(`linux-x86_64`, `linux-arm64`, `macos-arm64`) from the project's GitHub releases page, untar
it, and put the `nhpm` binary on your `PATH`. A `nhpm-<version>-source.zip` is also attached,
for building from a specific release's source without cloning the repo.

**From source:** needs a C11 compiler, CMake, `ncursesw`+`panel`, `libcurl`, `libxml2`
(headers and libraries), plus `quilt`, `patch`, `make`, `tar`/`gzip`/`unzip`, `diff` and
optionally `git` at runtime.

```sh
make            # build ./nhpm
make install    # optional; PREFIX=/usr/local by default
```

See the [root README](../README.md#building) and [CONTRIBUTING.md](../CONTRIBUTING.md) for
package names and the rest of the development workflow.

## 2. Concepts

| Concept | What it is | Example |
|---|---|---|
| **Index** | Cached list of every patch on the NetHack Patch Database | 323 entries: ID, name, author, target version, rating |
| **Library** | Patches you've downloaded, possibly several versions each | Color Alchemy 1.1 and 1.2 |
| **Base source** | An untouched NetHack source tree for one version, kept read-only | NetHack 3.6.7 |
| **Instance** | A saved, named setup: one base source + an ordered patch list + build settings + its own install and save-game folder | `367-colorful` = 3.6.7 + menucolors + color-alchemy |
| **Snapshot** | A saved copy of an instance's settings at a point in time | "before trying building patch" |
| **Combined patch** | One plain diff made from an instance's enabled patches | `367-colorful-combined.diff` |
| **Local patch** | A patch you made yourself, stored in the library with ID `local-...` | `local-healer-alchemy` 1.0 |

Downloading a patch puts it in the library once. Enabling it in an instance copies it into
that instance's own Quilt stack, so:

- you download a patch once and reuse it across as many instances as you like,
- every instance is independent -- changing or deleting one never affects another,
- deleting something from the library or a base source never breaks an existing instance
  that already copied it.

## 3. Quickstart

A typical first session, building a patched 3.6.7 from source via a git tag (works equally
with `nhpm source add <folder|tarball>` if you already have a NetHack tarball):

```sh
nhpm source git NetHack-3.6.7_Released       # clone and register a base source
nhpm refresh                                  # fetch the patch index (first run only)
nhpm search menucolor                         # find a patch you want
nhpm inst new 367-colorful --source 3.6.7 --recipe nethack-3.6-linux
nhpm enable <id>                              # downloads it (if needed) and enables it
nhpm build                                    # build the active instance
nhpm play                                     # run it
```

Everything above also works from the ncurses interface (just run `nhpm` with no arguments):
Sources (F7) to add the base source, Database (F3) to search and download, Instances (F2) to
create the instance, Patches (F5) to enable the patch, Build (F6) to build, and `p` on
Instances to play.

## 4. The ncurses interface

Run `nhpm` with no arguments. Minimum terminal size is 80x24. `?` opens in-app help with the
same key list below; it's generated from the same source as the screens themselves, so it
never goes stale.

**Global keys**

| Key | Action |
|---|---|
| `Up`/`Down`, `k`/`j`, `PgUp`/`PgDn`, `Home`/`End` | move the selection |
| Mouse wheel | scrolls the same way (no clicking yet) |
| `Tab` or `Left` | focus the sidebar (`Up`/`Down`+`Enter`, or a letter, jumps screens; `Right`/`Enter` returns) |
| `?` | help |
| `q` | quit immediately, on any screen (even mid-build) |

**F2 Instances** -- create, clone, rename, delete, snapshot, export/import, play

| Key | Action |
|---|---|
| `Enter` | make the selected instance active |
| `p` | play the active instance (once it's built) |
| `n` | new instance: name, Managed (from a base source) or Linked (an existing folder), recipe |
| `c` | clone the selected instance |
| `r` | rename the selected instance |
| `x` | delete the selected instance (asks first) |
| `t` | snapshots dialog: `Enter` restores, `x` deletes, `a` adds a new one, `Esc` closes |
| `e` | export to a `.nhinst` file |
| `i` | import a `.nhinst` file as a new instance |
| `C` | combined patch: save to file, and/or add to the library, and/or flatten into a new instance |

**F3 Database** -- browse and search the NetHack Patch Database

| Key | Action |
|---|---|
| `/` | open the search box (see [section 6](#6-search-syntax-database-and-library)) |
| `Enter` or `d` | download the selected patch (replaces an already-downloaded copy) |
| `D` | download it and add it to the active instance's patch stack, enabled |
| `u` | uninstall: remove it from the library |
| `R` | refresh the whole index from the site |
| `s`/`S` | sort by name/author/for/rating/id, `S` reverses |

**F4 Library** -- every downloaded patch and version, independent of any instance

| Key | Action |
|---|---|
| `/` | search |
| `s`/`S` | sort by name/version/for/id, `S` reverses |
| `Enter` | view the selected version's touched files |
| `a` | add the selected version to the active instance's patch stack, enabled |
| `x` | delete the selected version |
| `X` | delete the patch (every downloaded version) |
| `u` | check the site for a newer version |
| `i` | import a local `.diff` that isn't on the site |
| `m` | make a patch from a modified folder (base, another folder, a tarball, or a git ref) |

**F5 Patches** -- the active instance's Quilt stack

| Key | Action |
|---|---|
| `Space` | enable / disable the selected patch |
| `J` / `K` | move the selected patch down / up |
| `d` | show the selected patch's actual applied diff |
| `w` | Changes dialog: review uncaptured edits, `Space` tick, `Enter` full diff, `S` save ticked as a patch |
| `x` | remove the selected patch |
| `c` | dry-run every disabled patch (fills the Status column) |
| `t` | take a manual, labeled snapshot |
| `u` | undo the last Enable/Disable/Move/Add/Remove |
| `N` / `E` / `o` / `R` | new / edit / open file / refresh a patch (see [features.md 12.3-12.4](features.md)) |

A `*` by the column headers means uncaptured edits are sitting in `src/` (`w` to capture them).
The Mk column (shown on Database, Library and Patches) uses `=` exact match, `~` fuzzy, `!`
conflicts, `?` unknown, colored to match.

**F6 Build** -- streams the active instance's recipe output

| Key | Action |
|---|---|
| `b` | build / rebuild; `Esc` while it's running cancels it |
| `e` | jump to the next `error:` line (wraps around) |
| `c` | edit the recipe's custom commands (`custom` recipe only) |
| `s` | export `build.log` to a file you name |
| `Up`/`Down`, `PgUp`/`PgDn`, `Home`/`End` | scroll the output |

`error:`/`warning:` lines are colored red/yellow; play from Instances once a build succeeds.

**F7 Sources** -- base NetHack sources: folder, tarball, git

| Key | Action |
|---|---|
| `a` | add from a local folder |
| `t` | add from a tarball (`.tar.gz`/`.tgz`/`.zip`/...) |
| `g` | add from a git tag (clones NetHack/NetHack) |
| `x` | remove the selected source (asks first; an instance built from it keeps its own copy) |

## 5. Command-line mode

Every feature above also has a CLI command, for scripting or quick one-offs:

```
nhpm [--version] [--help] [--debug]
     --debug (interface mode only): writes a timestamped activity log to
     $XDG_CACHE_HOME/nhpm/ (every status-line message, success or failure);
     its path is printed once you quit
     nhpm refresh
     nhpm search [query]
     nhpm info <id>
     nhpm download <id>
     nhpm library [query]
     nhpm import <file> <name> [author] [target] [version]
     nhpm rm <id> [version] [--yes]

     nhpm source add <folder|tarball> [version]
     nhpm source git <tag>
     nhpm source list
     nhpm source rm <version>

     nhpm inst new <name> --source <version> [--recipe <recipe>]
     nhpm inst link <name> <folder> [recipe]
     nhpm inst list
     nhpm inst use <name>
     nhpm inst clone <name> <new-name> [--with-saves]
     nhpm inst rename <name> <new-name>
     nhpm inst rm <name> [--keep-saves] [--yes]
     nhpm inst snapshot <name> [label]
     nhpm inst snapshots <name>
     nhpm inst restore <name> <snapshot>
     nhpm inst export <name> [file]
     nhpm inst import <file> <new-name> [--source <version>]

     nhpm enable <id>[@version] [--inst <name>]
     nhpm disable <id> [--inst <name>]
     nhpm move <id> up|down|<position> [--inst <name>]
     nhpm undo [--inst <name>]
     nhpm edit-begin <id> [--inst <name>]
     nhpm edit-refresh <id> [--version V] [--name NAME]
             [--author A] [--target T] [--inst <name>]
     nhpm new-patch <name> [--inst <name>]
     nhpm track <path> [--inst <name>]
     nhpm check [--adopt|--restore] [--inst <name>]
     nhpm status [--check] [--inst <name>]
     nhpm build [--inst <name>]
     nhpm play [--inst <name>]

     nhpm combined [--out FILE] [--to-library] [--flatten NEW] [--inst <name>]
     nhpm changes [--hidden] [--inst <name>]
     nhpm capture <name> [--version V] [--author A] [--target T]
             [--desc TEXT] [--long-desc TEXT] [--files F...] [--inst <name>]
     nhpm mkpatch <folder> <name> [--from base|folder:P|tarball:P|git:REF]
             [--version V] [--author A] [--target T] [--desc TEXT]
             [--long-desc TEXT] [--files F...]
     nhpm export-patch <id>[@version] [FILE]
     nhpm update-check

     Run with no arguments to start the ncurses interface.
```

Most mutating commands (`enable`, `disable`, `move`, `undo`, `edit-begin`, `edit-refresh`,
`new-patch`, `track`, `build`, `play`, ...) act on the **active instance** -- the one last
selected with `nhpm inst use <name>` or `nhpm inst new`/`link` -- unless you pass
`--inst <name>` to target a different one. Destructive commands (`rm`, `inst rm`) ask for
confirmation unless you pass `--yes`.

## 6. Search syntax (Database and Library)

Instances, Patches and Sources have a simpler `/` search: plain words against their own
columns, case-insensitive, all must match. The Database and Library screens (and
`nhpm search`/`nhpm library`) support more:

Plain words match name, author and description (case-insensitive, all must match), plus:

| Token | Meaning |
|---|---|
| `v:3.6` | target version starts with 3.6 |
| `a:paxed` | author contains "paxed" |
| `r:>=80` | rating at least 80% |
| `lib:yes` / `lib:no` | in the library or not (Database only) |
| `in:367-colorful` | used by that instance (Library only) |
| `id:485` | exact ID |
| `mk:!` (or `mk:=`/`mk:~`/`mk:?`) | by Mk glyph: conflict/exact/fuzzy/unknown; `mk:conflict` etc. also work |

## 7. Build recipes

Chosen per instance (`nhpm inst new --recipe <recipe>`, or from the Instances screen's `n`):

| Recipe | For | Notes |
|---|---|---|
| `nethack-3.4-unix` | NetHack 3.4.x | |
| `nethack-3.6-linux` | NetHack 3.6.x | |
| `nethack-5.0-linux` | NetHack 5.0.x (what the never-released 3.7 line shipped as) | runs `make fetch-lua` first |
| `custom` | anything else | commands come from `instance.conf`'s `custom_commands` (`;`-separated, `$PREFIX` substituted); set it by hand or with the Build screen's `c` key |

See [features.md section 10.6](features.md#106-building-and-playing-an-instance) for exactly
what each recipe runs and why.

## 8. Configuration

`~/.config/nhpm/config` (created with defaults on first run), a plain `key = value` file:

```ini
index_max_age_days        = 7
request_delay_ms          = 1000
wayback_fallback          = yes
max_download_mb           = 5
check_updates             = yes  # "no" disables nhpm --version's update notice too
update_check_max_age_days = 3
author_name               = Your Name   # default author for patches you make
ignore                    = *.swp *.bak # extra patterns never included in made patches
pager                     = less
editor                    = vi          # $EDITOR wins if set
```

A missing key just uses its default; the file doesn't need to list everything.

## 9. Where nhpm keeps its files

Standard XDG locations:

```
~/.config/nhpm/      config, ui-state (remembered screen/instance/sort/filter)
~/.cache/nhpm/        index cache, cached detail pages, logs -- safe to delete
~/.local/share/nhpm/  library/, sources/, instances/, exports/
```

See [file-formats.md](file-formats.md) for the exact layout and file formats.
