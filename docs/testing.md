# nhpm: Testing

Part of the [nhpm development plan](nhpm-plan.md). See [features.md](features.md) for the
instance/quilt/patch-making behavior under test, and [style.md](style.md) for the lint and
formatting tooling (`make lint`) referenced below.

---

## 17. Testing

### 17.1 Unit tests (no network, no terminal)

| Area | Tests |
|---|---|
| `site.c` | saved browse page, hosted-download detail page (485), external-download page (397), page with comments, empty page |
| Name/version split | `(New) Dumplog 1.0.0`, `Additional Item Categories v5.2`, `Building V1.1`, `initpoly`, `Coloured Walls and Floor v2` |
| `version.c` | `NetHack 3.6.0`; `patchlevel.h` from 3.4.3, 3.6.7, 3.7; compatibility marks |
| `index.c` | TSV escaping round-trip; every search token |
| `config.c` | INI reader/writer round-trip, repeated sections, comments, bad input |
| `instance.c` | create/load/save round-trip; `.bak` handling; format upgrade; export/import round-trip; name validation |
| Diff detection | unified, context, plain, HTML, empty, CRLF |
| Path checks | `../etc/passwd`, `/usr/include/x.h`, `a/src/../../x` rejected |
| Sanitizer | `\x1b[2J`, `\x07`, other control characters |
| `mkpatch.c` | ignore patterns (general and per-version); binary detection; new-file default ticks; header writing; deterministic output (same input gives a byte-identical diff) |

### 17.2 Integration tests (need quilt and patch)

A tiny fake NetHack source in `tests/fixtures/source/` (a `patchlevel.h` and a few small C
files, plus a trivial Makefile and a "game" that prints a line) and hand-made patches that
apply cleanly, need different `-p` levels, conflict, and depend on each other.

Scripted through the command-line mode:

- create instance, enable/disable/move, check `instance.conf`, `series`, `quilt applied` and
  file contents after every step,
- forced failures must leave the folder **and** `instance.conf` identical to before
  (checksums of everything),
- kill nhpm mid-operation (`SIGKILL`) and check the next start detects and repairs it
  ([features.md](features.md), section 10.4),
- snapshot, change, restore,
- clone, rename, delete (with and without keeping saves),
- export, then import into a fresh data folder,
- build and play with the fake game,
- **combined patch:** for an instance with several patches, the combined diff applies with
  `patch -p1` to clean source and the result is byte-identical to the instance's applied
  source (excluding ignored files); disabled patches are not included,
- **capture:** edit files in an instance (including after a build, so build output exists),
  capture, and check the new patch contains exactly the edits, the folder is unchanged,
  `quilt applied` shows the new patch on top, and the library has `local-...` 1.0,
- **edit patch:** change a local patch in the middle of the stack, refresh, check version
  1.1 exists alongside 1.0 and later patches still apply,
- **from folder / git:** make a patch from a modified copy of the fake source and from a git
  repository with a tag, and check both give the same diff.

### 17.3 Interface tests

- Unit tests for widgets that don't need a terminal (column layout, truncation with wide
  characters, filtering).
- A scripted smoke test running nhpm inside `tmux` (or a pty via `script`), sending keys
  and checking the captured screen text for expected content.
- Manual checklist before each release: resize while scrolled, 80x24, `NO_COLOR`, monochrome
  `TERM=vt100`, Esc during refresh/build, editor round-trip, play and return, mouse clicks.

### 17.4 Real-world tests (manual, before release)

Real NetHack 3.4.3 and 3.6.7 sources (3.7 never released -- see docs/nhpm-plan.md section 5);
a handful of real patches; build and play
each instance.

### 17.5 Tooling

`make debug` (ASan/UBSan) for all tests, `valgrind` on CLI tests, `make lint` (formatting,
naming, banned functions; [style.md](style.md) section 6.13), CI on Ubuntu with GCC and Clang
running lint, unit and integration tests. CI never contacts the real site.

---
