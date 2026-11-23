# nhpm: Files on disk and file formats

Part of the [nhpm development plan](nhpm-plan.md). Where nhpm keeps its data (XDG
layout) and the formats of the files it owns. See [features.md](features.md) for what writes
and reads each of these, and [repo-structure.md](repo-structure.md) for the module that owns
each format (e.g. only `instance.c` reads or writes `instance.conf`).

---

## 4. Files on disk

nhpm follows the XDG base-directory convention, with the usual fallbacks.

```
~/.config/nhpm/
    config                      # general settings
    ui-state                    # remembered UI state (section 10.7)

~/.cache/nhpm/               # everything here can be deleted and rebuilt
    index.tsv                   # every patch on the site
    index.meta                  # when the index was refreshed
    pages/<id>.html             # cached detail pages
    nhpm.log                 # log of commands run

~/.local/share/nhpm/
    library/                    # downloaded patches
        485-new-dumplog/
            patch.meta
            1.0.0/
                patch.diff
                version.meta
    exports/                    # combined patches and exported own patches (default location)
    sources/                    # untouched NetHack sources, read-only
        3.4.3/
            source.meta
            tree/ ...
        3.6.7/
            source.meta
            tree/ ...
    instances/
        367-colorful/
            instance.conf       # THE saved instance (section 10.2)
            instance.conf.bak   # previous version, kept on every save
            src/                # working copy of the source, a Quilt setup
                patches/
                series
                .pc/
            install/            # where the game is installed; save files, bones, scores
            snapshots/
                2026-11-15T20-14-03-before-building.conf
            build.log           # output of the last build
            backup/<date>/      # copies of your edited files, made before capturing a patch
            lock                # one nhpm at a time per instance
```

### File formats

All of nhpm's own files are plain text, readable and editable by hand, with no extra
libraries needed. Two formats are used:

**`key = value` files** with `#` comments and optional `[section]` headers (INI style):
`config`, `ui-state`, `patch.meta`, `version.meta`, `source.meta`, `instance.conf`.

**Tab-separated files** for long lists: `index.tsv`.

Every file is saved by writing to a temporary file in the same folder, `fsync`, then
`rename` over the old file, so a crash or full disk never leaves a half-written file.

**`config`:**

```ini
index_max_age_days = 7
request_delay_ms   = 1000
wayback_fallback   = yes
max_download_mb    = 5
author_name        = Your Name   # default author for patches you make
ignore             = *.swp *.bak # extra patterns never included in made patches
pager              = less
editor             = vi          # $EDITOR wins if set
```

**`index.tsv`** (tabs, newlines and backslashes inside fields escaped as `\t`, `\n`, `\\`):

```
id	name	author	target	rating	short_description
485	(New) Dumplog 1.0.0	Nax	NetHack 3.6.0	-	A rewrite of the famous dumplog patch...
397	messagetype option v0.2	Pasi Kallinen	NetHack 3.4.3	100	Add a new configuration option MSGTYPE
```

**`patch.meta` / `version.meta`:**

```ini
# patch.meta
id          = 485
name        = (New) Dumplog
author      = Nax
target      = NetHack 3.6.0
source_page = https://nhpatchdb.alt.org/?485

# version.meta
version       = 1.0.0
downloaded    = 2026-11-15T14:02:11Z
url           = https://nhpatchdb.alt.org/?download=485
via_wayback   = no
sha256        = 3f1c...
files_touched = src/end.c src/pline.c include/decl.h
```

**`source.meta`:**

```ini
version   = 3.6.7
origin    = tarball:/home/me/Downloads/nethack-367-src.tgz
added     = 2026-11-15T13:40:00Z
sha256    = 9a2f...          # of the original tarball, if any
```

`instance.conf` is described in section 10.2.

---
