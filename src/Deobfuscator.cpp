#include "Deobfuscator.h"

#include <string>

#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Threading.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Evaluator.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Pass.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

#include "llvm/Transforms/AggressiveInstCombine/AggressiveInstCombine.h"
#include "llvm/Transforms/Coroutines/CoroElide.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/ADCE.h"
#include "llvm/Transforms/Scalar/BDCE.h"
#include "llvm/Transforms/Scalar/CallSiteSplitting.h"
#include "llvm/Transforms/Scalar/ConstraintElimination.h"
#include "llvm/Transforms/Scalar/CorrelatedValuePropagation.h"
#include "llvm/Transforms/Scalar/DeadStoreElimination.h"
#include "llvm/Transforms/Scalar/EarlyCSE.h"
#include "llvm/Transforms/Scalar/Float2Int.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/JumpThreading.h"
#include "llvm/Transforms/Scalar/LoopSink.h"
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h" // Added header for LowerExpectIntrinsicPass
#include "llvm/Transforms/Scalar/MemCpyOptimizer.h"
#include "llvm/Transforms/Scalar/MergedLoadStoreMotion.h"
#include "llvm/Transforms/Scalar/NewGVN.h"
#include "llvm/Transforms/Scalar/Reassociate.h"
#include "llvm/Transforms/Scalar/SCCP.h"
#include "llvm/Transforms/Scalar/SROA.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/SpeculativeExecution.h"
#include "llvm/Transforms/Scalar/TailRecursionElimination.h"
#include "llvm/Transforms/Utils/AssumeBundleBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/EntryExitInstrumenter.h"
#include "llvm/Transforms/Utils/InjectTLIMappings.h"
#include "llvm/Transforms/Utils/LibCallsShrinkWrap.h"
#include "llvm/Transforms/Utils/MoveAutoInit.h"
#include "llvm/Transforms/Vectorize/VectorCombine.h"
#include <llvm/Target/TargetMachine.h>

using namespace llvm;
using namespace std;

// Command line options
static cl::opt<bool> KeepWASMRuntime("keep-wasm-runtime",
                                     cl::desc("Keep WASM runtime functions"),
                                     cl::init(false));

static cl::list<string>
    FunctionNames("f", cl::desc("Function names to deobfuscate (default all)"),
                  cl::value_desc("function name"));

static cl::opt<bool> Verbose("v", cl::desc("Print verbose output"));

static cl::opt<string> RuntimePath("runtime-path",
                                   cl::desc("Path to the squanchy runtime"),
                                   cl::value_desc("path"),
                                   cl::init("wasm_runtime.bc"));

static cl::opt<int> OptLevel("O", cl::desc("Optimization level (Default 3)"),
                             cl::value_desc("level"), cl::init(3));

namespace squanchy {

llvm::LLVMContext Deobfuscator::Context;

Deobfuscator::Deobfuscator(const std::string &filename,
                           const std::string &OutputFile)

