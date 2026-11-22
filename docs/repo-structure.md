# nhpm: Repository structure

Part of the [nhpm development plan](nhpm-plan.md). Source layout and module boundaries.
See [style.md](style.md) for coding conventions and [features.md](features.md) for what each
module implements.

---

## 5. Source layout (modules)

```
nhpm/
├── Makefile
├── README.md
├── doc/nhpm.1
├── src/
│   ├── feature.h       # _XOPEN_SOURCE and portability macros, included first everywhere
│   ├── main.c          # arguments, start UI or run CLI command
│   ├── config.c/.h     # config file, XDG paths, key=value/INI reader+writer
│   ├── util.c/.h       # strings, dynamic arrays, safe paths, sha256, atomic save, logging
│   ├── proc.c/.h       # fork/exec with captured output, no shell
│   ├── net.c/.h        # libcurl wrapper: GET/POST to memory or file, progress, cancel
│   ├── site.c/.h       # ALL knowledge of the website: URLs and XPath expressions
│   ├── index.c/.h      # index.tsv, local search and filters
│   ├── library.c/.h    # downloaded patches: add, list, delete versions, verify
│   ├── version.c/.h    # parse "NetHack 3.6.0", read patchlevel.h, compare versions
│   ├── sources.c/.h    # base sources: import from folder/tarball/git, verify, remove
│   ├── instance.c/.h   # instances: create, load, save, clone, rename, delete,
│   │                   #   snapshots, export/import, lock
│   ├── quilt.c/.h      # run quilt/patch as subprocesses, parse output
│   ├── stack.c/.h      # enable/disable/reorder with rollback, sync with instance.conf
│   ├── build.c/.h      # build recipes, run build, stream output, launch game
│   ├── mkpatch.c/.h    # combined patches, capture changes, patch from folder/git,
│   │                   #   ignore rules, header writing, verification
│   └── ui/
│       ├── ui.c/.h         # ncurses setup/teardown, colors, main loop, resize, key bar
│       ├── widgets.c/.h    # list view, text view, input line, progress, dialogs
│       ├── scr_instances.c
│       ├── scr_db.c
│       ├── scr_library.c
│       ├── scr_patches.c   # the active instance's patch stack
│       ├── scr_sources.c
│       ├── scr_changes.c   # review changed files before saving a patch
│       ├── scr_build.c
│       └── scr_help.c
└── tests/
    ├── fixtures/       # saved HTML pages, sample diffs, a tiny fake NetHack source
    ├── test_site.c
    ├── test_index.c
    ├── test_version.c
    ├── test_instance.c
    ├── test_stack.c    # needs quilt installed
    └── run.sh
```

**Rules:**

- Only `site.c` knows URLs and HTML structure.
- Only `src/ui/` includes `<curses.h>`. The UI calls the other modules, never libcurl,
  quilt or the filesystem directly. The command-line mode uses the same modules, which keeps
  all the logic testable without a terminal.
- Only `instance.c` reads or writes `instance.conf`.

The repository root also holds the style and tool configuration from section 6:
`.clang-format`, `.clang-tidy`, `.editorconfig`, and `CONTRIBUTING.md` (a short copy of
section 6 for contributors).

---
