CC_CANDIDATES = clang gcc cc

ifeq ($(origin CC),default)
ifeq ($(OS),Windows_NT)
CC := $(firstword $(foreach c,$(CC_CANDIDATES),$(if $(shell where $(c) 2>NUL),$(c))))
else
CC := $(firstword $(foreach c,$(CC_CANDIDATES),$(if $(shell command -v $(c) 2>/dev/null),$(c))))
endif
endif

ifeq ($(strip $(CC)),)
$(error no C compiler found. install clang, gcc, or cc, or run make CC=/path/to/compiler)
endif

CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
CPPFLAGS ?= -Iinclude -Ivendor/miniz

SRC = src/main.c src/archive.c src/io.c src/paths.c src/log.c src/endian.c src/crc.c src/compress.c vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c
OBJ = $(SRC:.c=.o)
BIN = pak

ifeq ($(OS),Windows_NT)
BIN := pak.exe
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

%.o: %.c include/pak.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

ifeq ($(OS),Windows_NT)
clean:
	-cmd /C "del /Q $(subst /,\,$(OBJ)) $(BIN) >NUL 2>&1"
else
clean:
	rm -f $(OBJ) $(BIN)
endif

.PHONY: all clean