    : InputFile(filename), OutputFile(OutputFile) {

  // Load the input file
  this->M = parse(filename);
  if (!M) {
    llvm::report_fatal_error("[!] Could not parse the input file!", false);
  }

  // Get the instruction count
  this->InstructionCountBefore = getInstructionCount(M.get());

  // Load the runtime module
  if (RuntimePath.empty()) {
    llvm::report_fatal_error("[!] Runtime path is empty!", false);
  }

  this->RuntimeModule = parse(RuntimePath);
  if (!RuntimeModule) {
    llvm::report_fatal_error("[!] Could not parse the runtime file!", false);
  }

  // Initialize the module
  this->TLII = new TargetLibraryInfoImpl(Triple(M->getTargetTriple()));
  this->TLI = std::make_unique<TargetLibraryInfo>(*TLII);
};

Deobfuscator::~Deobfuscator() {}

std::unique_ptr<llvm::Module> Deobfuscator::parse(const std::string &filename) {
  SMDiagnostic Err;

  auto M = llvm::parseIRFile(filename, Err, Context);
  if (!M) {
    return nullptr;
  }

  return std::move(M);
};

int Deobfuscator::getInstructionCount(llvm::Module *M) {
  int count = 0;
  for (auto &F : *M) {
    for (auto &BB : F) {
      count += BB.size();
    }
  }
  return count;
};

bool Deobfuscator::deobfuscate() {
  if (FunctionNames.empty()) {
    for (auto &F : *M) {
      if (!deobfuscateFunction(&F)) {
        return false;
      }
    }
  } else {
    for (auto &F : *M) {
      if (std::find(FunctionNames.begin(), FunctionNames.end(), F.getName()) !=
          FunctionNames.end()) {
        if (deobfuscateFunction(&F)) {
          return true;
        }
      }
    }
  }

  return true;
};

// ptr nocapture noundef readonly %0
bool Deobfuscator::isWasm2CFunction(llvm::Function *F) {
  if (F->arg_size() != 1) {
    // return false;
  }

  if (!F->arg_begin()->getType()->isPointerTy()) {
    // return false;
  }

  // todo: build better checks

  return true;
};

void Deobfuscator::linkRuntime() {
  llvm::Linker L(*M.get());

  // Clone the runtime module
  auto RuntimeModule = llvm::CloneModule(*this->RuntimeModule);

  L.linkInModule(std::move(RuntimeModule), Linker::Flags::OverrideFromSrc);
}

void Deobfuscator::optimizeFunction(llvm::Function *F) {
  // Create a new function pass manager
  ModuleAnalysisManager MAM;
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CAM;

  LoopPassManager LPM;

  llvm::PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CAM);
  PB.crossRegisterProxies(LAM, FAM, CAM, MAM);

  //auto FPM = PB.buildFunctionSimplificationPipeline(OptimizationLevel::O3,
  //                                                  ThinOrFullLTOPhase::None);

  //FPM.run(*F, FAM);

  auto MPM = PB.buildModuleSimplificationPipeline(OptimizationLevel::O3, ThinOrFullLTOPhase::None);

  MPM.run(*this->M, MAM);

  return;
}

bool Deobfuscator::deobfuscateFunction(llvm::Function *F) {
  if (!isWasm2CFunction(F)) {
    errs() << "[!] Function " << F->getName()
           << " is not generated bt wasm2c\n";
    return false;
  }

  if (Verbose) {
    errs() << "[*] Deobfuscating function: " << F->getName() << "\n";
  }

  // Remove optnone from function
  if (F->hasFnAttribute(Attribute::AttrKind::OptimizeNone)) {
    F->removeFnAttr(Attribute::AttrKind::OptimizeNone);
  }

  // Set DataLayout
  RuntimeModule->setDataLayout(M->getDataLayout());

  // 1. Inject the runtime module
  linkRuntime();

  // Set Helper functions to always inline
  setFunctionsAlwayInline();

  // 3. Call Init functions
  auto &Entry = F->getEntryBlock();
  auto &FirstInst = Entry.front();
  auto Arg0 = F->getArg(0);

  // Allocate new ST
  // Get Struct %struct.w2c_0x24add0x2Ewasm
  // Todo: Get the struct type dynamically without the hardcoded name
  StructType *ST =
      StructType::getTypeByName(M->getContext(), "struct.w2c_0x24add0x2Ewasm");

  AllocaInst *w2cInstance = nullptr;
  if (ST) {
    w2cInstance =
        new AllocaInst(ST, 0, "w2cInstance", &F->getEntryBlock().front());
  } else {
    // Size of struct is: 80 bytes 
    w2cInstance = new AllocaInst(Type::getInt8Ty(Context), 0, ConstantInt::getIntegerValue(Type::getInt32Ty(Context), APInt(80,32)),"w2cInstance", &F->getEntryBlock().front());
  }

  auto init_globals = M->getFunction("init_globals");
  auto init_memories = M->getFunction("init_memories");
  auto init_data_instances = M->getFunction("init_data_instances");
  auto load_data = M->getFunction("load_data");

  IRBuilder<> Builder(&FirstInst);
  auto call_init_globals = Builder.CreateCall(init_globals, w2cInstance);
  auto call_init_memories = Builder.CreateCall(init_memories, w2cInstance);
  auto call_init_data_instances =
      Builder.CreateCall(init_data_instances, w2cInstance);

  // Replace all uses of Arg0 with w2cInstance
  Arg0->replaceAllUsesWith(w2cInstance);

  // 4. Store modification to global variables, if any
  // Todo: Implement this

  // 5. Inline and Optimize the function
  inlineFunctions(F);
  removeCallASMSideEffects(F);
  optimizeFunction(F);

  // 6. Write the output file
  writeOutput();

  return true;
};

