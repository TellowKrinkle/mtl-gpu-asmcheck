CXX ?= clang++
CXXFLAGS += -fno-rtti -fno-exceptions -Iinclude -Isrc -std=c++14
LDFLAGS += -Llib -lLLVM

mtl-gpu-objdump: src/llvm-objdump/COFFDump.cpp.o src/llvm-objdump/ELFDump.cpp.o src/llvm-objdump/llvm-objdump.cpp.o src/llvm-objdump/MachODump.cpp.o src/llvm-objdump/WasmDump.cpp.o src/llvm-objdump/MachOObjectFile.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS)

%.cpp.o: %.cpp
	$(CXX) -MMD -c -o $@ $< $(CXXFLAGS)

-include *.d

.PHONY: clean

clean:
	rm -f mtl-gpu-objdump src/*.o src/*.d
