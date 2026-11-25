# nhpm: Filing an issue

What to include when opening a bug report or feature request. Not a rigid form -- these are
the details that usually save a round trip asking for them.

## Security issues

Don't open a public issue for anything exploitable (path traversal, a crash triggered by a
malicious patch or site page, etc. -- see [security.md](security.md) for what nhpm already
guards against). Use GitHub's private vulnerability reporting instead (the repo's **Security**
tab -> **Report a vulnerability**).

## Bug reports

- `nhpm --version` output (or the git commit, if built from source)
- OS/distro and version; which recipe you were using, if it's build-related
  ([docs/README.md section 7](README.md#7-build-recipes))
- What you ran: the exact CLI command, or the screen (F2-F7) and key you pressed
- What you expected vs. what actually happened -- exact error text if there was one
- Whether it reproduces from a fresh instance/library, or only with a specific patch/instance
- For a build failure: attach `build.log` from the instance's folder, or the Build screen's
  `s` key to export it -- see [file-formats.md](file-formats.md) for where instance files live
- For a crash: if you can reproduce it, a run under `make debug` (ASan/UBSan) and its output
  is far more actionable than a plain backtrace

## Feature requests

- The problem you're hitting, not just the feature you want -- a fix might already exist
  another way
- Which screen or command this would fit into (an existing one, or a new F-key/subcommand)
- Whether it needs new on-disk state (a new `instance.conf` key, a new file) -- see
  [file-formats.md](file-formats.md)

## Everything else

Questions and docs gaps are fine as issues too; there's no separate discussion category.
