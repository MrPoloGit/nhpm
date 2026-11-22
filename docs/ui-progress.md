# UI progress (M7)

A living checklist of what's actually built in `src/ui/`, tracked against the design in
`features.md` section 13. Update this alongside the code -- when a key or screen goes from
planned to working, move it here instead of leaving the gap implicit.

## Foundation (`ui.c`, `widgets.c`, `screen.h`)

- [x] Startup/shutdown (`initscr`/`endwin`, signal handlers, `atexit`), 80x24 minimum (13.11)
- [x] Resize handling (`KEY_RESIZE` -> `relayout()`)
- [x] Sidebar navigation: Tab/Left focuses it, Up/Down+Enter or a screen's own letter jumps
      screens, Right/Enter returns focus to content
- [x] ASCII bordered panels (`|`/`-`/`+`, not ACS/Unicode line-drawing -- matches NetHack's own
      tty look)
- [x] Color pairs by role (header/selected/ok/warn/error/dim), monochrome/`NO_COLOR` fallback
      to plain glyphs (13.8)
- [x] Status line + key bar; `ui_flush_status()` for progress during a blocking call (13.6)
- [x] `p` play hand-off (`suspend_and_run_play`, the `def_prog_mode`/`endwin`/.../`refresh`
      pattern from 13.7)
- [x] `text_input_active` flag so a screen's own text box (search, prompts) owns every key
      instead of global `?`/`p`/Tab/`q` stealing them
- [x] `q` quits immediately from any screen, including sidebar focus and mid-build on the
      Build screen
- [x] No function keys anywhere: F1-F7/F9/F10 are gone (not reliably delivered by every
      terminal/multiplexer/SSH path). Plain-letter bindings instead: `?` help, `q` quit,
      `p` play, Tab/Left then a screen's own `nav_letter` to switch screens
