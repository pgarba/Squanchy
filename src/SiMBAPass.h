#include <llvm/IR/Function.h>
#include <llvm/IR/PassManager.h>

typedef struct {
  bool MBAFound = false;
  bool HasOptimized = false;
  int SimbaCallCounter = 0;
} OptimizationGuide;

class SiMBAPass : public llvm::PassInfoMixin<SiMBAPass> {
private:
  OptimizationGuide *OG;

public:
  SiMBAPass(OptimizationGuide &G) { this->OG = &G; };

  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
}; // end of struct SiMBAPass
