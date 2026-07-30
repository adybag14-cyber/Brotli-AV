CXX ?= g++
NASM ?= nasm
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra
LDFLAGS ?= -lz

ifeq ($(OS),Windows_NT)
  EXE := .exe
  NASM_FMT := win64
  NASM_OBJ := ../c/bav_kernels.obj
else
  EXE :=
  NASM_FMT := elf64
  NASM_OBJ := ../c/bav_kernels.o
endif

.PHONY: all nasm clean

all: bav-cpp$(EXE)

bav-cpp$(EXE): bav.cpp main.cpp bav.hpp
	$(CXX) $(CXXFLAGS) -o $@ bav.cpp main.cpp $(LDFLAGS)

nasm: bav-cpp-nasm$(EXE)

../c/bav_kernels.obj: ../nasm/bav_kernels.asm
	$(NASM) -f $(NASM_FMT) -o $@ $<

../c/bav_kernels.o: ../nasm/bav_kernels.asm
	$(NASM) -f $(NASM_FMT) -o $@ $<

bav-cpp-nasm$(EXE): bav.cpp main.cpp bav.hpp $(NASM_OBJ)
	$(CXX) $(CXXFLAGS) -DBAV_USE_NASM -o $@ bav.cpp main.cpp $(NASM_OBJ) $(LDFLAGS)

clean:
	rm -f bav-cpp bav-cpp.exe bav-cpp-nasm bav-cpp-nasm.exe
