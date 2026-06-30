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

ifeq ($(OS),Windows_NT)
HOST = windows
PYTHON_CANDIDATES = python python3
TEST_PAK = .\$(BIN)
else
HOST = unix
PYTHON_CANDIDATES = python3 python
TEST_PAK = ./$(BIN)
endif

ifeq ($(origin PYTHON),undefined)
ifeq ($(OS),Windows_NT)
PYTHON := $(if $(shell where py 2>NUL),py -3,$(firstword $(foreach p,$(PYTHON_CANDIDATES),$(if $(shell where $(p) 2>NUL),$(p)))))
else
PYTHON := $(firstword $(foreach p,$(PYTHON_CANDIDATES),$(if $(shell command -v $(p) 2>/dev/null),$(p))))
endif
endif

CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -O2
LDFLAGS ?=
CPPFLAGS ?= -Iinclude -Ivendor/miniz -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES

SRC = src/cli/main.c src/cli/hints.c src/archive/core.c src/archive/check.c src/codec/compress.c src/codec/crc.c src/fs/io.c src/fs/paths.c src/fs/pattern.c src/output/log.c src/output/diag.c src/format/endian.c vendor/miniz/miniz.c vendor/miniz/miniz_tdef.c vendor/miniz/miniz_tinfl.c
LIB_SRC = $(filter-out src/cli/main.c,$(SRC))
BUILD_DIR = build/$(HOST)
OBJ = $(SRC:%.c=$(BUILD_DIR)/%.o)
BIN = pak
FUZZ_CC ?= clang
FUZZ_DIR = fuzz
FUZZ_BIN_DIR = $(FUZZ_DIR)/bin
FUZZ_CORPUS_DIR = $(FUZZ_DIR)/corpus/archive
FUZZ_BIN_DIR_WIN = fuzz\bin
FUZZ_CORPUS_DIR_WIN = fuzz\corpus\archive
FUZZ_BIN = $(FUZZ_BIN_DIR)/archive_fuzz

ifeq ($(OS),Windows_NT)
BIN := pak.exe
FUZZ_BIN := $(FUZZ_BIN_DIR)/archive_fuzz.exe
DEFAULT_FUZZ_SANITIZERS = fuzzer
FUZZ_RUNTIME_DIR := $(shell $(FUZZ_CC) --print-resource-dir)/lib/windows
DEFAULT_FUZZ_LDLIBS = "$(FUZZ_RUNTIME_DIR)/clang_rt.stats-x86_64.lib" -lShell32 -lDbghelp -lmincore
else
DEFAULT_FUZZ_SANITIZERS = fuzzer,address
DEFAULT_FUZZ_LDLIBS =
endif

FUZZ_SANITIZERS ?= $(DEFAULT_FUZZ_SANITIZERS)
FUZZ_FLAGS ?= -g -O1 -fsanitize=$(FUZZ_SANITIZERS)
FUZZ_LDLIBS ?= $(DEFAULT_FUZZ_LDLIBS)
FUZZ_ITERS ?= 10000
FUZZ_SMOKE_BIN = $(FUZZ_BIN_DIR)/archive_fuzz_smoke

ifeq ($(OS),Windows_NT)
FUZZ_SMOKE_BIN := $(FUZZ_BIN_DIR)/archive_fuzz_smoke.exe
endif

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(LDFLAGS) $(OBJ) -o $@

test: $(BIN)
	$(if $(strip $(PYTHON)),,$(error no Python 3 found. install Python 3 or run make PYTHON=/path/to/python test))
	$(PYTHON) tests/test_cli.py --pak $(TEST_PAK)

fuzz: fuzz-smoke

fuzz-libfuzzer: fuzz/archive_fuzz.c $(LIB_SRC) include/pak.h
ifeq ($(OS),Windows_NT)
	-if not exist $(FUZZ_BIN_DIR_WIN) mkdir $(FUZZ_BIN_DIR_WIN)
else
	mkdir -p $(FUZZ_BIN_DIR)
endif
	$(FUZZ_CC) $(CPPFLAGS) $(CFLAGS) $(FUZZ_FLAGS) fuzz/archive_fuzz.c $(LIB_SRC) $(FUZZ_LDLIBS) -o $(FUZZ_BIN)

fuzz-smoke: fuzz/archive_fuzz.c $(LIB_SRC) include/pak.h
ifeq ($(OS),Windows_NT)
	-if not exist $(FUZZ_BIN_DIR_WIN) mkdir $(FUZZ_BIN_DIR_WIN)
else
	mkdir -p $(FUZZ_BIN_DIR)
endif
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPAK_FUZZ_STANDALONE fuzz/archive_fuzz.c $(LIB_SRC) -o $(FUZZ_SMOKE_BIN)
	$(FUZZ_SMOKE_BIN) $(FUZZ_ITERS)

ifeq ($(OS),Windows_NT)
fuzz-seed: $(BIN)
	-if not exist $(FUZZ_CORPUS_DIR_WIN) mkdir $(FUZZ_CORPUS_DIR_WIN)
	$(BIN) make $(FUZZ_CORPUS_DIR_WIN)\basic README.md >NUL
	$(BIN) make --compress --paths $(FUZZ_CORPUS_DIR_WIN)\compressed README.md include\pak.h >NUL
else
fuzz-seed: $(BIN)
	mkdir -p $(FUZZ_CORPUS_DIR)
	./$(BIN) make $(FUZZ_CORPUS_DIR)/basic README.md >/dev/null
	./$(BIN) make --compress --paths $(FUZZ_CORPUS_DIR)/compressed README.md include/pak.h >/dev/null
endif

fuzz-run: fuzz-run-libfuzzer

fuzz-run-libfuzzer: fuzz-seed fuzz-libfuzzer
ifeq ($(OS),Windows_NT)
	$(FUZZ_BIN) $(FUZZ_CORPUS_DIR_WIN) -max_total_time=15
else
	./$(FUZZ_BIN) $(FUZZ_CORPUS_DIR) -max_total_time=15
endif

$(BUILD_DIR)/%.o: %.c include/pak.h src/archive/internal.h
ifeq ($(OS),Windows_NT)
	-if not exist "$(subst /,\,$(@D))" mkdir "$(subst /,\,$(@D))"
else
	mkdir -p $(@D)
endif
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

ifeq ($(OS),Windows_NT)
clean:
	-cmd /C "if exist build rmdir /S /Q build & if exist pak.exe del /Q pak.exe & if exist pak del /Q pak"
	-cmd /C "for /R src %%F in (*.o) do del /Q %%F"
	-cmd /C "for /R vendor %%F in (*.o) do del /Q %%F"
	-cmd /C "if exist fuzz\bin rmdir /S /Q fuzz\bin & if exist fuzz\corpus rmdir /S /Q fuzz\corpus"
else
clean:
	rm -f $(BIN) $(FUZZ_BIN) $(FUZZ_SMOKE_BIN)
	rm -rf build
	find src vendor -name '*.o' -delete
	rm -rf $(FUZZ_BIN_DIR) fuzz/corpus
endif

.PHONY: all clean test fuzz fuzz-libfuzzer fuzz-run fuzz-run-libfuzzer fuzz-seed fuzz-smoke
