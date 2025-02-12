#include "ConstSynthesis.h"

#include <llvm/IR/ConstantRange.h>

#include "Solver.h"

using namespace souper;
using namespace llvm;
using namespace std;

namespace saturn {

void ConstSynthesis::testRange(const BlockPCs &BPCs,
                               const std::vector<InstMapping> &PCs, Inst *LHS,
                               llvm::APInt &C, llvm::APInt &ResultX,
                               bool &IsFound, InstContext &IC) {
  unsigned W = LHS->Width;

  Inst *ReservedX = IC.createSynthesisConstant(W, 1);
  Inst *CVal = IC.getConst(C);
  Inst *LowerVal = ReservedX;
  Inst *UpperValOverflow =
      IC.getInst(Inst::UAddWithOverflow, W + 1,
                 {IC.getInst(Inst::Add, W, {LowerVal, CVal}),
                  IC.getInst(Inst::UAddO, 1, {LowerVal, CVal})});

  Inst *IsOverflow =
      IC.getInst(Inst::ExtractValue, 1,
                 {UpperValOverflow, IC.getUntypedConst(llvm::APInt(W, 1))});
  Inst *UpperVal =
      IC.getInst(Inst::ExtractValue, W,
                 {UpperValOverflow, IC.getUntypedConst(llvm::APInt(W, 0))});

  Inst *GuessLowerPartNonWrapped = IC.getInst(Inst::Ule, 1, {LowerVal, LHS});
  Inst *GuessUpperPartNonWrapped = IC.getInst(Inst::Ult, 1, {LHS, UpperVal});

  // non-wrapped, x <= LHS < x+c
  Inst *GuessAnd = IC.getInst(
      Inst::And, 1, {GuessLowerPartNonWrapped, GuessUpperPartNonWrapped});
  // wrapped, LHS < x+c \/ LHS >= x
  Inst *GuessOr = IC.getInst(
      Inst::Or, 1, {GuessLowerPartNonWrapped, GuessUpperPartNonWrapped});

  // if x+c overflows, treat it as wrapped.
  Inst *Guess = IC.getInst(Inst::Select, 1, {IsOverflow, GuessOr, GuessAnd});

  std::set<Inst *> ConstSet{ReservedX};
  std::map<Inst *, llvm::APInt> ResultMap;
  SouperConstantSynthesis CS;
  // Before switching to ConcreteInterpreter for LHS simplification, the query
  // is Guess(ReservedX, LHS) == 1 Note there is a reservedconst (ReservedX) in
  // left side of the query. After the switch, the left side of the query needs
  // to be reservedconst free, and we still need LHS to stay on the left side of
  // the query to take care of UB, therefore, the new query is or(trunc(LHS), 1)
  // = Guess(ReservedX, LHS)
  LHS = IC.getInst(
      Inst::Or, 1,
      {IC.getInst(Inst::Trunc, 1, {LHS}), IC.getConst(llvm::APInt(1, true))}),
  CS.synthesize(SMTSolver.get(), BPCs, PCs, InstMapping(LHS, Guess), ConstSet,
                ResultMap, IC, MaxConstantSynthesisTries, Timeout,
                /*AvoidNops=*/false);
  if (ResultMap.empty()) {
    IsFound = false;
  } else {
    IsFound = true;
    ResultX = ResultMap[ReservedX];
  }
}

llvm::ConstantRange ConstSynthesis::constantRange(
    const BlockPCs &BPCs, const std::vector<InstMapping> &PCs, Inst *LHS,
    InstContext &IC) {
  unsigned W = LHS->Width;

  APInt L = APInt(W, 1), R = APInt::getAllOnes(W);
  APInt BinSearchResultX, BinSearchResultC;
  bool BinSearchHasResult = false;

  while (L.ule(R)) {
    APInt M = L + ((R - L)).lshr(1);
    APInt BinSearchX;
    bool Found = false;
    testRange(BPCs, PCs, LHS, M, BinSearchX, Found, IC);
    if (Found) {
      R = M - 1;

      // record result
      BinSearchResultX = BinSearchX;
      BinSearchResultC = M;
      BinSearchHasResult = true;
    } else {
      if (L == R) break;
      L = M + 1;
    }
  }

  if (BinSearchHasResult) {
    return llvm::ConstantRange(BinSearchResultX,
                               BinSearchResultX + BinSearchResultC);
  } else {
    return llvm::ConstantRange(W, true);
  }
}

ConstSynthesis::ConstSynthesis() { SMTSolver = GetUnderlyingSolver(); }

ConstantRange ConstSynthesis::synthesizeConstant(llvm::Function *F,
                                                 llvm::Value *ConstExpr) {
  InstContext IC;

  ExprBuilderContext EBC;
  ExprBuilderOptions Opts;
  llvm::SmallPtrSet<llvm::Value *, 32> Filter = {ConstExpr};
  Opts.CandidateFilterInstructions = &Filter;

  FunctionCandidateSet CS = ExtractCandidates(*F, IC, EBC, Opts);
  auto Cand = getSouperReplacement(CS, ConstExpr, 20);
  if (!Cand)
    return ConstantRange(ConstExpr->getType()->getIntegerBitWidth(), true);

  SouperConstantSynthesis ConstSyn;
  std::map<Inst *, llvm::APInt> ResultMap;

  auto Range =
      this->constantRange(Cand->BPCs, Cand->PCs, Cand->Mapping.LHS, IC);

  return Range;
}
};  // namespace saturn