#!/bin/sh
# setup.sh - no-op stand-in for NetHack's real sys/unix/setup.sh, just
# enough for build.c's "nethack-3.4-unix" recipe step to run successfully
# against this fake source fixture (docs/testing.md section 17.2). The
# real setup.sh generates Makefiles from hint files; this fixture's
# Makefile needs no generating, so there's nothing to do.
exit 0
