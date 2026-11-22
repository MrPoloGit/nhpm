# nhpm: Code style and conventions

Part of the [nhpm development plan](nhpm-plan.md). See [repo-structure.md](repo-structure.md)
for module layout and [file-formats.md](file-formats.md) for the on-disk file formats
referenced below.

---

## 6. Code style and conventions

These rules apply to all code in the project. Most formatting is enforced by tools
(6.13), so reviews can focus on the rest.

### 6.1 Language and standards

- **C11** (`-std=c11`) with **POSIX.1-2008 / XSI** functions:
  `#define _XOPEN_SOURCE 700` at the top of every `.c` file (before any `#include`), which
  also enables the wide-character ncurses functions. Put it in one shared header,
  `src/feature.h`, included first.
- No compiler extensions (`__attribute__`, statement expressions, nested functions) except
  behind a macro in `util.h` with a portable fallback, e.g. `NHP_PRINTF(fmt, args)` for
  printf-format checking.
- No variable-length arrays, no `alloca`.
- Must compile without warnings on current GCC and Clang with:

  ```
  -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings -Wnull-dereference -Wdouble-promotion
  ```

  CI adds `-Werror`. Local builds don't, so a new compiler warning never blocks someone.

### 6.2 Formatting

| Rule | Choice |
|---|---|
| Indentation | 4 spaces, never tabs (except in Makefiles) |
| Line length | 100 columns |
| Braces | K&R: function bodies open on their own line; `if`/`for`/`while`/`switch`/`struct` open on the same line |
| Braces always | yes, even around a single statement |
| Pointer `*` | next to the name: `char *name`, `const char *path` |
| Declarations | one per line; declare at first use, not all at the top |
| Spaces | after keywords (`if (x)`), around binary operators, none inside parentheses, none between a function name and `(` |
| `switch` | `case` labels indented one level; every `switch` on an enum lists all values or has `default:`; fall-through marked `/* fallthrough */` |
| Line endings / encoding | LF, UTF-8, file ends with one newline, no trailing spaces |
| Blank lines | one between functions; at most one inside a function to separate steps |

Example:

```c
static nhp_err lib_remove_version_dir(const char *patch_dir, const char *version)
{
    char *dir = path_join(patch_dir, version);
    if (dir == NULL) {
        return NHP_ENOMEM;
    }

    nhp_err err = fs_remove_tree(dir);
    if (err != NHP_OK) {
        err_set("could not delete %s", dir);
    }

    free(dir);
    return err;
}
```

### 6.3 Naming

**General:** names are English, descriptive, and in `snake_case`, except constants and
macros (`UPPER_SNAKE_CASE`). No Hungarian notation, no abbreviations beyond the standard ones
below.

| Kind | Style | Examples |
|---|---|---|
| Files | lowercase, `snake_case`, named after the module | `library.c`, `library.h`, `ui/scr_patches.c`, `tests/test_library.c` |
| Public functions | `<prefix>_<verb>_<object>` | `lib_delete_version()`, `inst_save()`, `stack_enable()`, `site_parse_browse_page()` |
| Static (file-local) functions | `snake_case`, no prefix needed | `parse_rating()`, `draw_key_bar()` |
| Variables, parameters, struct fields | `snake_case` | `patch_count`, `strip_level`, `base_source` |
| Struct and enum types | `snake_case`, module prefix, typedef'd to the same name | `typedef struct inst_patch inst_patch;` |
| Enum values | `UPPER_SNAKE_CASE` with the type's prefix | `STACK_APPLIED`, `STACK_DISABLED`, `STACK_CONFLICT` |
| Macros | `UPPER_SNAKE_CASE`, `NHP_` prefix if public | `NHPM_VERSION`, `NHP_ARRAY_LEN(a)` |
| Compile-time constants | `enum` value or macro, `UPPER_SNAKE_CASE` | `NHP_DEFAULT_DELAY_MS`, `INDEX_MAX_AGE_DAYS` |
| `const` objects (tables, strings) | `static const`, `snake_case` | `static const char *const default_ignore_patterns[]` |
| Callback types | `snake_case` ending in `_fn` | `net_progress_fn`, `proc_line_fn` |
| Header guards | `NHPM_<FILE>_H` | `NHPM_LIBRARY_H` |

