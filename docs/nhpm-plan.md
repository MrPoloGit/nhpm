# nhpm: Development plan

A terminal patch manager for NetHack, written in C with an **ncurses** interface in the
style of `htop` / `menuconfig`, sitting on top of **Quilt**.

It downloads patches from the NetHack Patch Database into a local library, and lets you
create saved **instances**: named NetHack setups, each with its own version, its own
ordered list of patches, its own build and its own save games. You can switch between
instances, clone them, snapshot and restore them, export them to share, and launch the game
from inside the app.

It also makes patches: it can turn an instance into **one combined patch** that anyone can
apply to clean NetHack source, and turn **your own changes** to the NetHack code into a
proper patch, whether you made them inside an instance or in any NetHack folder.

---

## Reference documents

The functional spec, source layout, code style, file formats, security and testing details
have moved out of this plan into their own documents:

| Document | Covers |
|---|---|
| [features.md](features.md) | Goals and non-goals, concepts, the Patch Database, the patch library, base sources, instances, Quilt integration, making patches, the ncurses interface, command-line mode, and the site HTML reference |
| [repo-structure.md](repo-structure.md) | Source layout and module boundaries |
| [style.md](style.md) | Code style and conventions |
| [file-formats.md](file-formats.md) | Files on disk (XDG layout) and file formats |
| [security.md](security.md) | Security considerations |
| [testing.md](testing.md) | Unit, integration, interface and real-world tests |
| [tools-libraries.md](tools-libraries.md) | What to install, to build nhpm and to actually use it, per platform |

This document keeps the build/dependencies overview, error handling, milestones, risks and
decisions.

---

## Contents

