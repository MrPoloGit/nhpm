CC      ?= cc
PREFIX  ?= /usr/local
DESTDIR ?=

# On macOS, Homebrew's ncurses is keg-only (it would conflict with the
# system one), so its .pc files -- in particular panelw.pc, which the system
# ncurses shim doesn't provide at all -- aren't on the default pkg-config
# path. Add them when present, as docs/nhpm-plan.md section 1 calls for
# ("the build uses Homebrew's ncurses through pkg-config when available").
# This has to be passed to pkg-config's own environment inline (rather than
# via "export", which $(shell ...) calls made while parsing the Makefile
# don't see on every `make` version) since PKG_CFLAGS/PKG_LIBS below are
# themselves resolved with $(shell ...) at parse time.
BREW_NCURSES_PREFIX := $(shell command -v brew >/dev/null 2>&1 && brew --prefix ncurses 2>/dev/null)
ifneq ($(strip $(BREW_NCURSES_PREFIX)),)
PKG_CONFIG_PATH_EXTRA := $(BREW_NCURSES_PREFIX)/lib/pkgconfig
endif

# M1 adds libcurl/libxml-2.0 (net.c, site.c); M7 adds ncursesw/panelw (src/ui).
# Linking against them now, even though nothing in src/ calls into them yet,
# matches the final shape of the project (docs/nhpm-plan.md section 1) and
# avoids rewriting this file milestone by milestone.
PKGS       := ncursesw panelw libcurl libxml-2.0
PKG_CONFIG := PKG_CONFIG_PATH="$(PKG_CONFIG_PATH_EXTRA):$$PKG_CONFIG_PATH" pkg-config
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lncursesw -lpanelw -lcurl -lxml2
endif

WARN = -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
       -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings -Wnull-dereference -Wdouble-promotion

CFLAGS  ?= -O2
# -MMD -MP: alongside each .o, write a .d file listing the headers it
# actually included, so editing a header rebuilds every .c that depends
# on it -- not just the ones that happen to #include it directly. Without
# this, `make` after a header-only change can silently link some
# translation units against the old layout and others against the new
# one (same struct, different member offsets in each .o), which doesn't
# fail to build -- it produces a binary that reads its own data at the
# wrong offsets and corrupts or crashes at runtime, far from the edit
# that caused it.
override CFLAGS += -std=c11 $(WARN) -Isrc $(PKG_CFLAGS) -MMD -MP
override LDLIBS += $(PKG_LIBS)

SRC      := $(wildcard src/*.c src/ui/*.c)
MAIN_SRC := src/main.c
LIB_SRC  := $(filter-out $(MAIN_SRC),$(SRC))

OBJDIR  := build/obj
OBJ     := $(patsubst src/%.c,$(OBJDIR)/%.o,$(SRC))
LIB_OBJ := $(patsubst src/%.c,$(OBJDIR)/%.o,$(LIB_SRC))

nhpm: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJDIR)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(wildcard build/obj/*.d build/obj-debug/*.d)

debug: CFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDLIBS += -fsanitize=address,undefined
debug: OBJDIR := build/obj-debug
debug: nhpm

TEST_SRC  := $(wildcard tests/test_*.c)
TEST_BINS := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		$$t || status=1; \
	done; \
	exit $$status

build/tests/%: tests/%.c $(LIB_OBJ)
	@mkdir -p build/tests
	$(CC) $(CFLAGS) -o $@ $< $(LIB_OBJ) $(LDLIBS)

FMT_FILES := $(wildcard src/*.c src/*.h src/ui/*.c src/ui/*.h tests/*.c tests/*.h)

format:
	clang-format -i $(FMT_FILES)

BANNED := strcpy|strcat|sprintf|gets|strtok|atoi|atol|system|popen|tmpnam|mktemp

lint:
	@echo "== clang-format --dry-run --Werror =="
	clang-format --dry-run --Werror $(FMT_FILES)
	@echo "== banned functions (docs/style.md section 6.8) =="
	@! grep -nE '\b($(BANNED))\(' $(FMT_FILES) || \
		{ echo "error: a banned function is used, see docs/style.md section 6.8"; exit 1; }
	@if command -v clang-tidy >/dev/null 2>&1; then \
		echo "== clang-tidy =="; \
		clang-tidy $(SRC) -- $(CFLAGS); \
	else \
		echo "clang-tidy not found locally; it runs in CI"; \
	fi
	@if command -v cppcheck >/dev/null 2>&1; then \
		echo "== cppcheck =="; \
		cppcheck --inline-suppr --enable=warning,style --error-exitcode=1 -Isrc $(SRC); \
	else \
		echo "cppcheck not found locally; it runs in CI"; \
	fi

install: nhpm
	install -Dm755 nhpm $(DESTDIR)$(PREFIX)/bin/nhpm
	install -Dm644 doc/nhpm.1 $(DESTDIR)$(PREFIX)/share/man/man1/nhpm.1

clean:
	rm -rf build nhpm

# Runs the same steps as .github/workflows/ci.yml (lint, build, test, then
# test again under ASan/UBSan). With an explicit CC= (e.g. to work around
# Apple clang's ASan deadlock, see CONTRIBUTING.md), runs with that compiler
# only; otherwise once per compiler actually available here.
ifeq ($(origin CC),command line)
CI_CCS := $(CC)
else ifeq ($(origin CC),environment)
CI_CCS := $(CC)
else
CI_CCS := $(strip $(foreach c,gcc clang,$(if $(shell command -v $(c) 2>/dev/null),$(c))))
endif

ci:
	@if [ -z "$(CI_CCS)" ]; then \
		echo "error: no usable compiler (neither gcc nor clang found on PATH)"; exit 1; \
	fi
	@for cc in $(CI_CCS); do \
		echo "== CC=$$cc: lint =="; \
		$(MAKE) lint CC=$$cc CFLAGS="-Werror" || exit 1; \
		echo "== CC=$$cc: build =="; \
		$(MAKE) CC=$$cc CFLAGS="-Werror" || exit 1; \
		echo "== CC=$$cc: test =="; \
		$(MAKE) test CC=$$cc CFLAGS="-Werror" || exit 1; \
		$(MAKE) clean >/dev/null; \
		echo "== CC=$$cc: test under ASan/UBSan =="; \
		$(MAKE) test CC=$$cc CFLAGS="-O0 -g -fsanitize=address,undefined -Werror" \
			LDLIBS="-fsanitize=address,undefined" || exit 1; \
		$(MAKE) clean >/dev/null; \
	done
	@echo "ci: all checks passed ($(CI_CCS))"

.PHONY: debug test format lint install clean ci