**Module prefixes** (every public function and type starts with one):

| Module | Prefix | Module | Prefix |
|---|---|---|---|
| `config.c` | `cfg_` | `version.c` | `ver_` |
| `util.c` | `str_`, `buf_`, `path_`, `fs_`, `err_`, `log_` | `sources.c` | `bsrc_` (base source) |
| `proc.c` | `proc_` | `instance.c` | `inst_` |
| `net.c` | `net_` | `quilt.c` | `quilt_` |
| `site.c` | `site_` | `stack.c` | `stack_` |
| `index.c` | `idx_` | `build.c` | `build_` |
| `library.c` | `lib_` | `mkpatch.c` | `mkp_` |
| `ui/ui.c` | `ui_` | `ui/widgets.c` | `wid_` |
| `ui/scr_<name>.c` | `scr_<name>_` | tests | `test_<module>_<what>` |

(`idx_` rather than `index_` because `index()` is an old POSIX function.)

**Name meanings that are always used the same way:**

| Pattern | Meaning |
|---|---|
| `*_count` | number of items (`patch_count`) |
| `*_len` | length of a string in bytes, without the NUL |
| `*_size` | capacity of a buffer in bytes |
| `*_ms`, `*_bytes`, `*_kb`, `*_days` | units are always in the name when a number has a unit |
| `is_*`, `has_*`, `can_*` | `bool` values and functions returning `bool` (`is_enabled`, `has_conflict`) |
| `*_new` / `*_free` | allocate / release an object; `*_free(NULL)` does nothing |
| `*_load` / `*_save` | read from / write to disk |
| `*_parse` / `*_format` | text to struct / struct to text, no I/O |
| `*_dup` | returns a newly allocated copy the caller frees |
| `*_find` | returns a pointer into an existing structure, or `NULL`; caller doesn't free |
| `i`, `j`, `n` | loop indices only; everything else gets a real name |

**Globals:** no mutable globals, except the application state struct in `main.c` and
ncurses' own state in `ui/`. File-local mutable state is `static` and documented at its
declaration. State is passed explicitly (`app_state *app` parameter) rather than reached
through globals.

### 6.4 Files and headers

- One module = one `.c` + one `.h`. A header declares only what other modules use;
  everything else in the `.c` is `static`.
- Every header is **self-contained** (compiles on its own) and has an include guard:

  ```c
  #ifndef NHPM_LIBRARY_H
  #define NHPM_LIBRARY_H
  ...
  #endif /* NHPM_LIBRARY_H */
  ```

- **Include order** in a `.c` file, blank line between groups:
  1. `"feature.h"`
  2. the module's own header (`"library.h"`), which proves it's self-contained
  3. C and POSIX headers (`<stdio.h>`, `<unistd.h>`), alphabetical
  4. third-party headers (`<curl/curl.h>`, `<libxml/HTMLparser.h>`, `<curses.h>`)
  5. other project headers, alphabetical
- Only files in `src/ui/` include `<curses.h>`; only `net.c` includes `<curl/curl.h>`; only
  `site.c` includes libxml2 headers.
- Use opaque types (`typedef struct inst inst;` in the header, full definition in the
  `.c`) when other modules don't need the fields.
- Every file starts with a one-line summary comment and the license line:

  ```c
  /* library.c - downloaded patches: store, list, delete versions, verify.
   * SPDX-License-Identifier: <license> */
  ```

- Guideline sizes: files under ~800 lines, functions under ~60 lines. Longer is fine when
  splitting would make it harder to read, but it's a hint to split.

### 6.5 Types

- `bool` from `<stdbool.h>` for true/false values, never `int` flags.
- `size_t` for sizes, lengths, counts and indices; `ssize_t` only where POSIX returns it.
- Fixed-width types (`uint8_t`, `int64_t`) only when the width matters (hashes, file formats).
- `const` everywhere it applies: pointer parameters the function doesn't modify are
  `const T *`; string literals are `const char *`.
