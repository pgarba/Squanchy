#include "Solver.h"

#include <filesystem>
#include <string>

#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>
#include <souper/Inst/Inst.h>

using namespace souper;
using namespace llvm;

llvm::cl::opt<std::string> Z3Path("z3-path",
                                  llvm::cl::desc("Path to z3 binary"),
                                  llvm::cl::value_desc("z3-path"),
                                  llvm::cl::Optional, llvm::cl::init(""));

std::unique_ptr<SMTLIBSolver> GetUnderlyingSolver() {
  std::string Z3PathStr(Z3Path);

  // Try to get the z3 path from saturn deps dir
  if (Z3Path == "") {
#ifdef _WIN32
    auto ZPath =  "\\z3.exe";
#elif __APPLE__
    auto ZPath = "/opt/homebrew/bin/z3";
#else
    auto ZPath = "/usr/bin/z3";
#endif
    //Z3Path = ZPath;
    Z3PathStr = ZPath;
  }

  if (!std::filesystem::exists(Z3PathStr)) {
    // Try to get the z3 binary from deps
    llvm::report_fatal_error(
        ("Solver '" + Z3PathStr +
         "' does not exist or is not executable.\nSet path "
         "by -z3-path=<z3_binary_path>")
            .c_str());
  }
  return createZ3Solver(makeExternalSolverProgram(Z3PathStr), false);
}

CandidateReplacement *getSouperReplacement(souper::FunctionCandidateSet &CS,
                                           llvm::Value *Candidate,
                                           int MaxCandidateInstCount) {
  for (auto &B : CS.Blocks) {
    for (auto &R : B->Replacements) {
      if (R.Origin != Candidate ||
          R.Mapping.LHS->Width != Candidate->getType()->getIntegerBitWidth())
        continue;

      // Check if instruction is not to long
      int ICount = souper::instCount(R.Mapping.LHS);
      if (ICount > MaxCandidateInstCount) {
        continue;
      }

      // Candidate found
      return &R;
    }
  }

  return nullptr;
}

CandidateReplacement *getSouperCandidate(souper::FunctionCandidateSet &FCS,
                                         llvm::Value *V,
                                         int CandidateBitWidth) {
  // Identify the candidate
  for (auto &B : FCS.Blocks) {
    for (auto &R : B->Replacements) {
      if ((R.Origin != V) || (R.Mapping.LHS->Width != CandidateBitWidth))
        continue;

      return &R;
      break;
    }
  }

  return nullptr;
}