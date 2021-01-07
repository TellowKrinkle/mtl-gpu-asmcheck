#include "llvm/ADT/Triple.h"
//#include "llvm/CodeGen/CommandFlags.inc"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"


#include <utility>

using namespace llvm;

class PassPrinter : public PassRegistrationListener {
	void passEnumerate(const PassInfo *Info) override {
		auto cur = outs().tell();
		outs() << Info->getPassArgument() << ": ";
		while (outs().tell() - cur < 32) { outs().write(' '); }
		outs() << Info->getPassName() << '\n';
	}
};

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input bitcode>"), cl::init("-"));

static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"), cl::value_desc("filename"));

static cl::opt<char> OptLevel("O",
	cl::desc("Optimization level. [-O0, -O1, -O2, or -O3] (default = '-O2')"),
	cl::Prefix,
	cl::ZeroOrMore,
	cl::init(' '));

static void initialize(PassRegistry& registry);
static std::pair<const Target*, std::unique_ptr<TargetMachine>> getAGX2TargetMachine();
static void compileModule(LLVMContext& ctx, const Target& target, TargetMachine& tm);

static const char* progName;

struct exit_exception : public std::exception {};

Target::MCCodeEmitterCtorTy actualCodeEmitterConstructor;
MCCodeEmitter *replacementCodeEmitterConstructor(const MCInstrInfo& II, const MCRegisterInfo& MRI, MCContext& Ctx);

int main(int argc, char **argv) {
	try {
		progName = argv[0];
		InitLLVM X(argc, argv);

		LLVMContext context;

		PassRegistry& registry = *PassRegistry::getPassRegistry();

		initialize(registry);

		cl::ParseCommandLineOptions(argc, argv, "Metal GPU ASM Checker");

		PassPrinter printer;
		// registry.enumerateWith(&printer);

		auto [target, tm] = getAGX2TargetMachine();

		actualCodeEmitterConstructor = target->MCCodeEmitterCtorFn;
		const_cast<Target*>(target)->MCCodeEmitterCtorFn = replacementCodeEmitterConstructor;

		compileModule(context, *target, *tm);
	} catch (exit_exception& e) {
		return 1;
	}
	return 0;
}


static void initialize(PassRegistry& registry)  {
	InitializeAllTargets();
	InitializeAllTargetMCs();
	InitializeAllAsmPrinters();
	InitializeAllAsmParsers();

	initializeCore(registry);
	initializeScalarOpts(registry);
	initializeVectorization(registry);
	initializeIPO(registry);
	initializeAnalysis(registry);
	initializeTransformUtils(registry);
	initializeInstCombine(registry);
	initializeTarget(registry);
}

static std::pair<const Target*, std::unique_ptr<TargetMachine>> getAGX2TargetMachine() {
	std::string error;
	Triple triple;
	StringRef features = "";
	auto target = TargetRegistry::lookupTarget("agx2", triple, error);
	if (!target) {
		throw std::runtime_error(error);
	}

	// Note: For some reason Apple included llvm/CodeGen/CommandFlags.inc in some file in their libLLVM, so if we also include it we'll double-register its command line arguments, causing a runtime exception.  If you really want to have access to the llc-style command line arguments, you can add a call to `llvm::cl::ResetCommandLineParser()` in a static constructor before including CommandFlags.inc (though that will break other things), then you can use InitTargetOptionsFromCodeGenFlags()

	TargetOptions opts;

	CodeGenOpt::Level OLvl = CodeGenOpt::Default;
	switch (OptLevel) {
		default:
			WithColor::error(errs(), progName) << "invalid optimization level.\n";
			throw exit_exception();
		case ' ': break;
		case '0': OLvl = CodeGenOpt::None; break;
		case '1': OLvl = CodeGenOpt::Less; break;
		case '2': OLvl = CodeGenOpt::Default; break;
		case '3': OLvl = CodeGenOpt::Aggressive; break;
	}

	// Note: Available processors: g10, g10p-b0, g11, g11g-a0, g11g-b0, g11m-a0, g11m-b0, g11p-a0, g11p-b0, g12, g12g-a0, g12m-a0, g12p-a0, g12p-b0, g12x, g13, g13-fullf32, g13g-a0, g13g-b0, g13g-b0-nofullf32, g13p-a0, g13x, g13x-a0

	std::unique_ptr<TargetMachine> targetMachine(target->createTargetMachine(triple.getTriple(), "g13x", features, opts, None, None, OLvl));
	return {target, std::move(targetMachine)};
}