- Enums for every set of named states (patch status, screen IDs, compatibility marks), never
  bare integers or strings.
- No casts to silence warnings; fix the type instead. Casting `malloc`'s result is not done.

### 6.6 Errors

- Functions that can fail return `nhp_err`, an enum where `NHP_OK` is `0` and errors are
  negative (`NHP_ENOMEM`, `NHP_EIO`, `NHP_ENET`, `NHP_EPARSE`, `NHP_ECONFLICT`, `NHP_ELOCKED`,
  `NHP_ECANCELED`, ...). Results come back through pointer parameters.
- Functions that return a pointer return `NULL` on failure.
- A human-readable message for the last error is set with `err_set(fmt, ...)` and read with
  `err_get()`, so the UI and CLI can show it. Messages say what failed and with what:
  `"could not read /path/instance.conf: Permission denied"`.
- Every return value that can signal failure is checked (`malloc`, `fopen`, `write`,
  `close`, `rename`, every `nhp_err`). `-Wunused-result` style warnings are fixed, not cast
  away.
- Functions with several resources use a single exit with `goto cleanup`:

  ```c
  nhp_err inst_export(const inst *in, const char *out_path)
  {
      nhp_err err = NHP_OK;
      char *tmp_dir = NULL;
      FILE *manifest = NULL;

      tmp_dir = fs_make_temp_dir("nhinst");
      if (tmp_dir == NULL) {
          err = NHP_EIO;
          goto cleanup;
      }
      ...
  cleanup:
      if (manifest != NULL) {
          fclose(manifest);
      }
      fs_remove_tree(tmp_dir);
      free(tmp_dir);
      return err;
  }
  ```

- Only `main()` calls `exit()`. Library code never prints to stdout/stderr directly; it
  logs (6.9) and returns errors.
- `assert()` only for programmer errors (impossible states), never for input or I/O checks.

### 6.7 Memory and ownership

- Whoever allocates documents who frees. By default: anything returned by `*_new`, `*_load`,
  `*_dup`, `*_format` is owned by the caller and freed with the matching `*_free` (or
  `free()` for plain strings).
- Every `*_free` accepts `NULL`.
- After freeing a pointer that stays in scope, set it to `NULL`.
- Dynamic arrays and strings use the helpers in `util.c` (`buf_*` growable byte buffer,
  `str_*` string helpers, a small typed vector macro), not ad-hoc `realloc` code.
- No fixed-size `PATH_MAX` buffers for paths; paths are allocated with `path_join()`.
- Strings are UTF-8 `char *` everywhere inside the program. Conversion to `wchar_t` happens
  only at the ncurses boundary in `ui/`.
- All tests run under AddressSanitizer, and the CLI tests under Valgrind, with zero leaks
  reported.

### 6.8 Banned and preferred functions

| Don't use | Use instead | Why |
|---|---|---|
| `strcpy`, `strcat`, `sprintf`, `gets` | `snprintf`, `str_dup`, `buf_append` | buffer overflows |
| `strtok` | `strtok_r` or a small hand-written splitter | hidden global state |
| `atoi`, `atol` | `strtol` / `strtoul` with error and range checks | no error reporting |
| `system`, `popen` | `proc_run()` (fork/exec with an argument list) | shell injection |
| `tmpnam`, `mktemp` | `mkstemp`, `mkdtemp` (via `fs_make_temp_*`) | races |
| writing a file in place | `fs_write_atomic()` (temp file, fsync, rename) | half-written files ([file-formats.md](file-formats.md), section 4) |
| `printf` in library code | `log_*`, `err_set` | the UI owns the terminal |

`clang-tidy` and a grep check in `make lint` flag the banned functions.

### 6.9 Logging

- `log_debug`, `log_info`, `log_warn`, `log_error` macros (printf-style, format-checked) write
  to `~/.cache/nhpm/nhpm.log` with a timestamp and module name.
- Every external command run (quilt, patch, make, git) is logged with its arguments and exit
  code; network requests with URL and HTTP status. Personal data (such as the configured
  author name or email) is never logged.
