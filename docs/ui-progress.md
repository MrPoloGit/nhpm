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
- [x] Play hand-off (`ui_suspend_and_run_play()` in ui.c, the `def_prog_mode`/`endwin`/.../
      `refresh` pattern from 13.7), exposed via screen.h for Instances' own `p` to call --
      deliberately screen-specific, not a global key like `?`/`q` (see F2 Instances below)
- [x] `text_input_active` flag so a screen's own text box (search, prompts) owns every key
      instead of global `?`/Tab/`q` stealing them
- [x] `q` quits immediately from any screen, including sidebar focus and mid-build on the
      Build screen
- [x] No function keys anywhere: F1-F7/F9/F10 are gone (not reliably delivered by every
      terminal/multiplexer/SSH path). Plain-letter bindings instead: `?` help, `q` quit,
      Tab/Left then a screen's own `nav_letter` to switch screens
- [x] `ui_confirm()` -- blocking y/N confirmation in the status line
- [x] Reusable scrolling list widget (`ui_list_*`): Up/Down/k/j/PgUp/PgDn/Home/End, shared by
      Database, Library, Patches and Sources -- a long or narrow list scrolls the same way on
      all four, not just Database
- [x] Reusable scrolling text-view widget (`ui_text_view_*`), used by Build's streamed output,
      Help's static text (previously just clipped on short terminals, now scrollable), and by
      `ui_show_text_dialog()` (a blocking modal for an arbitrary block of text, used by the
      Changes dialog's full diff)
- [x] Mouse wheel scrolls the active screen (`mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED)`,
      decoded in the main loop and reused as if Up/Down had been pressed, so every screen's
      existing scroll handling applies with no screen-specific code). Clicking a row or the
      key bar is still not handled (13.5/13.2), so `BUTTON1_CLICKED` is deliberately not
      requested. A plain drag still goes to nhpm instead of the terminal while wheel reporting
      is on; Shift+drag bypasses that and selects/copies text normally -- confirmed working,
      documented in the in-app Help screen and docs/features.md 13.2
- [x] `ui_show_popup_menu()`: a small bordered popup over the current screen (Up/Down/k/j,
      Enter selects, Esc cancels), for picking one of a short list without replacing the whole
      screen the way Snapshots/Changes do. First real use of the ncurses panel library
      (`panelw`). Wired into Instances' `n` New (base source version, build recipe) and
      Sources' `n` Known version
- [x] `ui_show_popup_input()`: the same floating box, but for free text a fixed list doesn't
      fit (a name, a path, a git tag). Every text entry in the UI goes through this (or
      `ui_show_popup_input_live()` for a live filter, e.g. `/` search) -- nothing prompts in
      the status line except `ui_confirm()`'s y/N
- [x] `str_matches_all_words()` (util.c): the "plain words match N fields, all must match"
      rule Database/Library's own search already used, generalized and shared. Backs the new
      `/` search on Instances, Patches and Sources -- those don't need the full `v:`/`a:`/
      `r:` mini-language idx_query_parse() has, just plain words against their own columns
- [x] `ui_show_conflict()`: on any `NHP_ECONFLICT`, shows the patch id/version, per-file hunk
      tally, and raw quilt/patch output (section 11.6's "view the failing hunks") in
      `ui_show_text_dialog()`. Wired into every conflict site that has nothing sensible to
      force: Patches' disable/J/K/x, Instances' flatten, and the CLI.
- [x] `ui_resolve_conflict()`: the real `[F]orce`/`[V]iew`/`[C]ancel` choice (`ui_show_popup_menu()`),
      for the one conflict site where Force actually applies -- enabling the patch itself
      (`conflict->is_requested_patch`). Wired into Patches' Space (enable, not disable),
      Database's `D` and Library's `a` (both add-then-enable). Force hands `$EDITOR` every
      failed file plus its `.rej` (`ui_run_editor()`, new and shared with Patches' own `o`)
      then calls `stack_force_finish()`. No CLI equivalent of the three-way choice exists --
      `stack_force_begin()`/`stack_force_finish()` are reachable only from the UI so far.
- [x] `ui_draw_progress()` (`widgets.c`/`widgets.h`): the real 13.5 progress widget -- a
      `[####----]  45%` bar plus text in the status line, colored `UI_COLOR_OK` (reverse
      video if monochrome), repainting immediately like `ui_flush_status()`. Negative
      `fraction` (total not yet known) draws no bar, just the text, same as before. Wired
      into Database's index-refresh progress (`scr_db.c`) and Sources' known-version
      download progress (`scr_sources.c`) -- the only two real progress-callback sites.
- [x] Esc now cancels those same two progress-callback sites (13.6), same `nodelay()`+
      `wget_wch()` poll as Build's own `on_build_line()`, `q` also flagging `quit_requested`
      for the same reason. `scr_sources.c`'s `known_download_progress()` polls directly
      (`net_progress_fn` already returned `bool`). `idx_progress_fn` (`index.h`) had to widen
      from `void` to `bool` first -- `idx_refresh()` now returns `NHP_ECANCELED` and leaves
      `*idx` untouched on cancel, same all-or-nothing contract a plain fetch failure already
      had. The CLI's own `refresh_progress()` (`main.c`) always returns `false`: no
      interactive cancel there, Ctrl-C already works. Confirmed live (pty) for both sites:
      Esc mid-refresh and mid-download both land on a clean "canceled" status and exit
      normally.
- [x] `s`/`S` sort-column toggle on Database and Library (cycles Name/Author/For/Rating/Id
      and Name/Version/For/Id; `S` reverses; selection follows its entry across a resort).
      Id sorts numerically (`strtol`), not as a string, and Library's own `local-<slug>` ids
      (a made-from-folder patch) sort after every real numeric id instead of reading as 0.
      Deliberately not added to Patches: its row order is the real Quilt stack order, which
      J/K already changes for real (with real apply-order consequences); a cosmetic sort
      there would show an order that doesn't match what J/K would actually do (user request).
      The other screens have too few rows to bother with any sort
- [x] A screen's own `draw()` no longer clobbers an action's one-shot result message with its
      generic summary line on the next redraw. Fixed via a sticky status buffer, checked
      first, on Build (`g_build_failed`/`g_build_error`), Sources, Database, Library, and the
      Changes dialog; Instances and Patches were already fine
- [x] `--debug` (main.c) opens a timestamped activity log at
      `$XDG_CACHE_HOME/nhpm/nhpm-debug-<stamp>.log` of every status-line message the UI shows,
      success or failure. Separate from util.c's always-on `nhpm.log`. Off by default; the
      path is printed to stderr once the interface exits

## Screens

**F1 Help** -- done (static text, scrollable, kept in sync with what's actually wired up per
screen below).

**F2 Instances** -- done
- [x] List (name/version/patches/built/last played), Enter activates. Built status is
      colored (13.8): red "NOT BUILT", yellow "out of date" (`build_is_stale()`), green
      "built". New instances show up before their first build, clearly marked rather than
      hidden
- [x] `p` Play (`ui_suspend_and_run_play()`, ui.c/screen.h): runs the active instance's
      installed game and returns to nhpm when it exits; `last_played` is updated. Instances-
      only, not a global key like `?`/`q` -- it was global at first, moved here on request
- [x] `/` search (name/version/recipe/notes, plain words, all must match): a true filter like
      Database/Library, hiding non-matching rows -- Instances has no J/K-style reordering, so
      unlike Patches there's no ambiguity in hiding rows. `g_visible` (pointers into the real
      `g_instances`) is what every action actually reads/writes by selection now, not the raw
      array directly, so removing/renaming the selected row still touches the right instance
- [x] `n` New: popups for name, Managed/Linked kind, then (Managed) base source version or
      (Linked) folder, then build recipe (Esc there defaults to "custom"); the new instance
      becomes active immediately (same as pressing Enter on it), so the next patch/build/play
      action applies to it. Was a separate manual step at first -- easy to silently apply a
      patch to whatever instance was already active instead of the one just created, so it
      moved here on request. Same change in the CLI's `inst new`/`inst link`
- [x] `c` Clone (prompts new name, y/N copy-saves)
- [x] `r` Rename (keeps the active-instance pointer following a renamed active instance)
- [x] `x` Delete (confirms, then y/N keep-saves)
- [x] `t` Snapshots dialog (blocking, modeled on `ui_confirm()`/`ui_show_popup_input()`): Enter
      restores, `x` deletes, `a` adds one with an optional label, Esc closes. Restore goes
      through `stack_restore_snapshot()` (stack.c) -- it used to only swap `instance.conf`
      and leave the Quilt folder stale (same bug the CLI's `inst restore` had); both now also
      call `stack_restore()` to actually re-sync the folder, same as `u` Undo already did
- [x] `e` Export / `i` Import (`.nhinst`): prompts path (export defaults to `<name>.nhinst`;
      import also prompts the new name and an optional base-source version override)
- [x] Linked instances: `n`'s popup now asks Managed/Linked before the recipe; Linked
      prompts for a folder and calls `inst_link()` instead of picking a base source
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
- [x] `/` search (`v:`/`a:`/`r:>=N`/`id:`/`lib:`/`in:` syntax): a floating popup box
      (`ui_show_popup_input_live()`), re-filtering live on every keystroke, same as every
      other text input; Esc clears the filter, Enter keeps it and closes the box
- [x] `Enter`/`d` download (force-fresh: deletes a stale copy of the same version first)
- [x] `D` download and add to the active instance, enabled
- [x] `u` uninstall (remove every downloaded version from the library)
- [x] `R` refresh the whole index, with live per-page progress
- [x] Incremental search: every keystroke re-filters live; Esc clears. `n`/`N` move to the
      next/previous match (same as `j`/`k`, under 13.9's common-key names)
- [x] "All versions" toggle (`v`): off shows the newest version plus a "(+)" count; on lists
      every downloaded version comma-separated
- [x] `lib:`/`in:`/`mk:` search tokens, pulled out before the rest goes through
      `idx_query_parse()`: `lib:yes`/`lib:no` filters on whether the patch is downloaded at
      all; `in:<instance>` on whether it's in that instance's patch stack; `mk:` on the Mk
      glyph (`mk:!`/`mk:=`/`mk:~`/`mk:?`, or the words `conflict`/`exact`/`fuzzy`/`unknown`)

**F4 Library** -- mostly done
- [x] List, versions nested under each patch, "Used in" column computed by scanning every
      instance's own patch list
- [x] `/` search, same syntax as Database (13.10), plus `in:<instance>`/`mk:`; live
      filtering, `n`/`N` move within the filtered rows
- [x] `s`/`S` sort-column toggle (Name/Version/For/Id, `S` reverses): reorders whole patches,
      keeping each patch's versions nested; the selected row follows its patch/version across
      a resort
- [x] `Enter` view (shows the selected version's touched files and download URL in the status
      line -- no scrolling text-view widget yet to show more)
- [x] `a` add the selected version straight to the active instance's patch stack
      (`stack_add()`, same as Database's `D`, but from an already-downloaded version).
      Adding an id already in the stack is rejected (`NHP_EEXIST`, status message naming
      which slot has it) instead of silently appending a second, conflicting slot -- pressing
      `a`/`D` twice on the same patch (e.g. because the first press gave no obvious feedback)
      used to do exactly that
- [x] `x` delete version / `X` delete patch, confirmed; names any instance still using that
      version (features.md section 8 -- they keep their own copy regardless)
- [x] `u` checks the site for a newer version (re-reads the cached detail page and compares
      versions; no per-version history on that page)
- [x] `i` import a local `.diff` (`lib_import_local()`), prompted path/name/author/target/
      version
- [x] Colored Mk compatibility mark (same `=`/`~`/`!`/`?` as Database, against the active
      instance's version), one per row/version
- [x] `f` fold/unfold (docs/features.md 10.7/13.9): collapses the selected row's patch to
      just its named row, `(+)` appended to the version column same as Database's own "all
      versions off". Persisted as `library_folded` in ui-state (comma-separated ids),
      restored once per process on first entry. A one-version patch has nothing to fold
- [ ] `r:>=N` search token: no-op here -- `lib_patch` carries no rating, only the Database's
      `idx_entry` does
- [x] `m` Make from folder: prompts folder and base (`base`/`folder:P`/`tarball:P`/`git:REF`,
      same syntax as the CLI's `--from`), then hands off into the Changes dialog the same way
      `w` on Patches does. Not available for a `git:` base, since that's compared directly
      with `git diff` text and never builds a file-by-file list

**F5 Patches** -- done
- [x] List (on/off, id, name, version, target, Status column: applied/disabled, "(modified)"
      for a hand-fixed patch, the "c" dry-run result for a disabled one), Space toggle, J/K
      move, `x` remove
- [x] Reloads the active instance fresh on every entry (not just trusting in-memory state)
- [x] Colored Mk compatibility mark per patch, against the instance's own version
- [x] `d` Diff view: the selected patch's applied diff, read from the instance's own
      `patches/` folder (not the library's copy -- they can differ for a hand-fixed patch),
      shown in `ui_show_text_dialog()`
- [x] `c` (dry-run every disabled patch): `stack_check_disabled()` (docs/features.md 11.7),
      read-only, also exposed via CLI as `nhpm status --check`. Fills in the list's own
      Status column (cached until the next stack change shifts indices)
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
- [x] `u` (undo last change): every `stack_enable`/`disable`/`move`/`add`/`remove` takes an
      `auto-<op>-<id>` snapshot first; `stack_undo()` (docs/features.md 11.8) finds the
      latest one and restores it, re-syncing the Quilt folder too (also `nhpm undo`). `t`
      takes a manual, labeled snapshot the same way Instances' own Snapshots dialog does,
      without leaving the Patches screen
- [x] "uncaptured changes" indicator: a `*` marker next to the column headers when
      `mkp_changes()` finds anything, recomputed after every stack change and on entry
- [x] The multi-patch file-conflict flag: `stack_find_shared_files()` (docs/features.md
      11.9), read-only, best-effort via library `files_touched` (also part of CLI's `nhpm
      status --check`). The details area shows which other enabled patches share a file
      with the selected one, in `UI_COLOR_WARN`
- [x] `/` search (id/name/version/target, plain words, all must match) and `n` jump to the
      next match, wrapping -- highlights (`A_BOLD`) rather than hides, unlike Database/
      Library/Instances/Sources: row position here is the real Quilt stack order J/K's "one
      slot" move depends on, so hiding rows could make J/K silently skip past several real
      positions at once. `n` is the only way to reach a match (not an alias for `j` the way
      Database's own `n` is); `N` stays "new patch" since it was already taken

**F6 Build** -- done
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
- [x] A succeeded build points at Instances for Play (`p` isn't a Build-screen key -- see
      Foundation and F2 Instances above)
- [x] `custom` recipes now run: `instance.conf`'s `custom_commands` (`;`-separated, no
      quoting, `$PREFIX` substituted for the install path, docs/features.md 10.6) is split
      and run through `proc_run_streaming()`, stopping at the first failure; `build_run()`
      still returns `NHP_EINVAL` only when it's empty. The Build screen's own `b` no longer
      blocks `custom` with a stale "not runnable yet" message -- it was never updated when
      the backend above landed
- [x] `c` edits `custom_commands` from the Build screen itself (a popup text box, saved via
      `inst_save()`); the details pane shows the current value when the recipe is `custom`.
      Hand-editing `instance.conf` is no longer the only way
- [x] `s` exports `build.log` to a file you name (a popup "Save to:" prompt, same pattern as
      Instances' combined-patch export); reads the file `build_run()` wrote rather than
      reserializing the on-screen lines, so a canceled build still exports what it captured
- [x] `nethack-3.4-unix` and `nethack-3.6-linux` both now pass extra `make` variables
      (`CC='cc -std=gnu89 ...'`, and for 3.4 also `WINTTYLIB`/`CHOWN`/`CHGRP`) so they actually
      build against current GCC/glibc, not just the fixture; see docs/features.md 10.6 for why.
      Both verified end to end against their real sources (character creation through `#quit`)

**F7 Sources** -- done
- [x] `n` Known versions popup (`ui_show_popup_menu()`): a small, hand-verified catalog
      (`BSRC_KNOWN_VERSIONS` in sources.c/.h) of real NetHack releases; picking one
      downloads/extracts a tarball or clones a git tag and registers it as a base source in
      one step. Currently 3.4.3 (tarball), 3.6.7 (git tag), and 5.0.0 (git tag,
      `NetHack-5.0.0_Released`). Every entry has actually been fetched and confirmed to
      produce a buildable source tree, with the matching recipe (10.6). Not unit tested (no
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
- [x] `/` search (version/origin, plain words, all must match): a true filter like Database/
      Library/Instances. No `n`/`N` next-match aliases -- `n` is already Known version here,
      and there's no second free letter left to pair with it

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
- [x] Per-row `+/-` line counts and the Details pane's first-hunk snippet from the 12.5
      mockup: `parse_diff_stats()` parses one combined `full_diff()` call (every non-binary
      file, run once per dialog open) into per-file counts and a snippet, splitting on each
      file's own `diff -Naur a/X b/X` header. Shown in `app->layout.details` as the selection
      moves -- `Enter`'s full diff still covers seeing more than the first hunk
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

## Issues