static void printHex(raw_ostream& os, uint8_t byte) {
	const char* hexEnc = "0123456789abcdef";
	os << hexEnc[byte / 16] << hexEnc[byte % 16];
}

class CodeEmitterWrapper : public MCCodeEmitter {
	std::unique_ptr<MCCodeEmitter> actual;
	const MCInstrInfo& ii;
	const MCRegisterInfo& mri;
public:
	CodeEmitterWrapper(std::unique_ptr<MCCodeEmitter> actual, const MCInstrInfo& ii, const MCRegisterInfo& mri): actual(std::move(actual)), ii(ii), mri(mri) {}

	void reset() override { actual->reset(); }

	class AutoArrayBrackets {
		const bool needed;
		bool needsComma = false;

	public:
		AutoArrayBrackets(size_t count): needed(count > 1) {
			if (needed) {
				outs() << "[";
			}
		}
		~AutoArrayBrackets() {
			if (needed) {
				outs() << "]";
			}
		}

		void comma() {
			if (needsComma) {
				outs() << ", ";
			} else {
				needsComma = true;
			}
		}
	};

	void encodeInstruction(const MCInst& inst_, raw_ostream& os, SmallVectorImpl<MCFixup>& fixups, const MCSubtargetInfo& sti) const override {
		MCInst inst = inst_;
		uint64_t pos = outs().tell();
		outs() << "Encoding Op #" << inst.getOpcode();
		const MCInstrDesc& info = ii.get(inst.getOpcode());
		for (int i = 0; i < inst.getNumOperands(); i++) {
			MCOperand& operand = inst.getOperand(i);
			if (i == 0) {
				outs() << " ";
			} else {
				outs() << ", ";
			}
			if (!operand.isValid()) {
				outs() << "<InvalidOperand>";
			} else if (operand.isReg()) {
				outs() << mri.getName(operand.getReg());
			} else if (operand.isImm()) {
				if ((int64_t)(int16_t)operand.getImm() != operand.getImm()) {
					// Display large numbers as hex
					outs() << "0x";
					outs().write_hex(operand.getImm());
				} else {
					outs() << operand.getImm();
				}
			} else if (operand.isFPImm()) {
				outs() << operand.getFPImm() << "f";
			} else if (operand.isExpr()) {
				outs() << "<Expr " << operand.getExpr() << ">";
			} else if (operand.isInst()) {
				outs() << "<SubInst>";
			} else {
				outs() << "<UnrecognizedOperand>";
			}
		}
		for (uint64_t i = outs().tell(); i < pos + 60; i++) {
			outs().write(' ');
		}
		outs() << " ; Size: " << info.getSize() << ", Defs: " << info.getNumDefs();
		if (info.getNumOperands() != inst.getNumOperands()) {
			outs() << ", NumOperands: " << info.getNumOperands();
		}
		if (info.getSchedClass()) {
			outs() << ", SchedClass: " << info.getSchedClass();
		}
		if (info.TSFlags) {
			outs() << ", TSFlags: ";
			outs().write_hex(info.TSFlags);
		}
		uint64_t flagsMinusAllocReq = info.Flags;
		// All AGX2 insts I've seen have these, so invert these flags
		flagsMinusAllocReq ^= 1ULL << MCID::ExtraSrcRegAllocReq;
		flagsMinusAllocReq ^= 1ULL << MCID::ExtraDefRegAllocReq;
		if (flagsMinusAllocReq) {
			outs() << ", Flags: ";
			AutoArrayBrackets brackets(__builtin_popcountl(flagsMinusAllocReq));
#define TEST(x) if (flagsMinusAllocReq & (1ULL << MCID::x)) { brackets.comma(); outs() << #x;  }
			TEST(Variadic)
			TEST(HasOptionalDef)
			TEST(Pseudo)
			TEST(Return)
			TEST(Call)
			TEST(Barrier)
			TEST(Terminator)
			TEST(Branch)
			TEST(IndirectBranch)
			TEST(Compare)
			TEST(MoveImm)
			TEST(MoveReg)
			TEST(Bitcast)
			TEST(Select)
			TEST(DelaySlot)
			TEST(FoldableAsLoad)
			TEST(MayLoad)
			TEST(MayStore)
			TEST(Predicable)
			TEST(NotDuplicable)
			TEST(UnmodeledSideEffects)
			TEST(Commutable)
			TEST(ConvertibleTo3Addr)
			TEST(UsesCustomInserter)
			TEST(HasPostISelHook)
			TEST(Rematerializable)
			TEST(CheapAsAMove)
			if (flagsMinusAllocReq & (1ULL << MCID::ExtraSrcRegAllocReq)) { brackets.comma(); outs() << "!ExtraSrcRegAllocReq"; }
			if (flagsMinusAllocReq & (1ULL << MCID::ExtraDefRegAllocReq)) { brackets.comma(); outs() << "!ExtraDefRegAllocReq"; }
			TEST(RegSequence)
			TEST(ExtractSubreg)
			TEST(InsertSubreg)
			TEST(Convergent)
			TEST(Add)
			TEST(Trap)
#undef TEST
		}
		if (info.getNumImplicitUses()) {
			outs() << ", ImplicitUses: ";
			AutoArrayBrackets brackets(info.getNumImplicitUses());
			for (unsigned i = 0; i < info.getNumImplicitUses(); i++) {
				brackets.comma();
				outs() << mri.getName(info.getImplicitUses()[i]);
			}
		}
		if (info.getNumImplicitDefs()) {
			outs() << ", ImplicitDefs: ";
			AutoArrayBrackets brackets(info.getNumImplicitDefs());
			for (unsigned i = 0; i < info.getNumImplicitDefs(); i++) {
				brackets.comma();
				outs() << mri.getName(info.getImplicitDefs()[i]);
			}
		}
		outs() << "\n";

		SmallVector<char, 32> binout;
		raw_svector_ostream bos(binout);
		actual->encodeInstruction(inst, bos, fixups, sti);
		bos.str(); // flush

		outs() << "\tResult: ";
		for (uint8_t c : binout) {
			printHex(outs(), c);
			outs() << " ";
		}
		outs() << "\n";

		os << binout;
	}
};

