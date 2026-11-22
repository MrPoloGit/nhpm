# Makefile - fake NetHack source fixture (docs/testing.md section 17.2).
# Mimics the "make all", "make install PREFIX=..." shape of the real
# recipes (docs/features.md section 10.6) closely enough for build/play
# integration tests, without being a real NetHack build.
CC ?= cc
CFLAGS += -std=c11 -Wall -Iinclude

SRC = src/main.c src/light.c src/objects.c src/apply.c
BIN = game

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC)

install: $(BIN)
	mkdir -p $(PREFIX)/games
	cp $(BIN) $(PREFIX)/games/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all install clean
