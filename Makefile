CC      ?= cc
PREFIX  ?= /usr/local
DESTDIR ?=
CFLAGS  ?= -O2
BUILD_DIR := build

# What `nhpm --version` reports: the release tag in CI (release.yml passes
# VERSION=), else the nearest git tag/commit, else plain "0.1" outside a
# git checkout. Passed through to CMakeLists.txt, which has the same fallback.
VERSION ?= $(shell git describe --tags --always --dirty 2>/dev/null)
ifeq ($(strip $(VERSION)),)
VERSION := 1.0.0
endif

# Flags, pkg-config lookup and source list live in CMakeLists.txt (docs/style.md
# section 11); CFLAGS/LDLIBS here are just extra flags layered on top. Recursive
# (`=`): `debug` below overrides CFLAGS/LDLIBS/BUILD_DIR per-target.
CMAKE_CONFIGURE = cmake -S . -B $(BUILD_DIR) -DCMAKE_C_COMPILER="$(CC)" \
	-DCMAKE_C_FLAGS="$(CFLAGS)" -DCMAKE_EXE_LINKER_FLAGS="$(LDLIBS)" -DVERSION="$(VERSION)"

nhpm:
	$(CMAKE_CONFIGURE)
	cmake --build $(BUILD_DIR) --target nhpm

debug: BUILD_DIR := build/debug
debug: CFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDLIBS += -fsanitize=address,undefined
debug: nhpm

test:
	$(CMAKE_CONFIGURE)
	cmake --build $(BUILD_DIR) --target nhpm_tests
	ctest --test-dir $(BUILD_DIR) --output-on-failure

SRC := $(wildcard src/*.c src/ui/*.c)

FMT_FILES := $(wildcard src/*.c src/*.h src/ui/*.c src/ui/*.h tests/*.c tests/*.h)

# tests/run.sh, Makefile itself, CMakeLists.txt, and every workflow -- the
# '#'-comment half of the comment-length check below (docs/style.md 7/8/9/11).
HASH_COMMENT_FILES := tests/run.sh Makefile CMakeLists.txt $(wildcard .github/workflows/*.yml)
COMMENT_MAX_LINES  := 3

format:
	clang-format -i $(FMT_FILES)

BANNED := strcpy|strcat|sprintf|gets|strtok|atoi|atol|system|popen|tmpnam|mktemp

comment-length-check:
	@echo "== comment length (docs/style.md 6.10/7/8/9/11, max $(COMMENT_MAX_LINES) lines) =="
	@status=0; \
	awk -v max=$(COMMENT_MAX_LINES) 'FNR==1{in_c=0} !in_c&&/^[ \t]*\/\*/{in_c=1;start=FNR} \
		in_c&&/\*\//{span=FNR-start+1; if(span>max){printf "%s:%d: /* */ comment spans %d lines (max %d)\n",FILENAME,start,span,max; bad++}; in_c=0} \
		END{exit(bad>0)}' $(FMT_FILES) || status=1; \
	awk -v max=$(COMMENT_MAX_LINES) 'FNR==1{run=0} /^[ \t]*#/&&!/^#!/{if(run==0)start=FNR; run++; next} \
		{if(run>max){printf "%s:%d: # comment run spans %d lines (max %d)\n",FILENAME,start,run,max; bad++}; run=0} \
		END{if(run>max){printf "%s:%d: # comment run spans %d lines (max %d)\n",FILENAME,start,run,max; bad++}; exit(bad>0)}' \
		$(HASH_COMMENT_FILES) || status=1; \
	if [ $$status -ne 0 ]; then \
		echo "error: a comment exceeds $(COMMENT_MAX_LINES) lines, see docs/style.md section 6.10"; \
		exit 1; \
	fi

lint: comment-length-check
	@echo "== clang-format --dry-run --Werror =="
	clang-format --dry-run --Werror $(FMT_FILES)
	@echo "== banned functions (docs/style.md section 6.8) =="
	@! grep -nE '\b($(BANNED))\(' $(FMT_FILES) || \
		{ echo "error: a banned function is used, see docs/style.md section 6.8"; exit 1; }
	@if command -v clang-tidy >/dev/null 2>&1; then \
		echo "== clang-tidy =="; \
		$(CMAKE_CONFIGURE) >/dev/null; \
		clang-tidy $(SRC) -p $(BUILD_DIR); \
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
	DESTDIR="$(DESTDIR)" cmake --install $(BUILD_DIR) --prefix "$(PREFIX)"

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

.PHONY: nhpm debug test format lint comment-length-check install clean ci
