#!/bin/sh
# run.sh - builds and runs the test suite. A thin wrapper around `make test`
# (see ../Makefile) so the tests can be run directly, e.g. from CI or an
# editor, without remembering the make target's name.
set -eu
cd "$(dirname "$0")/.."
exec make test
