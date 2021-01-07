# mtl-gpu-asmcheck
Tool to use Apple's private GPUCompiler libLLVM.dylib to assemble LLVM IR and print instruction metadata

Note: The version of libLLVM included in macOS (at least 11.0.1 20B29) does not include opcode names, so they will be printed as the internal opcode IDs.  It does, however, include register names.

There's also a mtl-gpu-llc and mtl-gpu-objdump here, which are the given tools from the LLVM project cut up until they would compile against the reduced set of symbols included in Apple's libLLVM.  The resulting llc works but only for creating binaries, and the objdump doesn't really work, but I left them in because whatever.

## Compiling
Use the supplied makefile (just run `make`).  Should compile on both x86 and arm machines, though I only have an x86 machine to test

## Notes
Files are based off LLVM version 7.0.0.  Not exactly sure what version the actual thing is based off of, but it's missing some changes added to llvm 8, but also has some changes that weren't included in llvm 7 (upstreamed by Apple devs) so I'm guessing it's based off llvm 7
