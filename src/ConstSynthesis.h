#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

#include <souper/Extractor/Solver.h>
#include <souper/SMTLIB2/Solver.h>

#include <vector>

#include "ConstantSynthesis.h"

namespace saturn {
class ConstSynthesis {
 public:
  ConstSynthesis();

  llvm::ConstantRange synthesizeConstant(llvm::Function *F,
                                         llvm::Value *ConstExpr);

 private:
  const int MaxConstantSynthesisTries = 30;
  const int Timeout = 10;

  std::unique_ptr<souper::SMTLIBSolver> SMTSolver;

  llvm::ConstantRange constantRange(const souper::BlockPCs &BPCs,
                                    const std::vector<souper::InstMapping> &PCs,
                                    souper::Inst *LHS, souper::InstContext &IC);

  void testRange(const souper::BlockPCs &BPCs,
                 const std::vector<souper::InstMapping> &PCs, souper::Inst *LHS,
                 llvm::APInt &C, llvm::APInt &ResultX, bool &IsFound,
                 souper::InstContext &IC);
};
}  // namespace saturn