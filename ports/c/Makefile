# Build BAV C port with optional NASM kernels (default: on when nasm found)
CC ?= gcc
NASM ?= nasm
CFLAGS ?= -O3 -Wall -Wextra -I.
LDFLAGS ?= -lz

ROOT := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
NASM_SRC := $(ROOT)../nasm/bav_kernels.asm

# Windows MinGW: produce .obj via NASM win64
UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)
ifeq ($(OS),Windows_NT)
  EXE := .exe
  NASM_FMT := win64
  NASM_OBJ := bav_kernels.obj
else
  EXE :=
  NASM_FMT := elf64
  NASM_OBJ := bav_kernels.o
endif

.PHONY: all nasm clean test

all: bav-c$(EXE)

nasm: CFLAGS += -DBAV_USE_NASM
nasm: bav-c-nasm$(EXE)

bav-c$(EXE): bav.c main.c bav.h
	$(CC) $(CFLAGS) -o $@ bav.c main.c $(LDFLAGS)

$(NASM_OBJ): $(NASM_SRC)
	$(NASM) -f $(NASM_FMT) -o $@ $<

bav-c-nasm$(EXE): bav.c main.c bav.h $(NASM_OBJ)
	$(CC) $(CFLAGS) -DBAV_USE_NASM -o $@ bav.c main.c $(NASM_OBJ) $(LDFLAGS)

clean:
	rm -f bav-c bav-c.exe bav-c-nasm bav-c-nasm.exe *.o *.obj
