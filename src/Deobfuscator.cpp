#include "Deobfuscator.h"

#include <string>

#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/TargetPassConfig.h"
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

#include <llvm/Target/TargetMachine.h>

// Passes
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
#include "llvm/Transforms/Scalar/LowerExpectIntrinsic.h"
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

#include "LLVMExtract.h"

using namespace llvm;
using namespace std;

extern cl::OptionCategory SquanchyCat;

// Command line options
static cl::opt<bool> KeepWASMRuntime("keep-wasm-runtime",
                                     cl::desc("Keep WASM runtime functions"),
                                     cl::init(false), cl::cat(SquanchyCat));

static cl::list<string>
    FunctionNames("f", cl::desc("List of function names to deobfuscate"),
                  cl::value_desc("function names"), cl::OneOrMore,
                  cl::cat(SquanchyCat));

static cl::opt<bool> Verbose("v", cl::desc("Print verbose output"),
                             cl::cat(SquanchyCat));

static cl::opt<bool>
    PrintFunctions("list-functions",
                   cl::desc("List all functions in the module"),
                   cl::cat(SquanchyCat));

static cl::opt<string> RuntimePath("runtime-path",
                                   cl::desc("Path to the squanchy runtime"),
                                   cl::value_desc("path"),
                                   cl::init("wasm_runtime.bc"),
                                   cl::cat(SquanchyCat));

static cl::opt<int> OptLevel("O", cl::desc("Optimization level (Default 3)"),
                             cl::value_desc("level"), cl::init(3),
                             cl::cat(SquanchyCat));

static cl::opt<string> ModuleName("module-name",
                                  cl::desc("The module-name used in wasm2c"),
                                  cl::value_desc("module-name"),
                                  cl::init("squanchy"), cl::cat(SquanchyCat));

static cl::opt<bool> ExtractRecursive("extract-recursive",
                                      cl::desc("extract functions recursively"),
                                      cl::init(false), cl::cat(SquanchyCat));

static cl::opt<bool>
    ExtractFunction("extract-function",
                    cl::desc("extract function from the module"),
                    cl::init(true), cl::cat(SquanchyCat));

// Disable for now as it might lead to wrong results ...
static cl::opt<bool> ReplaceCallocs("replace-callocs",
                                    cl::desc("Replace callocs with allocas"),
                                    cl::init(false), cl::cat(SquanchyCat));

namespace squanchy {
// Needs to be global otherwise we will see a crash during optimization
llvm::LLVMContext Context;

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

  // Override TargetTriple
  overrideTarget(M.get());
  overrideTarget(this->RuntimeModule.get());

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
    if (F.isDeclaration())
      continue;

    count += getInstructionCount(&F);
  }
  return count;
};

int Deobfuscator::getInstructionCount(llvm::Function *F) {
  int count = 0;
  for (auto &BB : *F) {
    count += BB.size();
  }
  return count;
};

bool Deobfuscator::deobfuscate() {
  // Print the functions
  if (PrintFunctions) {
    int i = 0;
    for (auto &F : *M) {
      if (F.isDeclaration())
        continue;

      outs() << "[" << i++ << "] " << F.getName() << "\t ("
             << getInstructionCount(&F) << ")\n";
    }
    return true;
  }

  // Deobfuscate the functions
  for (auto &FName : FunctionNames) {
    auto F = M->getFunction(FName);
    if (!F) {
      errs() << "[!] Function " << FName << " not found!\n";
      return false;
    }

    if (F->Function::isDeclaration()) {
      errs() << "[!] Function " << FName << " is a declaration!\n";
      return false;
    }

    outs() << "[*] Deobfuscating function: " << FName << "\n";

    int InstCountBefore = getInstructionCount(F);

    if (!deobfuscateFunction(F)) {
      return false;
    }

    int InstCountAfter = getInstructionCount(F);

    outs() << "[*] Instruction count before: " << InstCountBefore
           << " after: " << InstCountAfter << "\n";
  }

  // 9. Extract the function and globals
  if (ExtractFunction) {
    LLVMExtract(M.get(), FunctionNames, {"data_segment_data.*"},
                ExtractRecursive);
  }

  // 11. Optimize the functions with module passes enabled (folds the code
  // further)
  optimizeModule(M.get());

  for (auto &FName : FunctionNames) {
    auto F = M->getFunction(FName);
    if (!F) {
      continue;
    }

    outs() << "[*] Function: " << FName
           << " Instruction count: " << getInstructionCount(F) << "\n";
  }

  // 12. Write the output file
  writeOutput();

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
  llvm::Linker L(*M);

  // Clone the runtime module
  auto RuntimeModule = llvm::CloneModule(*this->RuntimeModule);

  L.linkInModule(std::move(RuntimeModule), Linker::Flags::OverrideFromSrc);
}

