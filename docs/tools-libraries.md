# Tools and libraries

nhpm leans on existing command-line tools rather than reimplementing them
([security.md](security.md), [repo-structure.md](repo-structure.md)), so there are two
separate lists: what you need to **build** nhpm, and what nhpm itself **shells out to
at runtime** to actually do its job. Both matter: a machine with every build dependency but
no `quilt` will compile a `nhpm` binary that can't manage a patch stack.

## Contents

1. [To build nhpm](#1-to-build-nhpm)
2. [To run nhpm](#2-to-run-nhpm)
3. [Platform notes](#3-platform-notes)

---

## 1. To build nhpm

| Dependency | Why |
|---|---|
| A C11 compiler | GCC or Clang |
| **ncursesw** + **panel** (dev headers) | the terminal interface (wide-character build for UTF-8; panel library for stacked dialogs) |
| libcurl (dev headers) | HTTPS downloads from the Patch Database |
| libxml2 (dev headers) | HTML parsing with XPath |
| pkg-config | the Makefile uses it to find the above |

```sh
# macOS (Homebrew)
brew install ncurses curl libxml2 pkg-config

# Debian/Ubuntu
sudo apt install build-essential libncurses-dev libcurl4-openssl-dev libxml2-dev pkg-config

# Fedora/RHEL
sudo dnf install gcc make ncurses-devel libcurl-devel libxml2-devel pkgconf-pkg-config

# Arch
sudo pacman -S base-devel ncurses curl libxml2 pkgconf
```

See [the Platform notes](#3-platform-notes) below for why macOS needs a bit more care here.

---

## 2. To run nhpm

These are **runtime** dependencies: nhpm's own startup check only looks for `quilt` and a
working `patch`, but the features below silently need the rest.

| Tool | Why | Where it's used |
|---|---|---|
| **quilt** | the patch stack underneath every instance | [features.md](features.md), section 11 |
| **GNU patch** | dry runs during strip-level detection | [features.md](features.md), section 11.4; must be a real GNU patch, not macOS's bundled BSD-derived one, see below |
| **tar**, **gzip**, **unzip** | extracting library downloads and base-source tarballs | [features.md](features.md), sections 8, 9.2 |
| **GNU diff** (diffutils) | generating combined patches and your own patches | [features.md](features.md), section 12 |
| **git** (optional) | base sources from a git tag; your own patches from a git checkout | [features.md](features.md), sections 9.2, 12.6 |
| a C compiler + **make** | building the NetHack instance itself | [features.md](features.md), section 10.6; usually the same toolchain as building nhpm, but note below |
| **\$EDITOR** | manual conflict resolution; editing/capturing your own patches | [features.md](features.md), sections 11.6, 12; falls back to `vi` if unset |

At startup nhpm checks that `quilt` and `patch` are on `PATH` and explains what's missing
instead of failing deep inside some later operation.

```sh
# macOS (Homebrew) -- also pulls in GNU patch, see Platform notes
brew install quilt

# Debian/Ubuntu
sudo apt install quilt patch diffutils tar gzip unzip git

# Fedora/RHEL
sudo dnf install quilt patch diffutils tar gzip unzip git

# Arch
sudo pacman -S quilt patch diffutils tar gzip unzip git
```

`tar`, `gzip` and `unzip` ship by default on essentially every Linux distribution and on
macOS, so they're rarely something you need to install by hand.

---

## 3. Platform notes

### macOS: the system `patch` is not GNU patch

macOS's bundled `/usr/bin/patch` is an old BSD-derived patch with Apple's own
modifications (`patch --version` reports something like `2.0-12u11-Apple`). It doesn't
reliably produce the output nhpm's strip-level detection parses for (`Hunk #N FAILED`,
`succeeded ... with fuzz`, `can't find file to patch`; see [features.md](features.md), section
11.4), and `quilt` itself needs real GNU patch to work correctly.

Installing `quilt` with Homebrew pulls in GNU patch automatically as a dependency, but
Homebrew installs it **keg-only** under the name `gpatch`, so it won't shadow
`/usr/bin/patch` on `PATH` by itself. nhpm's own PATH check looks for a working GNU patch
under either name (`patch` or `gpatch`) and tells you to run:

```sh
brew install quilt     # installs gpatch as a dependency
```

if neither is found. If you'd rather have `patch` itself resolve to the GNU one, put
Homebrew's `gpatch` directory ahead of `/usr/bin` on your `PATH`, or symlink it:

```sh
ln -s "$(brew --prefix gpatch)/bin/gpatch" "$(brew --prefix)/bin/patch"
```

### A separate C toolchain for old NetHack versions

Building nhpm and building the NetHack instances it manages are two different jobs that
happen to usually share a toolchain. They don't have to: a base source as old as 3.4.3
sometimes needs tooling (older `make`, specific warnings suppressed) that today's default
`cc` on your distribution has dropped support for. If an instance's `build` fails for reasons
that look toolchain-related rather than patch-related, check the recipe
([features.md](features.md), section 10.6) before assuming nhpm itself is at fault.
