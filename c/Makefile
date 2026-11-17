# fwtool - Makefile for Linux (gcc/clang) and Windows (MinGW-w64)
#   make             build
#   make WERROR=1    build with warnings treated as errors (used by CI)
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += -D__USE_MINGW_ANSI_STDIO=1

# Single source of truth for the version: ../VERSION
# ($(file) needs GNU make 4.2+; older makes, e.g. macOS's 3.81, fall back to cat)
VERSION := $(strip $(file <../VERSION))
ifeq ($(VERSION),)
    VERSION := $(strip $(shell cat ../VERSION))
endif
CFLAGS += -DFWTOOL_VERSION=\"$(VERSION)\"

ifeq ($(WERROR),1)
    CFLAGS += -Werror
endif

SRC = src/fwtool.c src/flash.c src/serial_win.c src/serial_posix.c
HDR = src/flash.h src/serial.h

ifeq ($(OS),Windows_NT)
    TARGET = fwtool.exe
    LDLIBS = -ladvapi32
    RM     = del /Q
else
    TARGET = fwtool
    LDLIBS =
    RM     = rm -f
endif

all: $(TARGET)

$(TARGET): $(SRC) $(HDR) ../VERSION
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	-$(RM) $(TARGET)

.PHONY: all clean