- [x] `ui_prompt_line()` -- blocking single-line text prompt in the status line
- [x] `ui_confirm()` -- blocking y/N confirmation in the status line
- [x] Reusable scrolling text-view widget (`ui_text_view_*`), used by Build's streamed output
      and by `ui_show_text_dialog()` (a blocking modal for an arbitrary block of text, used by
      the Changes dialog's full diff). Help isn't on either yet (still clipped on short
      terminals)
- [x] `ui_show_popup_menu()`: a small bordered popup over the current screen (Up/Down/k/j,
      Enter selects, Esc cancels), for picking one of a short list without replacing the whole
      screen the way Snapshots/Changes do. First real use of the ncurses panel library
      (`panelw`). Wired into Instances' `n` New (base source version, build recipe) and
      Sources' `n` Known version
- [x] `ui_show_popup_input()`: the same floating box, but for free text a fixed list doesn't
      fit (a name, a path, a git tag). Wired into Instances' `n` New (instance name) and
      Sources' `a`/`t`/`g` (path, tag, version hint). `ui_prompt_line()`'s status-line prompt
      is still used for quick answers like y/N confirms
- [x] `ui_show_conflict()`: on any `NHP_ECONFLICT`, shows the patch id/version, per-file hunk
      tally, and raw quilt/patch output (section 11.6's "view the failing hunks") in
      `ui_show_text_dialog()`. Wired into every conflict site: Patches' Space/J/K/x,
      Database's `D`, Library's `a`, Instances' flatten, and the CLI. No `[F]orce`/`[V]iew`/
      `[C]ancel` choice dialog yet -- a conflict just shows the detail and leaves the instance
      unchanged (Force is CLI-only)
- [ ] Progress widget distinct from plain status-line text (current refresh/download progress
      is functional but not the spec's dedicated widget)
- [ ] No click handling yet. Mouse mode (`mousemask()`) is off for now, since turning it on
      would cost the terminal's own text selection for no benefit until this is built
- [x] `s`/`S` sort-column toggle on Database and Library (cycles Name/Author/For/Rating and
      Name/Version/For; `S` reverses; selection follows its entry across a resort). Patches
      keeps stack order (J/K move); the other screens have too few rows to bother
- [x] A screen's own `draw()` no longer clobbers an action's one-shot result message with its
      generic summary line on the next redraw. Fixed via a sticky status buffer, checked
      first, on Build (`g_build_failed`/`g_build_error`), Sources, Database, Library, and the
      Changes dialog; Instances and Patches were already fine
- [x] `--debug` (main.c) opens a timestamped activity log at
      `$XDG_CACHE_HOME/nhpm/nhpm-debug-<stamp>.log` of every status-line message the UI shows,
      success or failure. Separate from util.c's always-on `nhpm.log`. Off by default; the
      path is printed to stderr once the interface exits

## Screens

**F1 Help** -- done (static text, kept in sync with what's actually wired up per screen below).

**F2 Instances**
- [x] List (name/version/patches/built/last played), Enter activates. Built status is
      colored (13.8): red "NOT BUILT", yellow "out of date" (`build_is_stale()`), green
      "built". New instances show up before their first build, clearly marked rather than
      hidden
- [x] `n` New: popups for name, base source version, and build recipe (Esc there defaults to
      "custom")
- [x] `c` Clone (prompts new name, y/N copy-saves)
- [x] `r` Rename (keeps the active-instance pointer following a renamed active instance)
- [x] `x` Delete (confirms, then y/N keep-saves)
- [x] `t` Snapshots dialog (blocking, modeled on `ui_confirm()`/`ui_prompt_line()`): Enter
      restores, `x` deletes, `a` adds one with an optional label, Esc closes
- [x] `e` Export / `i` Import (`.nhinst`): prompts path (export defaults to `<name>.nhinst`;
      import also prompts the new name and an optional base-source version override)
- [ ] Linked instances (`inst_link`, pointing at an existing folder) -- CLI only so far
- [x] `C` Combined-patch: prompts a save path (default `<name>-combined.diff`), then y/N
      "add to library?" and "flatten into a new instance?" -- the same three destinations as
      the CLI's `combined --out/--to-library/--flatten`
- [x] Section 10.8 instance locking: the active instance's lock is held for as long as it
      stays active, not re-acquired on every refresh (avoids thrashing `flock()` and a
      release/re-lock race). `NHP_ELOCKED` sets `app->active_read_only` and shows "<name> is
      open in another nhpm (pid N), read-only". `ui_require_write_access()` guards every
      mutating action (Patches, the Changes dialog, Database, Library, Build, Play, and
      Instances' rename/delete of the active instance). Deleting the active instance also
      drops the lock. Not yet lock-checked: rename/delete/clone/snapshot-restore on a
      different, non-active row
- [x] Section 10.4 consistency check/Adopt/Restore: runs whenever the active instance
      changes; on drift shows the detail then offers Adopt, then Restore (asked one at a
      time, same style as Combined-patch above). Declining both leaves things as found.
      Skipped while locked out, since the other nhpm's folder could be changing mid-check

**F3 Database** -- mostly done
- [x] List with colored Mk compatibility marks, Downloaded column (shows the version, or "+"
      for more than one)
- [x] `/` search (commit-on-Enter, Esc clears; `v:`/`a:`/`r:>=N`/`id:` syntax); shown in the
      status line, not a panel row
- [x] `Enter`/`d` download (force-fresh: deletes a stale copy of the same version first)
- [x] `D` download and add to the active instance, enabled
- [x] `u` uninstall (remove every downloaded version from the library)
- [x] `R` refresh the whole index, with live per-page progress
- [x] Incremental search: every keystroke re-filters live; Esc clears. `n`/`N` move to the
      next/previous match (same as `j`/`k`, under 13.9's common-key names)
- [x] "All versions" toggle (`v`): off shows the newest version plus a "(+)" count; on lists
      every downloaded version comma-separated
- [x] `lib:`/`in:` search tokens, pulled out before the rest goes through
      `idx_query_parse()`: `lib:yes`/`lib:no` filters on whether the patch is downloaded at
      all; `in:<instance>` on whether it's in that instance's patch stack

**F4 Library** -- mostly done
- [x] List, versions nested under each patch, "Used in" column computed by scanning every
      instance's own patch list
- [x] `/` search, same syntax as Database (13.10), plus `in:<instance>`; live filtering,
      `n`/`N` move within the filtered rows
- [x] `s`/`S` sort-column toggle (Name/Version/For, `S` reverses): reorders whole patches,
      keeping each patch's versions nested; the selected row follows its patch/version across
      a resort
- [x] `Enter` view (shows the selected version's touched files and download URL in the status
      line -- no scrolling text-view widget yet to show more)
- [x] `a` add the selected version straight to the active instance's patch stack
      (`stack_add()`, same as Database's `D`, but from an already-downloaded version)
- [x] `x` delete version / `X` delete patch, confirmed; names any instance still using that
      version (features.md section 8 -- they keep their own copy regardless)
- [x] `u` checks the site for a newer version (re-reads the cached detail page and compares
      versions; no per-version history on that page)
- [x] `i` import a local `.diff` (`lib_import_local()`), prompted path/name/author/target/
      version
- [x] Colored Mk compatibility mark (same `=`/`~`/`!`/`?` as Database, against the active
      instance's version), one per row/version
- [ ] `r:>=N` search token: no-op here -- `lib_patch` carries no rating, only the Database's
      `idx_entry` does
- [x] `m` Make from folder: prompts folder and base (`base`/`folder:P`/`tarball:P`/`git:REF`,
      same syntax as the CLI's `--from`), then hands off into the Changes dialog the same way
      `w` on Patches does. Not available for a `git:` base, since that's compared directly
      with `git diff` text and never builds a file-by-file list

**F5 Patches** -- partial
- [x] List (on/off, id, name, version, target, "(hand-fixed)" marker), Space toggle, J/K move,
      `x` remove
- [x] Reloads the active instance fresh on every entry (not just trusting in-memory state)
- [x] Colored Mk compatibility mark per patch, against the instance's own version
- [x] `d` Diff view: the selected patch's applied diff, read from the instance's own
      `patches/` folder (not the library's copy -- they can differ for a hand-fixed patch),
      shown in `ui_show_text_dialog()`
- [ ] `c` dry-run every disabled patch, fill in a real Status column ("2 hunks would fail")
- [x] `w` opens the Changes dialog (`scr_changes.c`, not a screen in its own right -- see
      below)
- [x] `N` new empty patch; `E` edit an existing one; `o` open a source file in `$EDITOR`; `R`
      refresh the patch being edited. All three work with zero patches on the stack. `R`
      mirrors `cmd_edit_refresh()` in main.c, including forking a downloaded patch's edit into
      its own `local-` variant automatically (section 12.4's "L"). `$EDITOR` wins over
      config's own `editor` when both are set. Backend: `stack_edit_begin()`/
      `stack_edit_refresh()`, `stack_new_patch()` and `stack_track_file()` in stack.c;
      `lib_add_version()` and `lib_fork_as_local()` in library.c. None of this has a
      header-writing step the way `mkp_capture()`'s patches get -- a patch made via
      N/edit/refresh is a plain diff with correct library metadata, no embedded name/author
      text in the file itself
- [ ] `t` snapshot, `u` undo last change
- [ ] "uncaptured changes" header indicator
- [ ] Multi-patch file-conflict flag in the details area

**F6 Build** -- mostly done
- [x] `b` runs the active instance's recipe (`build_run()`), streaming each line into the
      panel live using the scrolling text-view widget
- [x] `error:`/`warning:` lines colored red/yellow (case-insensitive substring match); `e`
      jumps to the next error line, wrapping around
- [x] Esc during a build polls non-blockingly (`nodelay()`+`wget_wch()`, 13.6) and cancels it
      (`proc.c` sends SIGTERM; shows "build canceled" rather than treating it as a failure)
- [x] Loads the instance's existing `build.log` on entry, so leaving and returning (or
      restarting nhpm) doesn't lose the last build's output
- [x] Details pane shows the recipe, last-built time, and error/warning counts; status line
      flags an out-of-date build (`build_is_stale()`)
- [x] A failed build's status message ("build failed: ...") survives the next redraw instead
      of being overwritten by the generic stale/line-count message (same sticky-status fix as
      Foundation above)
- [x] `p` play already works globally once a build has succeeded (no Build-specific wiring
      needed -- `suspend_and_run_play()` in ui.c is screen-independent)
- [ ] `custom` recipes: `build_run()` returns `NHP_EINVAL` for them (no built-in command list,
      docs/features.md 10.6); the screen reports this rather than running anything --
      editable recipes are still UI/M7 scope, not built

**F7 Sources** -- done
- [x] `n` Known versions popup (`ui_show_popup_menu()`): a small, hand-verified catalog
      (`BSRC_KNOWN_VERSIONS` in sources.c/.h) of real NetHack releases; picking one
      downloads/extracts a tarball or clones a git tag and registers it as a base source in
      one step. Currently just 3.4.3 (tarball) and 3.6.7 (git tag). Every entry has actually
      been fetched and confirmed to produce a buildable source tree. Not unit tested (no
      network in CI, docs/testing.md 17.1); exercised manually instead
- [x] `a`/`t`/`g` (add from folder/tarball/git, a path or tag you already have) remain the
      manual fallback for anything not in the known-versions catalog. Their path/tag/
      version-hint entry uses `ui_show_popup_input()`
- [x] List (version, origin parsed into "folder <path>"/"tarball <path>"/"git <tag>", a count
      of instances using each one)
- [x] "DL" column: a Y/N sanity glyph for whether `tree/` actually looks like a NetHack source
      rather than a stub (checks for more than just `include/patchlevel.h`, which every base
      source has regardless). Not a backend fact, just a glyph for this screen
- [x] `a` add from folder / `t` add from tarball, both prompting path then an optional version
      hint (blank reads `include/patchlevel.h`, section 9.3)
- [x] `g` add from a git tag (`bsrc_add_from_git()`)
- [x] Adding a version that already exists offers to replace the conflicting entry on the spot
      (confirm, remove the old one, retry the same add) instead of requiring a manual `x`
      first; `sources.h`'s `bsrc_add_from_*` functions hand back `*out_version` on
      `NHP_EEXIST` so the caller knows which version conflicted
- [x] `x` remove, confirmed; names every instance still using it (they keep their own copy,
      section 9.5) rather than blocking the removal
- [x] Details pane: added date, checksum (or "none" for a folder/git import), variant flag,
      and which instances use it
- [x] Same sticky-status fix as Foundation above, applied here first: a redraw right after
      `a`/`t`/`g`/`x` no longer overwrites that action's own result message with the generic
      "%zu base source(s)" line

**Changes dialog** (`scr_changes.c`, docs/features.md 12.5) -- done, opened by `w` on Patches
or `m` on Library
- [x] Lists the comparison's result (`mkp_changes()` for `w`, `mkp_from_folder_changes()` for
      `m`): `[x]`/`[ ]` tick (default from `default_selected`), M/A/D/B status letter, path;
      `B` can't be ticked (binary, can't go in a text diff)
- [x] `Space` toggles the selected row's tick
- [x] `Enter` shows the full diff of every non-binary changed file in `ui_show_text_dialog()`
- [x] `S` prompts name/version/author/target/short description, then commits exactly the
      ticked files (`mkp_capture()` for `w`, `mkp_from_folder()` for `m`); closes the dialog
      on success
- [x] Esc closes without saving
- [ ] Per-row +/- line counts and the inline diff-snippet strip from the 12.5 mockup --
      skipped for now (would need per-file diff splitting); `Enter`'s full diff covers the
      same need
- [x] `h` toggles showing build-output files guessed hidden: they're tagged
      (`mkp_file.hidden`) instead of dropped, so the dialog can reveal and even tick one.
      `mkp_combined()` still excludes hidden files from the actual diff regardless. The CLI's
      `nhpm changes` has the same via `--hidden`
- [x] `i` adds the selected row's file (as `*.ext`, or the exact path with no extension) to
      config's ignore list, editable in the prompt first; every file is then re-tagged against
      the updated patterns in place
- [x] Reachable from both Patches (`w`, an instance's own uncaptured edits) and Library (`m`,
      12.6's "from any modified folder") via a backend-agnostic dialog loop (two callbacks,
      `full_diff`/`commit`). Not available for a `git:` base on `m` -- see F4 Library's note
      above

## Testing

- [ ] Automated tmux smoke test (`testing.md` 17.3)
- [ ] Manual checklist pass recorded
- Verification so far has been: build/lint/test green after each change, plus the user
  exercising each screen live and reporting back (per their own request -- the UI is not
  self-tested via tmux in this project going forward).