void Deobfuscator::optimizeFunction(llvm::Function *F) {
  if (OptLevel == 0) {
    return;
  }

  // Create a new function pass manager
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Register all the basic analyses with the managers.
  PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  auto FPM = PB.buildFunctionSimplificationPipeline(OptimizationLevel::O3,
                                                    ThinOrFullLTOPhase::None);
  FPM.run(*F, FAM);

  return;
}

void Deobfuscator::optimizeFunctionWithCustomPipeline(llvm::Function *F,
                                                      bool SimplifyCFG) {
  if (OptLevel == 0) {
    return;
  }

  // Create a new function pass manager
  ModuleAnalysisManager MAM;
  FunctionAnalysisManager FAM;
  LoopAnalysisManager LAM;
  CGSCCAnalysisManager CAM;

  FunctionPassManager FPM;
  LoopPassManager LPM;

  llvm::PipelineTuningOptions opts;
  opts.InlinerThreshold = 0;

  llvm::PassBuilder PB(nullptr, opts);

  PB.registerModuleAnalyses(MAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.registerCGSCCAnalyses(CAM);
  PB.crossRegisterProxies(LAM, FAM, CAM, MAM);

  // https://github.com/llvm/llvm-project/blob/c9e5c42ad1bba84670d6f7ebe7859f4f12063c5a/llvm/lib/Passes/PassBuilderPipelines.cpp#L1586
  FPM.addPass(EntryExitInstrumenterPass(false));

  FPM.addPass(LowerExpectIntrinsicPass());
  if (SimplifyCFG) {
    FPM.addPass(SimplifyCFGPass());
  }
  FPM.addPass(SROAPass(SROAOptions::PreserveCFG));
  FPM.addPass(EarlyCSEPass());
  FPM.addPass(CallSiteSplittingPass());

  // buildModuleOptimizationPipeline
  // FPM.addPass(LoopVersioningLICMPass());
  FPM.addPass(Float2IntPass());

  // Add loop passes here
  // https://github.com/llvm/llvm-project/blob/64075837b5532108a1fe96a5b158feb7a9025694/llvm/lib/Passes/PassBuilderPipelines.cpp#L1473

  FPM.addPass(InjectTLIMappings());

  // https://github.com/llvm/llvm-project/blob/64075837b5532108a1fe96a5b158feb7a9025694/llvm/lib/Passes/PassBuilderPipelines.cpp#L545
  FPM.addPass(SROAPass(SROAOptions::PreserveCFG));

  FPM.addPass(EarlyCSEPass(true));

  bool EnableKnowledgeRetention = false;
  if (EnableKnowledgeRetention)
    FPM.addPass(AssumeSimplifyPass());

  bool EnableGVNHoist = true;
  if (EnableGVNHoist)
    FPM.addPass(GVNHoistPass());

  if (SimplifyCFG) {
    bool EnableGVNSink = true;
    if (EnableGVNSink) {
      FPM.addPass(GVNSinkPass());
      FPM.addPass(
          SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
    }
  }

  FPM.addPass(SpeculativeExecutionPass(true));
  FPM.addPass(JumpThreadingPass());
  FPM.addPass(CorrelatedValuePropagationPass());

  if (SimplifyCFG) {
    FPM.addPass(
        SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  }

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

  if (SimplifyCFG) {
    FPM.addPass(
        SimplifyCFGPass(SimplifyCFGOptions().convertSwitchRangeToICmp(true)));
  }
  FPM.addPass(InstCombinePass(ICO));

  // Delete small array after loop unroll.
  FPM.addPass(SROAPass(SROAOptions::PreserveCFG));

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

  FPM.addPass(JumpThreadingPass());
  FPM.addPass(CorrelatedValuePropagationPass());

  // Finally, do an expensive DCE pass to catch all the dead code exposed by
  // the simplifications and basic cleanup after all the simplifications.
  FPM.addPass(ADCEPass());

  // Specially optimize memory movement as it doesn't look like dataflow in SSA.
  FPM.addPass(MemCpyOptPass());
  FPM.addPass(DSEPass());

  FPM.addPass(MoveAutoInitPass());

  FPM.addPass(CoroElidePass());

  FPM.addPass(SimplifyCFGPass(SimplifyCFGOptions()
                                  .convertSwitchRangeToICmp(true)
                                  .hoistCommonInsts(true)
                                  .sinkCommonInsts(true)));
  FPM.addPass(InstCombinePass(ICO));

  // Run Opts
  bool DoRun = true;
  int Run = 1;
  while (DoRun) {
    DoRun = false;

    int InstCountBefore = getInstructionCount(F);

    auto HasOpt = FPM.run(*F, FAM);

    int InstCountAfter = getInstructionCount(F);

    outs() << "[" << Run << "]" << "Before: " << InstCountBefore << " After: " << InstCountAfter
           << "\n";
    if (InstCountAfter != InstCountBefore)
      DoRun = true;
  };
}

void Deobfuscator::optimizeModule(llvm::Module *M) {
  if (OptLevel == 0) {
    return;
  }

  // Create a new function pass manager
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Register all the basic analyses with the managers.
  PassBuilder PB;

  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  auto MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3);
  MPM.run(*this->M, MAM);
}

bool Deobfuscator::deobfuscateFunction(llvm::Function *F) {
  if (!isWasm2CFunction(F)) {
    errs() << "[!] Function " << F->getName()
           << " is not generated by wasm2c\n";
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
  injectInitializer(F);

  // 4. Store modification to global variables, if any
  // Todo: Implement this
  // Maybe not needed anymore ?

  // 5. Inline functions
  inlineFunctions(F);

  // 6. Remove asm calls with sideeffect
  removeCallASMSideEffects(F);

  // 7. Set function noinline and Remove the noinline attribute
  F->addFnAttr(Attribute::NoInline);

  for (llvm::Function &F : *M) {
    if (F.hasFnAttribute(Attribute::NoInline) &&
        F.hasFnAttribute(Attribute::AlwaysInline)) {
      F.removeFnAttr(Attribute::NoInline);
      F.removeFnAttr(Attribute::AlwaysInline);
    }
  }

  // 8. Optimize the functions
  optimizeFunctionWithCustomPipeline(F);
  optimizeFunction(F);


  // 10. Replace Callocs
  if (ReplaceCallocs) {
    replaceCallocs(F);
  }

  return true;
};

void Deobfuscator::replaceCallocs(llvm::Function *F) {
  // Replace callocs with allocas
  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    auto II = I;
    I++;
    if (CallInst *CI = dyn_cast<CallInst>(&*II)) {
      if (Function *Callee = CI->getCalledFunction()) {
        if (Callee->getName() == "calloc") {
          // Get the size and count of calloc
          auto Size = CI->getArgOperand(0);
          auto Count = CI->getArgOperand(1);

          // Replace calloc with alloca
          IRBuilder<> Builder(CI);
          auto Alloca = Builder.CreateAlloca(Type::getInt8Ty(Context),
                                             Builder.CreateMul(Size, Count));
          CI->replaceAllUsesWith(Alloca);
          CI->eraseFromParent();
        }
      }
    }
  }
}

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

void Deobfuscator::injectInitializer(llvm::Function *F) {
  // Init the env for the function properly
  auto &Entry = F->getEntryBlock();
  auto &FirstInst = Entry.front();
  auto Arg0 = F->getArg(0);

  // Allocate new ST
  // Get wasm2c struct used for the instance
  // w2c_squanchy
  string StructName = "struct.w2c_" + ModuleName;
  StructType *ST = StructType::getTypeByName(M->getContext(), StructName);

  AllocaInst *w2cInstance = nullptr;
  if (ST) {
    // Allocate a real struct type
    w2cInstance =
        new AllocaInst(ST, 0, "w2cInstance", &F->getEntryBlock().front());
  }

  if (!ST)
    report_fatal_error("Could not find the struct type");

  // Get Struct w2c_env
  StructType *STEnv =
      StructType::getTypeByName(M->getContext(), "struct.w2c_env");
  AllocaInst *w2c_env = nullptr;
  if (STEnv) {
    // Allocate an ptr for struct w2c_env
    w2c_env = new AllocaInst(STEnv, 0, "w2c_env", &F->getEntryBlock().front());
  } else {
    // Use w2c_env_size to get the size of the struct
    auto w2c_env_size = M->getGlobalVariable("w2c_env_size");
    if (!w2c_env_size) {
      report_fatal_error("Could not find w2c_env_size");
    }

    // Allocate an alloca for the struct
    auto w2c_env_size_val = cast<ConstantInt>(w2c_env_size->getInitializer());
    auto w2c_env_size_int = w2c_env_size_val->getZExtValue();
    auto w2c_env_size_type = Type::getIntNTy(Context, w2c_env_size_int * 8);
    w2c_env = new AllocaInst(w2c_env_size_type, 0, "w2c_env",
                             &F->getEntryBlock().front());
  }

  // Call wasm2c_squanchy_instantiate(w2c_squanchy* instance, struct
  // w2c_env* w2c_env_instance)
  auto wasm2c_squanchy_instantiate =
      M->getFunction("wasm2c_squanchy_instantiate");

  IRBuilder<> Builder(&FirstInst);
  auto call_wasm2c_squanchy_instantiate =
      Builder.CreateCall(wasm2c_squanchy_instantiate, {w2cInstance, w2c_env});

  // Replace all uses of Arg0 with w2cInstance
  Arg0->replaceAllUsesWith(w2cInstance);
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

void Deobfuscator::removeAlwayInlineAttribute() {
  for (auto FName : AIFunctionNames) {
    auto F = M->getFunction(FName);
    if (!F) {
      continue;
    }

    F->removeFnAttr(Attribute::AlwaysInline);
  }
}

void Deobfuscator::setFunctionAlwayInline(llvm::Function *F) {
  if (!F)
    return;

  // Check if function is already always inline
  if (F->hasFnAttribute(Attribute::AlwaysInline)) {
    return;
  }

  F->addFnAttr(Attribute::AlwaysInline);

  for (auto U : F->users()) {
    if (auto *CI = dyn_cast<CallInst>(U)) {
      CI->addFnAttr(Attribute::AlwaysInline);
    }
  }

  AIFunctionNames.push_back(F->getName().str());
}

void Deobfuscator::setFunctionAlwayInline(std::string FunctionName) {
  auto F = M->getFunction(FunctionName);
  if (!F) {
    return;
  }

  setFunctionAlwayInline(F);
}

void Deobfuscator::setFunctionsAlwayInline() {
  // wasm2c_squanchy_instantiate function
  string FunctionName = "wasm2c_" + ModuleName + "_instantiate";
  setFunctionAlwayInline(FunctionName);

  // Set always inline attribute
  setFunctionAlwayInline("wasm_rt_is_initialized");
  setFunctionAlwayInline("init_instance_import");
  setFunctionAlwayInline("init_globals");
  setFunctionAlwayInline("init_tables");
  setFunctionAlwayInline("funcref_table_init");
  setFunctionAlwayInline("init_memories");
  setFunctionAlwayInline("init_elem_instances");
  setFunctionAlwayInline("init_data_instances");
  setFunctionAlwayInline("load_data");

  // Older wasm2c
  const std::string LoadStoreFunctions[] = {
      "i8_store",     "i16_store",    "i32_store",    "i64_store",
      "i8_load",      "i16_load",     "i32_load",     "i64_load",
      "i32_load8_s",  "i64_load8_s",  "i32_load8_u",  "i64_load8_u",
      "i32_load16_s", "i64_load16_s", "i32_load16_u", "i64_load16_u",
      "i64_load32_s", "i64_load32_u", "f32_load",     "f64_load",
      "f32_store",    "f64_store",    "i32_store8",   "i32_store16",
      "i64_store8",   "i64_store16",  "i64_store32"};

  for (auto &FName : LoadStoreFunctions) {
    setFunctionAlwayInline(FName);
  }

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

  setFunctionAlwayInline("i32_load8_u_unchecked");
  setFunctionAlwayInline("i32_store8_unchecked");

  setFunctionAlwayInline("i32_store8_default32");
  setFunctionAlwayInline("i32_load8_u_default32");

  setFunctionAlwayInline("add_overflow");
  setFunctionAlwayInline("func_types_eq");
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

void Deobfuscator::overrideTarget(llvm::Module *M) {
  std::string Target = "x86_64-linux-gnu";

  M->setTargetTriple(Target);
  M->setDataLayout("");
}

} // namespace squanchy