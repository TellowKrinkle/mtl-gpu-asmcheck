CXX ?= clang++
CXXFLAGS += -fno-rtti -fno-exceptions -Iinclude -Isrc -std=c++14
LDFLAGS += -Llib -lLLVM

mtl-gpu-objdump: src/COFFDump.cpp.o src/ELFDump.cpp.o src/llvm-objdump.cpp.o src/MachODump.cpp.o src/WasmDump.cpp.o src/MachOObjectFile.cpp.o
	$(CXX) -o $@ $^ $(LDFLAGS)

%.cpp.o: %.cpp
	$(CXX) -MMD -c -o $@ $< $(CXXFLAGS)

-include *.d

.PHONY: clean

clean:
	rm -f mtl-gpu-objdump src/*.o src/*.d
