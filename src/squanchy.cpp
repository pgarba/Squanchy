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

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // Some JIT Things
  InitializeAllTargets();
  InitializeAllTargetMCs();
  InitializeAllAsmPrinters();
  InitializeAllAsmParsers();

  cl::ParseCommandLineOptions(argc, argv, "squanchy wasm deobfuscator\n");
  cl::HideUnrelatedOptions(SquanchyCat);

  // Deobfuscate the input file
  squanchy::Deobfuscator Deobfuscator(InputFilename, OutputFilename);
  if (!Deobfuscator.deobfuscate()) {
    errs() << "[!] Could not deobfuscate the input file\n";
    return 1;
  }

  return 0;
};