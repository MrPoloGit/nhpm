# Contributing to nhpm

Full details live in [docs/style.md](docs/style.md); this is the short version.

## Building and testing

```sh
make            # build ./nhpm
make test       # build and run the unit/integration tests
make format     # apply clang-format
make lint       # clang-format --dry-run, banned-function check, clang-tidy, cppcheck
make debug      # build with ASan/UBSan (see note below)
```

`make lint` runs clang-tidy and cppcheck only if they're installed locally; CI always runs
them. On macOS, `brew install llvm cppcheck` gets you both (`clang-tidy` ships inside `llvm`,
not as a separate formula). Apple's bundled clang/ASan deadlocks during startup on some recent
macOS versions; if `make debug` hangs, build with Homebrew's clang instead:
`CC=$(brew --prefix llvm)/bin/clang make debug`. Also on macOS, running anything that makes a
real network request (e.g. `nhpm download`) under LeakSanitizer reports leaks inside Apple's
`Network.framework`/curl's async DNS thread; that's a known platform false positive, not a bug
here -- check that the leak's stack trace has no `src/` frames before worrying about it.

Every commit should build, and pass `make lint` and `make test`.

## Code style, in brief

- **C11 + POSIX.1-2008/XSI.** `src/feature.h` sets `_XOPEN_SOURCE 700` and is included first
  in every `.c` file.
- **4-space indentation, no tabs** (except Makefiles), **100-column lines**, K&R braces with
  braces always used, pointer `*` next to the name. `clang-format` enforces this — don't
  hand-format, run `make format`.
- **Naming:** `snake_case` for everything except constants/macros/enum values
  (`UPPER_SNAKE_CASE`). Every public function and type is prefixed by its module
  (`lib_delete_version`, `inst_save`, `stack_enable`, ...). See docs/style.md section 6.3 for
  the full prefix table.
- **One module = one `.c` + one `.h`.** A header declares only what other modules use and is
  self-contained. Include order: `feature.h`, the module's own header, system headers
  (alphabetical), third-party headers, other project headers (alphabetical).
- **Errors:** functions that can fail return `nhp_err` (`NHP_OK` is `0`, everything else is
  negative) or `NULL`. Set a human-readable reason with `err_set(...)` right before returning
  an error; the UI/CLI read it back with `err_get()`. Every value that can signal failure
  (`malloc`, `fopen`, `write`, `close`, `rename`, every `nhp_err`) is checked.
- **Memory:** whoever allocates documents who frees; `*_new`/`*_load`/`*_dup`/`*_format`
  results are owned by the caller. Every `*_free` accepts `NULL`. Use `util.c`'s `buf_*`
  growable buffer and `str_*` helpers instead of ad-hoc `realloc`.
- **Never:** `strcpy`/`strcat`/`sprintf`/`gets`/`strtok`/`atoi`/`atol`/`system`/`popen`/
  `tmpnam`/`mktemp`. Use `snprintf`/`buf_append`/`strtol`/`proc_run`/`mkstemp`/`mkdtemp`
  instead (`make lint` greps for the banned ones).
- **No shell:** external commands run through `proc.c`'s `proc_run`/`proc_run_streaming`
  (`fork`+`execvp` with an argument array), never a shell string.
- **Comments** explain *why*, not what the next line does. Every public function gets a
  doc comment in its header: what it does, what the parameters mean, what it returns, who
  owns returned memory.
- **New behavior comes with tests; bug fixes come with a test that failed before the fix**
  (see docs/testing.md). `CHANGELOG.md` gets a line for every user-visible change.
- **Commits:** a short imperative summary (≤ 72 chars), blank line, then why. One logical
  change per commit; formatting-only changes go in their own commit.

## Where things live

- [docs/repo-structure.md](docs/repo-structure.md) — source layout and module boundaries.
- [docs/file-formats.md](docs/file-formats.md) — on-disk files and their formats.
- [docs/features.md](docs/features.md) — what nhpm does, feature by feature.
- [docs/security.md](docs/security.md) — security considerations.
- [docs/testing.md](docs/testing.md) — the test plan.
- [docs/nhpm-plan.md](docs/nhpm-plan.md) — dependencies/build, error handling, milestones,
  risks, open decisions.
