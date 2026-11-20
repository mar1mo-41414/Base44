# Base44 — GNU base64 / base32 と同形式の CLI (C11、依存は libc のみ)

CC = cc
CFLAGS = -O2 -std=c11 -Wall -Wextra

# インストール先: デフォルトは ~/.local/bin/My
# 例: make install PREFIX=/usr/local BINDIR=/usr/local/bin
#     make install DESTDIR=/tmp/stage
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin/My
DESTDIR ?=

TARGET = base44
OBJS = src/base44.o src/main.o
CORE_TEST = test/core_test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

src/base44.o: src/base44.c src/base44.h
	$(CC) $(CFLAGS) -c -o $@ src/base44.c

src/main.o: src/main.c src/base44.h
	$(CC) $(CFLAGS) -c -o $@ src/main.c

$(CORE_TEST): test/core_test.c src/base44.c src/base44.h
	$(CC) $(CFLAGS) -o $@ test/core_test.c src/base44.c

test: $(TARGET) $(CORE_TEST)
	./$(CORE_TEST)
	python3 test/test_base44.py

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) $(CORE_TEST)

.PHONY: all test install uninstall clean