- `-v` on the command line or `log_level = debug` in `config` shows debug lines.

### 6.10 Comments and documentation

- Every public function has a comment in its header saying what it does, what each
  parameter means, what it returns, and who owns returned memory:

  ```c
  /* Delete one downloaded version of a library patch.
   * Instances that use it keep their own copy and are not affected.
   * Returns NHP_OK, or NHP_ENOENT if the version doesn't exist, or NHP_EIO. */
  nhp_err lib_delete_version(lib *library, const char *patch_id, const char *version);
  ```

- Comments explain **why**, not what the next line does. Anything surprising (a site quirk,
  a Quilt behavior, a NetHack build oddity) gets a comment with the reason.
- No comment is longer than 3 lines, including a function's own header comment. If the
  reason needs more than that, it belongs in docs/, not in the source.
- `/* ... */` for comments above code; `//` allowed for short end-of-line notes.
- `TODO(name):` and `FIXME:` markers include a short reason; open ones are listed in CI
  output so they don't pile up.
- User-facing documentation (README, man page, `F1` help) is updated in the same change
  as the feature.

### 6.11 Text the user sees

- Use the same terms everywhere: **instance**, **base source**, **library**, **patch**,
  **combined patch**, **snapshot**. Never mix in synonyms ("profile", "tree", "mod").
- Labels and titles in sentence case: "Delete version", not "Delete Version".
- Messages say what happened and what to do next:
  "Couldn't download the patch: the link is dead. Press `Enter` to try the Wayback Machine."
- No exclamation marks, no jokes in error messages, no blame ("invalid input" should instead
  say what is expected).
- Key names written as they appear on the key bar: `F5`, `Space`, `Esc`, `Enter`.
- All UI strings live in the screen files (not scattered in logic modules), so wording can
  be reviewed in one place and translated later if wanted.

### 6.12 Data file conventions

- Keys in `config`, `instance.conf` and `*.meta` files are `lower_snake_case`.
- Timestamps are ISO 8601 in UTC: `2026-11-15T21:40:12Z`.
- Sizes on disk are stored in bytes; only the UI converts to KB/MB.
- Booleans are written `yes` / `no`; the reader also accepts `true`/`false`/`1`/`0`.
- Every file format nhpm owns has a `format = N` key (or header line) so it can be
  upgraded later.

### 6.13 Tools that enforce this

| File / target | What it does |
|---|---|
| `.clang-format` | formatting rules from 6.2 |
| `.clang-tidy` | `bugprone-*`, `cert-*`, `clang-analyzer-*`, `misc-*`, `readability-identifier-naming` (enforces 6.3), `readability-function-size`; noisy checks turned off explicitly |
| `.editorconfig` | spaces, LF, final newline, tabs only in Makefiles, 2-space indent for YAML, for editors |
| `make format` | runs `clang-format -i` on all sources |
| `make lint` | `clang-format --dry-run --Werror`, `clang-tidy`, `cppcheck`, banned-function grep |
| CI | runs `make lint`, builds with `-Werror` on GCC and Clang, runs all tests under sanitizers |

Starting `.clang-format`:

```yaml
BasedOnStyle: LLVM
IndentWidth: 4
UseTab: Never
ColumnLimit: 100
BreakBeforeBraces: Linux          # function braces on their own line
AllowShortIfStatementsOnASingleLine: Never
AllowShortFunctionsOnASingleLine: None
AllowShortLoopsOnASingleLine: false
InsertBraces: true
PointerAlignment: Right           # char *name
IndentCaseLabels: true
SortIncludes: Never               # include order is set by 6.4, not alphabetically overall
SpaceBeforeParens: ControlStatements
AlignConsecutiveMacros: Consecutive
```

### 6.14 Commits and changes

- Each commit builds, passes `make lint` and `make test`.
- Commit messages: a short imperative summary line (≤ 72 characters), blank line, then why
  the change was made. Prefix with the module when useful: `stack: roll back when a later
  push fails`.
