CC      ?= cc
PREFIX  ?= /usr/local
DESTDIR ?=

# What `nhpm --version` reports: the release tag in CI (release.yml passes
# VERSION=), else the nearest git tag/commit, else plain "0.1" outside a
# git checkout.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null)
ifeq ($(strip $(VERSION)),)
VERSION := 0.1
endif

# Homebrew's ncurses is keg-only; panelw.pc isn't on the default pkg-config
# path (docs/tools-libraries.md section 3), so add it when present. Passed
# inline, not "export"ed, since $(shell ...) below may not see that.
BREW_NCURSES_PREFIX := $(shell command -v brew >/dev/null 2>&1 && brew --prefix ncurses 2>/dev/null)
ifneq ($(strip $(BREW_NCURSES_PREFIX)),)
PKG_CONFIG_PATH_EXTRA := $(BREW_NCURSES_PREFIX)/lib/pkgconfig
endif

# M1 adds libcurl/libxml-2.0, M7 adds ncursesw/panelw; linking against all
# of them now matches the project's final shape (docs/nhpm-plan.md section 1)
# instead of rewriting this file milestone by milestone.
PKGS       := ncursesw panelw libcurl libxml-2.0
PKG_CONFIG := PKG_CONFIG_PATH="$(PKG_CONFIG_PATH_EXTRA):$$PKG_CONFIG_PATH" pkg-config
PKG_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell $(PKG_CONFIG) --libs $(PKGS) 2>/dev/null)
ifeq ($(strip $(PKG_LIBS)),)
PKG_LIBS := -lncursesw -lpanelw -lcurl -lxml2
endif

WARN := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
        -Wformat=2 -Wundef -Wcast-qual -Wwrite-strings -Wnull-dereference -Wdouble-promotion

CFLAGS  ?= -O2
# -MMD -MP: per-.o header dependency tracking, so a header-only edit
# rebuilds every .c that depends on it, not just direct #includes -- see
# docs/nhpm-plan.md section 1 for why a stale .o here is dangerous.
override CFLAGS += -std=c11 $(WARN) -Isrc $(PKG_CFLAGS) -MMD -MP -DNHPM_VERSION='"$(VERSION)"'
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

# Runs the same steps as .github/workflows/ci.yml, per compiler available
# (or just CC= if given explicitly, e.g. for Apple clang's ASan deadlock --
# see CONTRIBUTING.md).
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
