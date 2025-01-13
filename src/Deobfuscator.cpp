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
        if (!deobfuscateFunction(&F)) {
          return false;
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

  llvm::PipelineTuningOptions opts;
  opts.InlinerThreshold = 0;

  llvm::PassBuilder PB(nullptr, opts);

  PB.registerModuleAnalyses(MAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CAM);
  PB.crossRegisterProxies(LAM, FAM, CAM, MAM);

  auto FPM = PB.buildFunctionSimplificationPipeline(OptimizationLevel::O3,
                                                    ThinOrFullLTOPhase::None);

  FPM.run(*F, FAM);

  // Create the pass manager.
  // This one corresponds to a typical -O2 optimization pipeline.
  /*
  ModulePassManager MPM =
      PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3);

  // Optimize the IR!
  MPM.run(*F->getParent(), MAM);
  */

  FPM.addPass(EntryExitInstrumenterPass(false));

  FPM.addPass(LowerExpectIntrinsicPass());
  FPM.addPass(SimplifyCFGPass());
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));
  FPM.addPass(EarlyCSEPass());
  FPM.addPass(CallSiteSplittingPass());

  // buildModuleOptimizationPipeline
  // FPM.addPass(LoopVersioningLICMPass());
  FPM.addPass(Float2IntPass());

  // Add loop passes here
  // https://github.com/llvm/llvm-project/blob/64075837b5532108a1fe96a5b158feb7a9025694/llvm/lib/Passes/PassBuilderPipelines.cpp#L1473

  FPM.addPass(InjectTLIMappings());

  // https://github.com/llvm/llvm-project/blob/64075837b5532108a1fe96a5b158feb7a9025694/llvm/lib/Passes/PassBuilderPipelines.cpp#L545
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));

  FPM.addPass(EarlyCSEPass(true));

  bool EnableKnowledgeRetention = false;
  if (EnableKnowledgeRetention)
    FPM.addPass(AssumeSimplifyPass());

  bool EnableGVNHoist = true;
  if (EnableGVNHoist)
    FPM.addPass(GVNHoistPass());

  bool EnableGVNSink = true;
  if (EnableGVNSink) {
    FPM.addPass(GVNSinkPass());
    FPM.addPass(
        SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  }

  FPM.addPass(SpeculativeExecutionPass(true));
  FPM.addPass(JumpThreadingPass());
  FPM.addPass(CorrelatedValuePropagationPass());

  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

  // Only run instcombine once
  InstCombineOptions ICO;
  ICO.setMaxIterations(1);

  FPM.addPass(InstCombinePass(ICO));
  FPM.addPass(AggressiveInstCombinePass());

  // Optimizes for size
  FPM.addPass(LibCallsShrinkWrapPass());

  FPM.addPass(TailCallElimPass());
  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));

  FPM.addPass(ReassociatePass());

  FPM.addPass(ConstraintEliminationPass());

  // todo add LoopPasss
  // https://github.com/llvm/llvm-project/blob/64075837b5532108a1fe96a5b158feb7a9025694/llvm/lib/Passes/PassBuilderPipelines.cpp#L627

  FPM.addPass(
      SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  FPM.addPass(InstCombinePass(ICO));

  // Delete small array after loop unroll.
  FPM.addPass(SROAPass(SROAOptions::ModifyCFG));

  FPM.addPass(VectorCombinePass(true));

  // Eliminate redundancies.
  FPM.addPass(MergedLoadStoreMotionPass());

  bool RunNewGVN = false;
  if (RunNewGVN)
    FPM.addPass(NewGVNPass());
  else
    FPM.addPass(GVNPass());

  FPM.addPass(SCCPPass());
  FPM.addPass(BDCEPass());
  FPM.addPass(InstCombinePass(ICO));

  // FPM.addPass(JumpThreadingPass());
  FPM.addPass(CorrelatedValuePropagationPass());

  // Finally, do an expensive DCE pass to catch all the dead code exposed by
  // the simplifications and basic cleanup after all the simplifications.
  FPM.addPass(ADCEPass());

  FPM.addPass(DSEPass());

  FPM.addPass(MoveAutoInitPass());

  FPM.addPass(CoroElidePass());

  FPM.addPass(SimplifyCFGPass(SimplifyCFGOptions()
                                  .convertSwitchRangeToICmp(true)
                                  .hoistCommonInsts(true)
                                  .sinkCommonInsts(true)));
  FPM.addPass(InstCombinePass(ICO));

  // Run Opts
  auto HasOpt = FPM.run(*F, FAM);
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

  // Set DataLayout
  RuntimeModule->setDataLayout(M->getDataLayout());

  // 1. Inject the runtime module
  linkRuntime();

  // 2. Set always inline attribute
  setFunctionAlwayInline("init_globals");
  setFunctionAlwayInline("init_memories");
  setFunctionAlwayInline("init_data_instances");
  setFunctionAlwayInline("load_data");

  setFunctionAlwayInline("i8_store");
  setFunctionAlwayInline("i16_store");
  setFunctionAlwayInline("i32_store");
  setFunctionAlwayInline("i64_store");

  setFunctionAlwayInline("i8_load");
  setFunctionAlwayInline("i16_load");
  setFunctionAlwayInline("i32_load");
  setFunctionAlwayInline("i64_load");

  // 2. Remove sideeffect asm calls (Creates cleaner code)
  removeCallASMSideEffects("i8_load");
  removeCallASMSideEffects("i16_load");
  removeCallASMSideEffects("i32_load");
  removeCallASMSideEffects("i64_load");

  // 3. Call Init functions
  auto &Entry = F->getEntryBlock();
  auto &FirstInst = Entry.front();
  auto Arg0 = F->getArg(0);

  // Allocate new ST
  // Get Struct %struct.w2c_0x24add0x2Ewasm
  // Todo: Get the struct type dynamically without the hardcoded name
  StructType *ST =
      StructType::getTypeByName(M->getContext(), "struct.w2c_0x24add0x2Ewasm");
  auto w2cInstance =
      new AllocaInst(ST, 0, "w2cInstance", &F->getEntryBlock().front());

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

  // 4. Store modification to global variables
  // Todo: Implement this

  // 5. Inline and Optimize the function
  inlineFunctions(F);
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