- One logical change per commit; formatting-only changes go in their own commits.
- New behavior comes with tests; bug fixes come with a test that failed before the fix.
- `CHANGELOG.md` gets a line for every user-visible change.

The sections below cover every other file type actually tracked in the repo. Captured
test data under `tests/fixtures/` (`.html` site snapshots, `.diff` patches, the `.tar.gz`
and the fake NetHack `source*/` trees) is deliberately exempt: it represents *other*
systems' output (a website, a patch author, upstream NetHack), not nhpm's own code, so
holding it to nhpm's conventions would defeat its purpose as a fixture.

---

## 7. Shell scripts

The only shell script in the repo proper is `tests/run.sh`; keep any new one this small and
this plain.

- `#!/bin/sh`, and POSIX-only syntax -- no bash arrays, `[[ ]]`, or other bashisms. These are
  thin wrappers, not programs; they stay portable to `dash`.
- `set -eu` as the first line after the shebang/header comment, so a failing command or an
  unset variable stops the script instead of continuing on bad state.
- Same file-header comment as C (6.4): a one-line summary of what the script does and why it
  exists, right after the shebang.
- Same 3-line comment cap as 6.10: a `#` comment longer than that moves its reasoning to
  docs/ and leaves a pointer behind.
- Quote every expansion (`"$var"`, `"$@"`); unquoted expansions are the classic source of
  word-splitting and glob bugs.
- Formatting otherwise follows `.editorconfig`'s defaults: 4-space indentation, no tabs, LF,
  final newline.
- A script is a wrapper around `make` or another already-correct command, not a place to
  reimplement logic that belongs in `Makefile` or `src/`:

  ```sh
  #!/bin/sh
  # run.sh - builds and runs the test suite. A thin wrapper around `make test`
  # so the tests can be run directly, e.g. from CI or an editor, without
  # remembering the make target's name.
  set -eu
  cd "$(dirname "$0")/.."
  exec make test
  ```

---

## 8. Python scripts

Dev tooling under `scripts/` (currently `find_long_comments.py`) is Python, not shell, when
the logic is easier to express that way (parsing, not just chaining commands).

- Python 3, **standard library only** -- no third-party dependencies and no
  `requirements.txt`, so a script runs on any machine with `python3` and nothing else
  installed.
- PEP 8: 4-space indentation, `snake_case` functions, a module-level docstring with a
  `Usage:` line.
- A one-line docstring on any function whose purpose isn't obvious from its name; no
  docstring at all is also fine for a short, obviously-named helper.
- `if __name__ == "__main__": sys.exit(main())`, so the script is directly runnable and
  returns a real exit status for shell/CI callers.
- No stray `print()` debugging left in; the only output is the tool's actual job (e.g. the
  comments `find_long_comments.py` found), so it composes cleanly in a pipeline or `make`
  target.
- No formatter or linter is wired up for these yet (`make lint` only covers `src/`/`tests/`
  C code); match the existing script's style by eye until one is added.
- Same 3-line cap as 6.10 on an inline `#` comment (a docstring is not a comment and isn't
  bound by this -- it can run as long as it needs to).

---

## 9. Makefiles

Covers the top-level `Makefile`; `tests/fixtures/source/Makefile` is fixture data and this
section doesn't apply to it (see the note above).

- Tabs for recipe lines, spaces everywhere else -- required by `make` itself, and the only
  exception `.editorconfig`'s `[Makefile]` section carves out from its normal spaces-only
  rule.
- Variables a caller might reasonably override (`CC`, `PREFIX`, `DESTDIR`, `VERSION`,
  `CFLAGS`) use `?=`/`override ... +=`, never a bare `=`, so `make CC=clang` or
  `make VERSION=v1.2.3` works without editing the file.
- Internal, derived variables (`PKG_CFLAGS`, `OBJ`, `SRC`) use `:=` (immediate expansion),
  not `=`, so a `$(shell ...)` call runs once at parse time, not once per reference.
- Source lists come from `$(wildcard src/*.c src/ui/*.c)` and pattern rules
  (`$(OBJDIR)/%.o: src/%.c`), not hand-maintained file lists, so adding a new `.c` file never
  needs a Makefile edit.