void Deobfuscator::writeOutput() {
  if (OutputFile.empty()) {
    M->print(outs(), nullptr);
    return;
  }

  std::error_code EC;
  raw_fd_ostream OS(OutputFile, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "[!] Could not open the output file\n";
    return;
  }

  M->print(OS, nullptr);
}

void Deobfuscator::removeCallASMSideEffects(llvm::Function *F) {
  if (!F)
    return;

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    auto II = I;
    I++;
    if (CallInst *CI = dyn_cast<CallInst>(&*II)) {
      if (CI->isInlineAsm()) {
        CI->eraseFromParent();
      }
    }
  }
}

void Deobfuscator::removeCallASMSideEffects(std::string FunctionName) {
  auto F = M->getFunction(FunctionName);
  if (!F) {
    return;
  }

  removeCallASMSideEffects(F);
}

void Deobfuscator::setFunctionAlwayInline(llvm::Function *F) {
  if (!F)
    return;

  F->addFnAttr(Attribute::AlwaysInline);

  for (auto U : F->users()) {
    if (auto *CI = dyn_cast<CallInst>(U)) {
      CI->addFnAttr(Attribute::AlwaysInline);
    }
  }
}

void Deobfuscator::setFunctionAlwayInline(std::string FunctionName) {
  auto F = M->getFunction(FunctionName);
  if (!F) {
    return;
  }

  setFunctionAlwayInline(F);
}

void Deobfuscator::setFunctionsAlwayInline() {
   // 2. Set always inline attribute
  setFunctionAlwayInline("init_globals");
  setFunctionAlwayInline("init_memories");
  setFunctionAlwayInline("init_data_instances");
  setFunctionAlwayInline("load_data");

  // Older wasm2c 
  setFunctionAlwayInline("i8_store");
  setFunctionAlwayInline("i16_store");
  setFunctionAlwayInline("i32_store");
  setFunctionAlwayInline("i64_store");

  setFunctionAlwayInline("i8_load");
  setFunctionAlwayInline("i16_load");
  setFunctionAlwayInline("i32_load");
  setFunctionAlwayInline("i64_load");

  // Newer wasm2c
  setFunctionAlwayInline("i8_store_default32");
  setFunctionAlwayInline("i16_store_default32");
  setFunctionAlwayInline("i32_store_default32");
  setFunctionAlwayInline("i64_store_default32");

  setFunctionAlwayInline("i8_load_default32");
  setFunctionAlwayInline("i16_load_default32");
  setFunctionAlwayInline("i32_load_default32");
  setFunctionAlwayInline("i64_load_default32");

  setFunctionAlwayInline("i8_store_unchecked");
  setFunctionAlwayInline("i16_store_unchecked");
  setFunctionAlwayInline("i32_store_unchecked");
  setFunctionAlwayInline("i64_store_unchecked");

  setFunctionAlwayInline("i8_load_unchecked");
  setFunctionAlwayInline("i18_load_unchecked");
  setFunctionAlwayInline("i32_load_unchecked");
  setFunctionAlwayInline("i64_load_unchecked");

  setFunctionAlwayInline("add_overflow");
}

void Deobfuscator::inlineFunctions(Function *F) {
  bool Changes;
  do {
    // Inline all calls to remill functions
    Changes = false;
    std::set<CallInst *> FuncToInline;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
      auto II = I;
      I++;
      if (CallInst *CI = dyn_cast<CallInst>(&*II)) {
        // Check if the called inst has a always inline attribute
        if (CI->isInlineAsm())
          continue;

        auto *CF = CI->getCalledFunction();
        if (!CF)
          continue;

        if (CF->hasFnAttribute(Attribute::AlwaysInline) &&
            CF->isDeclaration() == false) {
          FuncToInline.insert(CI);
        }
      }
    }

    // Now inline them all
    for (auto *CI : FuncToInline) {
      InlineFunctionInfo IFI;
      InlineFunction(*CI, IFI);
    }

    if (FuncToInline.size()) {
      Changes = true;
    }
  } while (Changes);

  return;
}

} // namespace squanchy