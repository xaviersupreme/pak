CC = clang
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
CPPFLAGS ?= -Iinclude

SRC = src/main.c src/archive.c src/io.c src/log.c src/endian.c src/crc.c src/compress.c
OBJ = $(SRC:.c=.o)
BIN = pak

ifeq ($(OS),Windows_NT)
BIN := pak.exe
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(OBJ) -o $@

%.o: %.c include/pak.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all clean