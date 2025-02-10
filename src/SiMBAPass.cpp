#include "SiMBAPass.h"

#include <chrono>

#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/LegacyPassManager.h"

#include "../dependencies/SiMBA-/LLVMParser.h"

using namespace std;
using namespace llvm;
using namespace std::chrono;

cl::opt<bool> PrintMBADebug("simba-debug", cl::Optional,
                            cl::desc("Print SiMBA debug output"),
                            cl::value_desc("simba-debug"), cl::init(false));

cl::opt<bool> SiMBAStats("simba-stats", cl::Optional,
                         cl::desc("Print SiMBA stats"),
                         cl::value_desc("simba-stats"), cl::init(true));

PreservedAnalyses SiMBAPass::run(Function &F, FunctionAnalysisManager &FAM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();

  // Reset the MBAFound flag
  this->OG->MBAFound = false;

  LSiMBA::LLVMParser Parser(&F, true, true, false, false, PrintMBADebug, true);

  // Measure the time
  auto start = high_resolution_clock::now();

  // Run the simplification
  int MBACount = Parser.simplify();

  if (SiMBAStats && MBACount) {
    auto stop = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(stop - start);

    outs() << "[SiMBA++] MBAs found and replaced: '" << MBACount
           << "' time: " << (int)duration.count() << "ms\n";
  }

  if (MBACount > 0) {
    this->OG->HasOptimized = false;
    this->OG->MBAFound = true;
  }

  // Increase the call counter
  this->OG->SimbaCallCounter++;

  return PreservedAnalyses::all();
}
