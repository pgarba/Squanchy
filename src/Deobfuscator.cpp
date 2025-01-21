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

#include "LLVMExtract.h"

using namespace llvm;
using namespace std;

extern cl::OptionCategory SquanchyCat;

// Command line options
static cl::opt<bool> KeepWASMRuntime("keep-wasm-runtime",
                                     cl::desc("Keep WASM runtime functions"),
                                     cl::init(false), cl::cat(SquanchyCat));

static cl::list<string>
    FunctionNames("f", cl::desc("Function names to deobfuscate (default all)"),
                  cl::value_desc("function name"), cl::cat(SquanchyCat));

static cl::opt<bool> Verbose("v", cl::desc("Print verbose output"),
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
  llvm::Linker L(*M);

  // Clone the runtime module
  auto RuntimeModule = llvm::CloneModule(*this->RuntimeModule);

  L.linkInModule(std::move(RuntimeModule), Linker::Flags::OverrideFromSrc);
}

void Deobfuscator::optimizeFunction(llvm::Function *F, bool runModulePasses) {
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

  if (runModulePasses) {
    auto MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O3);
    MPM.run(*this->M, MAM);
  }

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
  injectInitializer(F);

  // 4. Store modification to global variables, if any
  // Todo: Implement this

  // 5. Inline functions
  inlineFunctions(F);

  // 6. Remove asm calls with sideeffect
  removeCallASMSideEffects(F);

  // Set function noinline
  F->addFnAttr(Attribute::NoInline);

  // 7. Optimize the functions
  optimizeFunction(F);

  // 8. Extract the function and globals
  LLVMExtract(M.get(), {F->getName().str()}, {"data_segment_data.*"}, false);

  // 9. Optimize the functions with module passes enabled (folds the code
  // further)
  optimizeFunction(F, true);

  // 10. Remove the noinline attribute
  F->removeFnAttr(Attribute::NoInline);

  // 11. Write the output file
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

void Deobfuscator::handle_funcref_table_init(llvm::Function *F) {
  // Handle funcref_table_init
  // Check if there is any usage for funcref_table_init
  // Todo: See how to initialize the funcref_table properly
  auto funcref_table_init = M->getFunction("funcref_table_init");
  if (!funcref_table_init) {
    return;
  }

  // get a call that comes from init_tables
  CallInst *call_funcref_table_init = nullptr;
  int count = 0;

  for (auto U : funcref_table_init->users()) {
    if (auto *CI = dyn_cast<CallInst>(U)) {
      call_funcref_table_init = CI;
      count++;
    }
  }

  if (!call_funcref_table_init || count > 1) {
    errs() << "[!] Could not find the call to funcref_table_init uses( "
              "count: "
           << count << ")\n";
    return;
  }

  // Get the number of elements in the table
  auto *tableSize = call_funcref_table_init->getArgOperand(5);

  // Get create_table function
  auto create_table = M->getFunction("create_table");
  if (!create_table) {
    errs() << "[!] Could not find the create_table function\n";
    return;
  }

  // Call create_table
  auto CT =
      CallInst::Create(create_table, {tableSize}, "", call_funcref_table_init);

  CT->dump();

  // Set table as call argument
  // call_funcref_table_init->setArgOperand(0, CT);
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

  // Allocate an ptr for struct w2c_env
  const int w2c_envSize = 80;
  auto w2c_env =
      new AllocaInst(Type::getInt8Ty(Context), 0,
                     ConstantInt::getIntegerValue(Type::getInt32Ty(Context),
                                                  APInt(w2c_envSize, 32)),
                     "w2c_env", &F->getEntryBlock().front());

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