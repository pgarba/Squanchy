#include <iostream>
#include <string>

#include <llvm/Support/CommandLine.h>

#include "Deobfuscator.h"

using namespace llvm;
using namespace std;

static cl::opt<string>
    InputFilename(cl::Positional, cl::desc("Input llvm ir file"), cl::Required);

static cl::opt<string> OutputFilename("o", cl::desc("Output llvm ir filename"),
                                      cl::value_desc("filename"));

static cl::opt<int> OptLevel("O", cl::desc("Optimization level (Default 3)"),
                             cl::value_desc("level"), cl::init(3));

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "squanchy wasm deobfuscator\n");

  // Deobfuscate the input file
  squanchy::Deobfuscator Deobfuscator(InputFilename, OutputFilename);
  if (!Deobfuscator.deobfuscate()) {
    errs() << "[!] Could not deobfuscate the input file\n";
    return 1;
  }

  return 0;
};