MCCodeEmitter *replacementCodeEmitterConstructor(const MCInstrInfo& II, const MCRegisterInfo& MRI, MCContext& Ctx) {
	std::unique_ptr<MCCodeEmitter> ptr(actualCodeEmitterConstructor(II, MRI, Ctx));
	return new CodeEmitterWrapper(std::move(ptr), II, MRI);
}

static std::unique_ptr<ToolOutputFile> GetOutputStream() {
	// Open the file.
	std::error_code EC;
	sys::fs::OpenFlags OpenFlags = sys::fs::F_None;
	auto FDOut = llvm::make_unique<ToolOutputFile>(OutputFilename, EC, OpenFlags);
	if (EC) {
		WithColor::error() << EC.message() << '\n';
		throw exit_exception();
	}
	return FDOut;
}

static void compileModule(LLVMContext& ctx, const Target& target, TargetMachine& tm) {
	SMDiagnostic err;
	std::unique_ptr<Module> m = parseIRFile(InputFilename, err, ctx);

	if (!m) {
		err.print(progName, WithColor::error(errs(), progName));
		throw exit_exception();
	}

	StringRef triple = tm.getTargetTriple().getTriple();

	m->setTargetTriple(triple);
	m->setDataLayout(tm.createDataLayout());

	SmallVector<char, 0> binary_out;
	raw_svector_ostream bos(binary_out);
	std::unique_ptr<ToolOutputFile> ofile;
	raw_pwrite_stream* os = &bos;

	if (!OutputFilename.empty()) {
		ofile = GetOutputStream();
		os = &ofile->os();
	}

	legacy::PassManager pm;

	tm.addPassesToEmitFile(pm, *os, nullptr, TargetMachine::CodeGenFileType::CGFT_ObjectFile);

	pm.run(*m);

	if (ofile) {
		ofile->keep();
	}

	outs() << "Assembled " << os->tell() << " bytes" << (ofile ? "" : "(use -o to save)") << "\n";
}