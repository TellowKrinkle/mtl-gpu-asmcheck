CXX ?= clang++
CXXFLAGS += -fno-rtti -Iinclude -Isrc -std=c++17
LDFLAGS += -Llib -lLLVM

all: mtl-gpu-objdump mtl-gpu-llc mtl-gpu-asmcheck

mtl-gpu-objdump: src/llvm-objdump/COFFDump.cpp.o src/llvm-objdump/ELFDump.cpp.o src/llvm-objdump/llvm-objdump.cpp.o src/llvm-objdump/MachODump.cpp.o src/llvm-objdump/WasmDump.cpp.o src/llvm-objdump/MachOObjectFile.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS)

mtl-gpu-llc: src/llc/llc.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS)

mtl-gpu-asmcheck: src/asmcheck/main.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS)

%.cpp.o: %.cpp
	$(CXX) -MMD -c -o $@ $< $(CXXFLAGS)

-include *.d

.PHONY: clean all

clean:
	rm -f mtl-gpu-objdump mtl-gpu-llc mtl-gpu-asmcheck src/*/*.o src/*/*.d