1. [Dependencies and build](#1-dependencies-and-build)
2. [Error handling](#2-error-handling)
3. [Milestones](#3-milestones)
4. [Risks](#4-risks)
5. [Decisions](#5-decisions)

---

## 1. Dependencies and build

What to install, per platform, both to build nhpm and to actually use it, has moved to
[tools-libraries.md](tools-libraries.md). In short: a C11 compiler, ncursesw+panel, libcurl
and libxml2 (dev headers) to build; `quilt`, GNU patch, tar/gzip/unzip, GNU diff, and
optionally git and a NetHack-side C toolchain, at runtime.

On macOS, Apple's bundled ncurses is old; the build uses Homebrew's `ncurses` through
`pkg-config` when available. Apple's bundled `patch` is also not GNU patch
([tools-libraries.md](tools-libraries.md), section 3).

At startup nhpm checks that `quilt` and `patch` are on `PATH` and explains what to install
if not.

### Build system

This sketch shows the shape of the logic rather than literal file contents: the actual
Makefile now shells out to a top-level `CMakeLists.txt` for everything but `test`/`format`/
`lint` (docs/style.md sections 8 and 11), which is where `SRC`/`OBJ`/`pkg-config` etc. below
actually live today.

```make
PKGS    = ncursesw panelw libcurl libxml-2.0
WARN    = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
          -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings -Wnull-dereference -Wdouble-promotion
CFLAGS += -std=c11 $(WARN) -O2 $(shell pkg-config --cflags $(PKGS))
LDLIBS += $(shell pkg-config --libs $(PKGS))

SRC = $(wildcard src/*.c src/ui/*.c)
OBJ = $(SRC:.c=.o)

nhpm: $(OBJ)
	$(CC) -o $@ $^ $(LDLIBS)

debug: CFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDLIBS += -fsanitize=address,undefined
debug: nhpm

test:    ...      # see testing.md, section 17
format:  ; clang-format -i src/*.[ch] src/ui/*.[ch] tests/*.c
lint:    ...      # see style.md, section 6.13
install: nhpm
	install -Dm755 nhpm $(DESTDIR)$(PREFIX)/bin/nhpm
	install -Dm644 doc/nhpm.1 $(DESTDIR)$(PREFIX)/share/man/man1/nhpm.1
```

`_XOPEN_SOURCE 700`, defined in `src/feature.h` (style.md, section 6.1), enables the wide-character
ncurses functions (`add_wch`, `mvaddwstr`, `get_wch`) along with the POSIX functions used. If `pkg-config` names differ (`ncursesw` vs `ncurses`), the Makefile
falls back to `-lncursesw -lpanelw`.

The real Makefile also adds `-MMD -MP` to `CFLAGS`: alongside each `.o`, this writes a `.d`
file listing the headers that translation unit actually included, so editing a header
rebuilds every `.c` that depends on it, not just the ones that `#include` it directly.
Without this, `make` after a header-only change can link some translation units against the
old layout and others against the new one -- same struct, different member offsets per
`.o` -- which doesn't fail to build; it produces a binary that reads its own data at the
wrong offsets and corrupts or crashes at runtime, far from the edit that caused it.

---

## 2. Error handling

| Situation | Behavior |
|---|---|
| No network / site down | everything local works; network actions say "site not reachable" |
| Cloudflare challenge | "site is blocking automated requests right now, try later" |
| Site layout changed | parser finds no rows, shows "couldn't read the site's pages; nhpm may need an update" |
| Dead download link | Wayback fallback, then mark "unavailable" and show links from comments |
| Download isn't a diff | rejected with a clear message |
| quilt/patch missing | explained at startup; instance changes disabled |
| Operation fails halfway | automatic rollback; if rollback fails, stop and show the exact quilt output and the manual fix |
| `instance.conf` unreadable | offer `instance.conf.bak`, then the latest snapshot |
| Instance folder and file disagree | Adopt / Restore / read-only (features.md, section 10.4) |
| Instance locked by another nhpm | open read-only, show the PID |
| Disk full | atomic saves mean old files stay intact; the operation is rolled back |
| Build fails | log kept in `build.log`; instance marked "build failed"; previous install left in place |
| Combined patch fails verification | file is not handed out; error shows which patch and file differ (indicates an nhpm bug) |
| Capture fails partway | snapshot and file backup restored; nothing saved to the library |
| Nothing changed when capturing | "no changes found" (with a hint to press `h` if files were hidden as build output) |

---

## 3. Milestones

| # | Milestone | Contents | Done when | Status |
|---|---|---|---|---|
| **M0** | Skeleton | Makefile, `feature.h`, modules, XDG paths, INI/TSV reader-writer, atomic save, `nhp_err` + `err_*`, logging, `proc.c`, test runner; style tooling (`.clang-format`, `.clang-tidy`, `.editorconfig`, `make format`, `make lint`), `CONTRIBUTING.md`, CI | `make test` and `make lint` pass in CI on GCC and Clang; `nhpm --version` | Done |
| **M1** | Site access | `net.c`, `site.c`, `index.c`; `refresh`, `search`, `info` | full index from the real site; parser tests pass | Done |
| **M2** | Library | downloads (hosted, external, Wayback, archives), diff check, versions, delete, import; `download`, `library`, `rm` | real patches downloaded; tests pass | Done |
| **M3** | Sources + instances | `version.c`, `sources.c`, `instance.c`: base sources, new/clone/rename/delete, `instance.conf`, autosave, snapshots, export/import, locking, load-time checks | instance tests pass, including crash recovery | Done |
| **M4** | Quilt stack | `quilt.c`, `stack.c`: strip detection, enable/disable/move with rollback, conflicts | all integration tests pass | Done |
| **M5** | Build and play | recipes verified against real 3.4.3 / 3.6.7 (3.7 never released -- it shipped as 5.0.0, which no patch targets yet), per-instance install, `build`, `play` | real patched instances build and run | Done |
| **M6** *(optional extension)* | Making patches | `mkpatch.c`: ignore rules (per-version lists with the recipes), combined patch with verification, capture changes, new/edit/refresh own patches, patch from folder and from git, export with header; `combined`, `changes`, `capture`, `mkpatch`, `export-patch` | making-patches integration tests pass; a combined patch of a real 3.6.7 instance applies to clean source | Done |
| **M7** | ncurses interface | `ui.c`, widgets, all screens, dialogs, search syntax, progress/cancel, terminal hand-off, mouse, themes; the Changes screen only if M6 has been built | everything from M1-M5 (and M6, if built) doable in the interface; tmux smoke test and manual checklist pass | In progress, see [ui-progress.md](ui-progress.md) |
| **M8** | Polish + release | man page, README with screenshots, `make install`, update checks, saved UI state | tagged 0.1 | Not started |

M1-M5 have no interface, which makes the risky parts (site parsing, instance saving, Quilt
rollback) solid first; M7 is then a front end over tested code. M6 (making patches) is a
genuinely separate feature -- consuming patches (M1-M5) works completely without it -- so it
can be built whenever it's wanted, before or after the interface, without blocking either. If
you'd rather see the interface early, a bare-bones version of M7 (Instances and Patches
screens only) can move up to right after M4.

### Later ideas

- Variant-aware compatibility.
- GitHub repos or gists as patch sources.
- Export an instance's stack as a git branch (one commit per patch).
- Combine patches with `combinediff` (patchutils) as an alternative method.
- Background index refresh.
- Per-instance `.nethackrc` management.

---

## 4. Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Site HTML changes | medium | parsing isolated in `site.c`; fixtures; clear error; cached index keeps working |
| Cloudflare blocks non-browser clients | low to medium | slow requests, real User-Agent; manual import fallback |
| Many external links dead | high | Wayback fallback, links from comments, manual import |
| Old patches don't apply to new versions | high | compatibility marks, dry runs, conflict flow, snapshots |
| Build output leaks into made patches | medium | per-version ignore lists, binary skip, new files unticked by default, file-by-file review, `h` to see hidden files |
| NetHack build setups differ by version | high | recipes in one place, verified in M5, `custom` recipe |
| Instance file and folder drift apart | medium | hashes, load-time check, Adopt/Restore, `.bak`, snapshots |
| Half-patched state after crash | low | Quilt first, save second; load-time repair; tests with SIGKILL |
| Terminal left broken after a crash | low | `endwin()` in `atexit` and signal handlers |
| Quilt output differs between versions | low | `LC_ALL=C`; exit codes first; test against Debian, Fedora and Homebrew quilt |

---

## 5. Decisions

1. **Saving instances** means saved, named NetHack setups ([features.md](features.md)
   section 10: version, patch list, build and save games), saved automatically with
   snapshots and export.
2. **Deleting versions** means deleting downloaded versions of a patch (features.md,
   section 8).
3. **Supported NetHack versions:** vanilla 3.4.3 and 3.6.x first, where essentially every
   real patch in the Database targets. The 3.7 line never had its own release; it shipped
   directly as 5.0.0, which no patch targets yet, so supporting it is future work once
   patches for it exist.
4. **Generated patch format:** unified diff applied with `-p1` (the common modern choice).
   A `-p0` option could be added later to match older Patch Database patches, which often
   use `-p0` or context diffs.
5. **Save games are per instance**, not shared between instances of the same NetHack
   version, because patched builds can make save files incompatible.
6. **License:** the [NetHack General Public License](../LICENSE) (NGPL), the same one
   NetHack itself uses.
7. **Platforms:** Linux and macOS first; *BSD likely; Windows via WSL.
