#include <iostream>
#include <string>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>

#include "Deobfuscator.h"

using namespace llvm;
using namespace std;

cl::OptionCategory SquanchyCat("Squanchy Options");

static cl::opt<string> InputFilename(cl::Positional,
                                     cl::desc("Input llvm ir file"),
                                     cl::Required, cl::cat(SquanchyCat));

static cl::opt<string> OutputFilename("o", cl::desc("Output llvm ir filename"),
                                      cl::value_desc("filename"),
                                      cl::cat(SquanchyCat));

void ParseLLVMOptions(int argc, char **argv) {
  SmallVector<const char *, 20> newArgv;
  BumpPtrAllocator A;
  StringSaver Saver(A);

  for (int i = 0; i < argc; i++) {
    newArgv.push_back(Saver.save(argv[i]).data());
  }

  // Gives a nice improvment and code folds like before
  // At least those 4 options are needed to let large code fold and create valid
  // results
  newArgv.push_back(Saver.save("-memdep-block-scan-limit=1000000").data());
  newArgv.push_back(Saver.save("-dse-memoryssa-walklimit=1000000").data());
  newArgv.push_back(Saver.save("-available-load-scan-limit=1000000").data());
  newArgv.push_back(Saver.save("-dse-memoryssa-scanlimit=1000000").data());

  newArgv.push_back(
      Saver.save("-earlycse-mssa-optimization-cap=1000000").data());
  newArgv.push_back(Saver.save("-memssa-check-limit=1000000").data());

  newArgv.push_back(
      Saver.save("-dse-memoryssa-defs-per-block-limit=1000000").data());
  newArgv.push_back(
      Saver.save("-dse-memoryssa-partial-store-limit=1000000").data());
  newArgv.push_back(
      Saver.save("-dse-memoryssa-path-check-limit=1000000").data());
  newArgv.push_back(Saver.save("-dse-memoryssa-otherbb-cost=2").data());
  newArgv.push_back(Saver.save("-memdep-block-number-limit=1000000").data());
  newArgv.push_back(Saver.save("-gvn-max-block-speculations=1000000").data());
  newArgv.push_back(Saver.save("-gvn-max-num-deps=1000000").data());
  newArgv.push_back(Saver.save("-gvn-hoist-max-chain-length=-1").data());
  newArgv.push_back(Saver.save("-gvn-hoist-max-depth=-1").data());
  newArgv.push_back(Saver.save("-gvn-hoist-max-bbs=-1").data());

  newArgv.push_back(Saver.save("-unroll-threshold=1000000").data());
  newArgv.push_back(Saver.save("-unroll-count=64").data());

  newArgv.push_back(Saver.save("-dfa-cost-threshold=1000000").data());
  newArgv.push_back(Saver.save("-dfa-max-path-length=1000000").data());
  newArgv.push_back(Saver.save("-dfa-max-num-paths=1000000").data());

  int newArgc = static_cast<int>(newArgv.size());
  llvm::cl::ParseCommandLineOptions(newArgc, &newArgv[0]);
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // Some JIT Things
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  cl::HideUnrelatedOptions(SquanchyCat);
  ParseLLVMOptions(argc, argv);

  // Deobfuscate the input file
  squanchy::Deobfuscator Deobfuscator(InputFilename, OutputFilename);
  if (!Deobfuscator.deobfuscate()) {
    errs() << "[!] Could not deobfuscate the input file\n";
    return 1;
  }

  return 0;
};