- Every target that doesn't produce a file of its own name is listed in `.PHONY`
  (`test format lint install clean ci`, ...).
- Comments explain *why*, same rule as 6.10, including the 3-line cap -- the Makefile's own
  notes on why `-MMD -MP` matters or why `VERSION` falls back to `git describe` point at
  docs/nhpm-plan.md rather than spelling the reason out in full. A recipe step whose purpose
  isn't obvious from the command itself gets one.
- Long recipes split across lines with a trailing `\` and `;` between shell statements on the
  continued line (see the `ci:` target), since each recipe line is its own shell invocation.

---

## 10. GitHub Actions workflows (YAML)

Covers `.github/workflows/*.yml` (`ci.yml`, `release.yml`).

- 2-space indentation, never tabs -- the YAML convention, enforced by `.editorconfig`'s
  `[*.{yml,yaml}]` section.
- Job and step `id`s (the keys under `jobs:`) are `kebab-case` (`build-and-test`); a step's
  `name:` is a short sentence in sentence case describing the action ("Install dependencies",
  "Test"), matching the log output someone actually reads in the Actions UI.
- Third-party actions are pinned to a major-version tag (`actions/checkout@v4`), not a
  branch or an unpinned `@main`.
- A multi-command step uses `run: |` with one command per line, same shape as a shell
  script's body, rather than chaining with `&&` on one line.
- Only the permissions a workflow actually needs are granted, declared explicitly with a
  `permissions:` block (e.g. `contents: write` to upload a release asset) instead of relying
  on the repository's default token scope.
- Secrets and tokens are only ever referenced through `${{ secrets.* }}` or
  `${{ github.token }}`, never hardcoded.
- Comments explain *why* a non-obvious choice was made, with the same 3-line cap as 6.10 --
  e.g. why `release.yml` doesn't build a native Windows binary gets a short pointer to
  docs/tools-libraries.md rather than the full reasoning inline.

---

## 11. Markdown documentation

Covers `README.md`, `CONTRIBUTING.md`, and everything under `docs/`.

- Every `docs/*.md` file keeps the structure set by the original split of `nhpm-plan.md`: an
  `# nhpm: <Topic>` title, a short intro paragraph linking back to
  [nhpm-plan.md](nhpm-plan.md) and any sibling doc it depends on, then the numbered sections
  carried over from that original single document (`## N. Topic`, `### N.M Subtopic`). A
  doc's sections are **never renumbered from 1**: other docs and commit messages
  cross-reference the absolute number (`docs/style.md section 6.3`), so renumbering silently
  breaks every reference to it.
- A `---` rule separates the intro from the first section, and separates each top-level
  `## N.` section from the next, within the same file (see how `features.md` or
  `nhpm-plan.md` break up their several sections). A doc with only one top-level section
  (`security.md`, `testing.md`, this file before section 7) typically still closes with a
  trailing `---` after it; a doc with several sections usually doesn't add one after the
  last, since there's nothing left to separate it from.
- `README.md` and `CONTRIBUTING.md` are entry points, not part of that numbering: plain `##`
  headings, no section numbers.
- Tables for structured, scannable reference info (dependency lists, naming rules, platform
  notes); prose only for what a table would flatten.
- A fenced block carries a language tag (` ```c `, ` ```sh `, ` ```make `, ` ```yaml `) when
  it holds real code in that language, so it highlights correctly. A block that isn't code in
  any language -- an ASCII diagram, a directory tree, a sample data dump -- stays a bare
  ` ``` ` fence; tagging it `text` to satisfy a linter would be noise, not a fix.
- Links between docs are relative (`[features.md](features.md)`), never an absolute path or
  a URL back into this repo.
- Inline code spans for every identifier, filename and command (`` `nhp_err` ``,
  `` `make test` ``), same as a C doc comment would.
- One blank line between elements (a heading, a paragraph, a list, a `---` rule).
- `.editorconfig` turns off trailing-whitespace trimming for `*.md` specifically: a line
  ending in two spaces is a deliberate Markdown hard line break, not something to clean up.
