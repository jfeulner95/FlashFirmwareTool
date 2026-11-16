# fwtool - Makefile for Linux (gcc/clang) and Windows (MinGW-w64)
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=c99
CFLAGS += -D__USE_MINGW_ANSI_STDIO=1

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

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

clean:
	-$(RM) $(TARGET)

.PHONY: all clean
