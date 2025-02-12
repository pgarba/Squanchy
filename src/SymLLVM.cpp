#include "SymLLVM.h"
#include <llvm/Support/Format.h>

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Transforms/Utils/Evaluator.h"

#include <unordered_set>

#include "ConstSynthesis.h"
#include "ConstantSynthesis.h"
#include "Solver.h"
#include "SolverCache.h"

#include "llvm/AsmParser/Parser.h"
#include <llvm/Support/SourceMgr.h>

using namespace llvm;
using namespace std;

using namespace souper;

extern SolverCache SaturnSC;

typedef struct _ProvedConstant {
  Inst *I;
  uint64_t Arr;
  uint64_t Edge;

  _ProvedConstant(Inst *I, uint64_t Arr, uint64_t Edge)
      : I(I), Arr(Arr), Edge(Edge) {}
} ProvedConstant;

// On Mac we need to set it to 0 otherwise sys::ExecuteAndWait will directly
// return with -2
int SolverTimeout = 0;
unsigned DebugLevel;

int solveEdges(llvm::Function *F, uint64_t BBVA, llvm::BasicBlock *B,
               llvm::Value *VDest, int BitNum,
               llvm::SmallVectorImpl<uint64_t> &SolvedValues) {

  BitNum = VDest->getType()->getIntegerBitWidth();

  // Use Souper
  InstContext IC;
  ExprBuilderContext EBC;
  FunctionCandidateSet CS;

  // Get the candidates (Like ExtractCandidatesFromPass in Pass.cpp)
  ExprBuilderOptions Opts;
  llvm::SmallPtrSet<llvm::Value *, 32> Filter = {VDest};
  Opts.CandidateFilterInstructions = &Filter;
  CS = ExtractCandidates(*F, IC, EBC, Opts);

  bool IsSat = false;
  souper::CandidateReplacement *CR = nullptr;

  for (auto &B : CS.Blocks) {
    for (auto &R : B->Replacements) {
      if (R.Origin != VDest || R.Mapping.LHS->Width != BitNum)
        continue;

      CR = &R;
      break;
    }
  }

  // Could not find a candidate
  if (!CR) {
    VDest->dump();

    report_fatal_error(
        "No candidate found for solving! (Might be a non reachable "
        "BB)\n");
    return 0;
  }

  // Print CR
  //ReplacementContext Context;
  //CR->printLHS(llvm::outs(), Context);

  // Get solutions
  souper::Inst *Const = nullptr;
  std::vector<ProvedConstant> RecoveredConstants;

  // Create a new variable
  Inst *I = IC.createVar(CR->Mapping.LHS->Width, "constant");

  // Map the variable to left hand side
  InstMapping Mapping(CR->Mapping.LHS, I);

  do {
    std::vector<Inst *> ModelInsts;
    std::vector<llvm::APInt> ModelVals;

    // Build Precondition
    vector<Inst *> ConstChecks;
    for (auto &C : RecoveredConstants) {
      Inst *NotConst = IC.getInst(Inst::Ne, BitNum, {CR->Mapping.LHS, C.I});
      ConstChecks.push_back(NotConst);
    }
    souper::Inst *Precondition = nullptr;
    if (ConstChecks.size()) {
      Precondition = IC.getInst(Inst::And, 1, ConstChecks);
    }

    // Build Query
    std::string Query = BuildQuery(IC, CR->BPCs, CR->PCs, Mapping, &ModelInsts,
                                   Precondition, true);

    /*
        outs() << "[Slicer] SMT Query:\n";
        outs() << Query << "\n";
    */
    // Solve it (cached)
    SaturnSC.isSatisfiableCached(Query, IsSat, ModelInsts.size(), &ModelVals,
                                 SolverTimeout);

    // Check if we got a solution
    if (!IsSat) {
      break;
    }

    // Parse return and get our constant variable
    Const = nullptr;
    int ValIndex = 0;
    for (int i = 0; i < ModelVals.size(); i++) {
      if (ModelInsts[i]->Name == "constant") {
        Const = IC.getConst(ModelVals[i]);
        ValIndex = i;
      }
    }
    // No constant found
    if (!Const) {
      break;
    }

    // Store solution
    souper::Inst *Arr = IC.getConst(ModelVals[ValIndex]);
    auto ProvedCon = ProvedConstant(Const, Arr->Val.getLimitedValue(),
                                    Const->Val.getLimitedValue());
    RecoveredConstants.push_back(ProvedCon);

    // We dont need this here anymore
    // Hard stop
    if (RecoveredConstants.size() > 10) {
      // report_fatal_error("To many edges recovered! (Verify this!)");
      // outs() << "[Slicer] WIP: More than 10 edges solved ... Aborting
      // here!\n";
      return 0;
    }
  } while (Const);

  // Debug output
  /*
  outs() << "[Slicer] Souper Solved Edges: ";
  for (auto E : RecoveredConstants) {
    outs() << llvm::format_hex(E.Edge, 8) << " ";
  }
  outs() << "\n";
  */

  // Create edges
  if (RecoveredConstants.size() > 2) {
    report_fatal_error("[Slicer] RecoveredConstants > 2.... verify!\n");
  }

  for (auto &C : RecoveredConstants) {
    SolvedValues.push_back(C.Edge);
  }

  return RecoveredConstants.size();
}

bool symllvm::Symllvm::isSupportedIntrinsic(llvm::CallInst *CI) {
  auto F = CI->getCalledFunction();
  if (!F->isIntrinsic())
    return false;

  // llvm.ctpop.i8 intrinsic
  // llvm.fshl.i64
  switch (F->getIntrinsicID()) {
  case Intrinsic::ctpop:
  case Intrinsic::fshl:
    return true;
  }

  if (this->Debug) {
    outs() << "[SymLLVM] Unsupported intrinsic: ";
    CI->dump();
  }

  return false;
}

bool symllvm::Symllvm::isSupportedInstruction(llvm::Value *V) {
  if (auto BO = dyn_cast<BinaryOperator>(V)) {
    // Got removed from constant expr
    if (BO->getOpcode() == Instruction::Shl ||
        BO->getOpcode() == Instruction::Or ||
        BO->getOpcode() == Instruction::And ||
        BO->getOpcode() == Instruction::LShr ||
        BO->getOpcode() == Instruction::AShr) {
      return true;
    }

    return ConstantExpr::isSupportedBinOp(BO->getOpcode());
  }

  if (isa<TruncInst>(V)) {
    return true;
  }

  if (isa<ZExtInst>(V)) {
    return true;
  }

  if (isa<SExtInst>(V)) {
    return true;
  }

  if (isa<LoadInst>(V)) {
    return true;
  }

  if (isa<StoreInst>(V)) {
    return true;
  }

  if (isa<ICmpInst>(V)) {
    return true;
  }

  if (isa<SelectInst>(V)) {
    return true;
  }

  if (isa<GetElementPtrInst>(V)) {
    return true;
  }

  if (isa<CallInst>(V)) {
    return isSupportedIntrinsic(cast<CallInst>(V));
  }

  if (isa<ReturnInst>(V)) {
    return true;
  }

  if (isa<AllocaInst>(V)) {
    return true;
  }

  if (this->Debug) {
    outs() << "[SymLLVM] Unsupported instruction: ";
    V->dump();
  }

  return false;
}

bool symllvm::Symllvm::dominates(DominatorTree *DT, const Instruction *a,
                                 const Instruction *b) {
  // a < b -> true else false
  if (a->getParent() == b->getParent()) {
    return a->comesBefore(b);
  }

  DomTreeNode *DA = DT->getNode(a->getParent());
  DomTreeNode *DB = DT->getNode(b->getParent());

  // return DT->dominates(DA, DB);
  if (DA->getLevel() < DB->getLevel())
    return true;

  return false;
}

void symllvm::Symllvm::getAST(llvm::DominatorTree *DT, llvm::Instruction *I,
                              llvm::SmallVectorImpl<llvm::Instruction *> &AST,
                              llvm::SmallVectorImpl<llvm::Value *> &Variables,
                              bool KeepRoot) {
  // Only work on supported operands
  if (isSupportedInstruction(I) == false) {
    return;
  }

  // Walk the AST in BFS
  std::deque<llvm::Value *> Q;
  std::set<llvm::Value *> Dis;
  std::unordered_set<llvm::Value *> Vars;

  int Depth = 0;

  // Mark root as discovered
  Dis.insert(I);

  if (KeepRoot) {
    AST.push_back(I);
  }

  // Run BFS
  Q.push_front(I);
  while (!Q.empty()) {
    auto v = Q.back();
    Q.pop_back();

    // We are only following instructions
    auto Ins = dyn_cast<Instruction>(v);
    if (!Ins)
      continue;

    for (auto &O : Ins->operands()) {
      if (isa<Constant>(O)) {
        continue;
      }

      // Must be a variable
      if (isa<Argument>(O)) {
        Vars.insert(O);
        continue;
      }

      auto OpIns = dyn_cast<Instruction>(O->stripPointerCasts());
      if (OpIns) {
        if (Dis.find(OpIns) != Dis.end())
          continue;

        // Check if supported
        if (!isSupportedInstruction(OpIns)) {
          // Use as variable
          Vars.insert(OpIns);
          continue;
        }

        Dis.insert(OpIns);
        Q.push_front(OpIns);

        AST.push_back(OpIns);

      } else {
        // Investigate
        O->print(outs());
        report_fatal_error("Unknown Inst!", false);
      }
    }

    // Check Uses for a store
    for (auto U : Ins->users()) {
      auto SI = dyn_cast<StoreInst>(U);
      if (!SI)
        continue;

      // Skip stores that dominate the current instruction
      if (dominates(DT, dyn_cast<Instruction>(U), Ins))
        continue;

      // Skip stores that come after the instruction we  for
      if (dominates(DT, I, dyn_cast<Instruction>(U)))
        continue;

      // Skip stores that are in different level in different blocks
      if (SI->getParent() != I->getParent() &&
          DT->getNode(SI->getParent())->getLevel() ==
              DT->getNode(I->getParent())->getLevel()) {
        continue;
      }

      // Skip stores that are already in the AST
      if (Dis.find(SI) != Dis.end())
        continue;

      Dis.insert(SI);
      Q.push_front(SI);

      AST.push_back(SI);
    }
  }

  // Stupid sort algorithm
  for (int i = 0; i < (AST.size() - 1); i++) {
    for (int j = i + 1; j < AST.size(); j++) {
      if (dominates(DT, AST[i], AST[j])) {
        std::swap(AST[i], AST[j]);
      }
    }
  }

  std::reverse(AST.begin(), AST.end());

  if (Debug) {
    printAST(DT, AST);
  }

  // Verify the dominance
  for (int i = 0; i < (AST.size() - 1); i++) {
    if (!dominates(DT, AST[i], AST[i + 1])) {
      outs() << "No Dom: AST[" << i << "] ";
      AST[i]->print(outs());
      outs() << " < AST[" << i + 1 << "] ";
      AST[i + 1]->dump();
      // report_fatal_error("Invalid AST!", false);
    }
  }

  // Fill Variables
  for (auto V : Vars) {
    Variables.push_back(V);
  }

  // Sort Variables
  std::sort(Variables.begin(), Variables.end());
}

void symllvm::Symllvm::printAST(
    DominatorTree *DT, llvm::SmallVectorImpl<llvm::Instruction *> &AST) {
  outs() << "[*] AST (Operators: " << AST.size() << "):\n";

  int i = 0;
  for (auto &e : AST) {
    // DomTreeNode *DA = DT->getNode(e->getParent());

    // outs() << "  [" << i << "/" << DA->getLevel() << "] ";
    e->print(outs());
    outs() << "\n";
    i++;
  }
};

z3::expr *
symllvm::Symllvm::getZ3Val(z3::context &Z3Ctx, llvm::Value *V,
                           llvm::DenseMap<llvm::Value *, z3::expr *> &ValueMap,
                           int OverrideBitWidth) {
  if (ConstantInt *CV = dyn_cast<ConstantInt>(V)) {
    int BitWidth = 0;
    if (OverrideBitWidth) {
      BitWidth = OverrideBitWidth;
    } else {
      BitWidth = CV->getBitWidth();
    }

    z3::expr *Z3Val = nullptr;
    if (CV->isNegative()) {
      auto ConstExpr = Z3Ctx.bv_val(CV->getSExtValue(), BitWidth);
      Z3Val = new z3::expr(ConstExpr);
    } else {
      auto ConstExpr = Z3Ctx.bv_val(CV->getZExtValue(), BitWidth);
      Z3Val = new z3::expr(ConstExpr);
    }

    ValueMap[V] = Z3Val;

    return Z3Val;
  }

  if (ValueMap.count(V) == 0) {
    outs() << "\nValue: ";
    V->dump();
    report_fatal_error("[getZ3Val] Value not found!");
  }

  return ValueMap[V];
}

z3::expr symllvm::Symllvm::getZ3ExpressionFromAST(
    llvm::SmallVectorImpl<llvm::Instruction *> &AST,
    llvm::SmallVectorImpl<llvm::Value *> &Variables,
    std::map<std::string, z3::expr *> &VarMap, int OverrideBitWidth,
    bool &Error) {
  llvm::DenseMap<llvm::Value *, z3::expr *> ValueMAP;
  llvm::DenseMap<z3::expr *, unsigned int> BitMap;

  Error = false;

  // Create Variables
  char Var = 'a';
  for (auto V : Variables) {
    std::string VarStr = "_" + std::string(1, Var);
    if (V->hasName()) {
      VarStr = V->getName();
    }

    // Check if Ptr
    int BitWidth = 64;
    if (!V->getType()->isPointerTy()) {
      BitWidth = V->getType()->getIntegerBitWidth();
    }

    auto VExpr = Z3Ctx.bv_const(VarStr.c_str(), BitWidth);

    ValueMAP[V] = new z3::expr(VExpr);
    VarMap[VarStr] = ValueMAP[V];
    BitMap[ValueMAP[V]] = BitWidth;

    Var++;
  }

  // Loop over BinOps
  z3::expr *LastInst = nullptr;
  int i = 0;
  for (auto E = AST.begin(); E != AST.end(); ++E) {
    auto CurInst = *E;
    if (Debug) {
      outs() << "[" << i << "] ";
      CurInst->print(outs());
      outs() << " ";
      i++;
    }

    auto BO = dyn_cast<BinaryOperator>(CurInst);
    if (BO) {
      switch (BO->getOpcode()) {
      case Instruction::BinaryOps::Add: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) +
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::Sub: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) -
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::Mul: {
        auto a = getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth);
        auto exp = *a * *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP,
                                  OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::SDiv: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) /
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::Xor: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) ^
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::And: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) &
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::Or: {
        auto exp =
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth) |
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth);
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::Shl: {
        auto exp = z3::shl(
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth),
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth));
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::LShr: {
        auto exp = z3::lshr(
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth),
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth));
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      case Instruction::BinaryOps::AShr: {
        auto exp = z3::ashr(
            *getZ3Val(Z3Ctx, BO->getOperand(0), ValueMAP, OverrideBitWidth),
            *getZ3Val(Z3Ctx, BO->getOperand(1), ValueMAP, OverrideBitWidth));
        ValueMAP[BO] = new z3::expr(exp);
      } break;
      default: {
        BO->print(outs());
        Error = true;
        report_fatal_error("Unknown opcode!");
      }
      }

      BitMap[ValueMAP[BO]] = BO->getType()->getIntegerBitWidth();

    } else if (auto Trunc = dyn_cast<llvm::TruncInst>(CurInst)) {
      //  Trunc
      auto exp =
          getZ3Val(Z3Ctx, Trunc->getOperand(0), ValueMAP, OverrideBitWidth)
              ->extract(Trunc->getType()->getIntegerBitWidth() - 1, 0);

      ValueMAP[Trunc] = new z3::expr(exp);
      BitMap[ValueMAP[Trunc]] = BitMap[ValueMAP[Trunc->getOperand(0)]];
    } else if (auto ZExt = dyn_cast<ZExtInst>(CurInst)) {
      // ZExt
      auto V = getZ3Val(Z3Ctx, ZExt->getOperand(0), ValueMAP, OverrideBitWidth);

      // Convert bool to bv if needed
      if (V->get_sort().is_bool()) {
        *V = boolToBV(*V, 1);
      }

      auto exp = z3::zext(
          *V, ZExt->getType()->getIntegerBitWidth() -
                  ZExt->getOperand(0)->getType()->getIntegerBitWidth());
      ValueMAP[ZExt] = new z3::expr(exp);
      BitMap[ValueMAP[ZExt]] = BitMap[ValueMAP[ZExt->getOperand(0)]];
    } else if (auto SExt = dyn_cast<SExtInst>(CurInst)) {
      // SExt
      auto V = getZ3Val(Z3Ctx, SExt->getOperand(0), ValueMAP, OverrideBitWidth);

      // Convert bool to bv if needed
      if (V->get_sort().is_bool()) {
        *V = boolToBV(*V, 1);
      }

      auto exp = z3::sext(
          *V, SExt->getType()->getIntegerBitWidth() -
                  SExt->getOperand(0)->getType()->getIntegerBitWidth());

      ValueMAP[SExt] = new z3::expr(exp);
      BitMap[ValueMAP[SExt]] = BitMap[ValueMAP[SExt->getOperand(0)]];
    } else if (auto GEP = dyn_cast<GetElementPtrInst>(CurInst)) {
      // GEP
      // Check if RAM
      if (GEP->getPointerOperand()->getName() != "RAM") {
        GEP->dump();
        outs() << "[SYMLLVM E] GEP not based on RAM!\n";
        Error = true;
        return z3::expr(Z3Ctx);
      }

      auto Index = GEP->getOperand(2);
      if (isa<ConstantInt>(Index)) {
        // Concrete
        report_fatal_error("[GEP] Implement me!", false);
      } else {
        // Symbolic
        // Todo: Just assign value for now
        ValueMAP[GEP] = ValueMAP[Index];
        BitMap[ValueMAP[GEP]] = BitMap[ValueMAP[Index]];
      }
    } else if (auto Load = dyn_cast<LoadInst>(CurInst)) {
      // Load
      // Todo: Use Memory Manager to get concrete values
      auto PtrValue = ValueMAP[Load->getOperand(0)];

      // Check if bitwidth matches
      int NewValueBitWidth = 0;
      auto CurValueBitWidth = BitMap[PtrValue];
      if (Load->getType()->isPointerTy()) {
        NewValueBitWidth = CurValueBitWidth;
      } else {
        NewValueBitWidth = Load->getType()->getIntegerBitWidth();
      }

      if (CurValueBitWidth == NewValueBitWidth) {
        // Just Map
        ValueMAP[Load] = ValueMAP[Load->getPointerOperand()];
        BitMap[ValueMAP[Load]] = BitMap[ValueMAP[Load->getPointerOperand()]];
      } else if (CurValueBitWidth > NewValueBitWidth) {
        // Apply Trunc
        auto exp =
            PtrValue->extract(Load->getType()->getIntegerBitWidth() - 1, 0);

        ValueMAP[Load] = new z3::expr(exp);
        BitMap[ValueMAP[Load]] = NewValueBitWidth;
      } else {
        // Might happen when we are missing the Ptr Value in the AST
        Error = true;
        Load->dump();
        Load->getPointerOperand()->dump();
        report_fatal_error("[Load] Implement me!", false);
      }
    } else if (auto Store = dyn_cast<StoreInst>(CurInst)) {
      // Store
      // Check if bitwidth is the same
      auto CurValue = ValueMAP[Store->getOperand(1)];
      auto NewValue = Store->getOperand(0);

      int NewValueBitWidth = 0;
      auto CurValueBitWidth = BitMap[CurValue];
      if (NewValue->getType()->isPointerTy()) {
        NewValueBitWidth = CurValueBitWidth;
      } else {
        NewValueBitWidth = NewValue->getType()->getIntegerBitWidth();
      }

      // Check if same bitwidth
      if (CurValueBitWidth == NewValueBitWidth) {
        auto V = *getZ3Val(Z3Ctx, NewValue, ValueMAP, OverrideBitWidth);

        // Overwrite current value with new value
        V = (*CurValue & 0) | V;

        // Update CurValue
        ValueMAP[Store->getOperand(1)] = new z3::expr(V);
        BitMap[ValueMAP[Store->getOperand(1)]] = CurValueBitWidth;
      } else {
        // Need to apply some boolean magic
        if (CurValueBitWidth > NewValueBitWidth) {
          // Create high bits bitmask with all ones
          uint64_t BitMask = -1;
          switch (NewValueBitWidth) {
          case 32: {
            BitMask = BitMask - (uint32_t)-1;
            break;
          }
          case 16: {
            BitMask = BitMask - (uint16_t)-1;
            break;
          }
          case 8: {
            BitMask = BitMask - (uint8_t)-1;
            break;
          }
          case 1: {
            BitMask = BitMask - 1;
            break;
          }
          default:
            report_fatal_error("[Store] Implement me!", false);
          }

          // Zext V to Destination Type
          auto V = *getZ3Val(Z3Ctx, NewValue, ValueMAP, NewValueBitWidth);
          auto VZext = z3::zext(V, CurValueBitWidth - NewValueBitWidth);

          V = *CurValue & Z3Ctx.bv_val(BitMask, CurValueBitWidth) | VZext;

          // Update CurValue
          ValueMAP[Store->getOperand(1)] = new z3::expr(V);
          BitMap[ValueMAP[Store->getOperand(1)]] = CurValueBitWidth;
        } else {
          // Todo
          Store->getOperand(1)->dump();
          report_fatal_error("[Store] Implement me 2!", false);
        }
      }
    } else if (auto ICmp = dyn_cast<ICmpInst>(CurInst)) {
      // ICmp
      auto V0 = getZ3Val(Z3Ctx, ICmp->getOperand(0), ValueMAP, false);
      auto V1 = getZ3Val(Z3Ctx, ICmp->getOperand(1), ValueMAP, false);

      z3::expr *Res = nullptr;
      switch (ICmp->getPredicate()) {
      case llvm::ICmpInst::ICMP_EQ: {
        Res = new z3::expr(*V0 == *V1);
      } break;
      case llvm::ICmpInst::ICMP_NE:
        Res = new z3::expr(*V0 != *V1);
        break;
      case llvm::ICmpInst::ICMP_UGT:
        Res = new z3::expr(z3::ugt(*V0, *V1));
        break;
      case llvm::ICmpInst::ICMP_UGE:
        Res = new z3::expr(z3::uge(*V0, *V1));
        break;
      case llvm::ICmpInst::ICMP_ULT:
        Res = new z3::expr(z3::ult(*V0, *V1));
        break;
      case llvm::ICmpInst::ICMP_ULE:
        Res = new z3::expr(z3::ule(*V0, *V1));
        break;
      case llvm::ICmpInst::ICMP_SGT:
        Res = new z3::expr(*V0 > *V1);
        break;
      case llvm::ICmpInst::ICMP_SGE:
        Res = new z3::expr(*V0 >= *V1);
        break;
      case llvm::ICmpInst::ICMP_SLT:
        Res = new z3::expr(*V0 < *V1);
        break;
      case llvm::ICmpInst::ICMP_SLE:
        Res = new z3::expr(*V0 > *V1);
        break;
      default:
        report_fatal_error("Unsupported Predicate!", false);
      }

      ValueMAP[ICmp] = Res;
      BitMap[ValueMAP[ICmp]] = ICmp->getType()->getIntegerBitWidth();
    } else if (auto Select = dyn_cast<SelectInst>(CurInst)) {
      // Select
      auto Cond = getZ3Val(Z3Ctx, Select->getCondition(), ValueMAP, false);
      auto VTrue = getZ3Val(Z3Ctx, Select->getTrueValue(), ValueMAP, false);
      auto VFalse = getZ3Val(Z3Ctx, Select->getFalseValue(), ValueMAP, false);

      // Get BitWidth
      int VTrueBitWidth =
          Select->getTrueValue()->getType()->getIntegerBitWidth();
      int VFalseBitWidth =
          Select->getFalseValue()->getType()->getIntegerBitWidth();

      // Cast to bool if needed
      if (Cond->get_sort().is_bool() == false) {
        Cond = new z3::expr(Cond->bit2bool(0));
      }

      // Cast bool to bv if needed
      if (VTrueBitWidth == 1 && VTrue->get_sort().is_bool() == false) {
        VTrue = new z3::expr(VTrue->bit2bool(0));
      }

      // Check is cast to bool is needed
      if (VFalseBitWidth == 1 && VFalse->get_sort().is_bool() == false) {
        VFalse = new z3::expr(VFalse->bit2bool(0));
      }

      auto Res = z3::ite(*Cond, *VTrue, *VFalse);

      ValueMAP[Select] = new z3::expr(Res);
      BitMap[ValueMAP[Select]] = Select->getType()->getIntegerBitWidth();
    } else if (auto Call = dyn_cast<CallInst>(CurInst)) {
      // Must be an intrinsic
      handleIntrinsic(Call, ValueMAP, BitMap);
    } else if (auto Ret = dyn_cast<ReturnInst>(CurInst)) {
      ValueMAP[Ret] = getZ3Val(Z3Ctx, Ret->getReturnValue(), ValueMAP, 0);
    } else if (auto Alloca = dyn_cast<AllocaInst>(CurInst)) {
      // Alloca
      ValueMAP[CurInst] = new z3::expr(Z3Ctx.bv_const("Alloca", 32));
      BitMap[ValueMAP[CurInst]] = 32;
    } else {
      CurInst->dump();
      report_fatal_error("[getZ3ExpressionFromAST] Unsupported instruction!");
    }

    if (Debug) {
      outs() << "Bit: " << BitMap[ValueMAP[CurInst]] << "\n";
    }

    // Set last inst
    LastInst = ValueMAP[CurInst];
  }

  // Map LastInst to Output
  if (!LastInst) {
    Error = true;
    report_fatal_error("[getZ3ExpressionFromAST] LastInst is null!");
  }
  z3::expr Result = *LastInst;

  // Clean up
  /*
  for (auto V : ValueMAP) {
    // Skip Vars
    bool Found = false;
    for (auto &E : VarMap) {
      if (E.second == V.second) {
        Found = true;
        break;
      }
    }

    if (Found)
      continue;

    delete V.second;
  }
  */

  return Result;
}

bool symllvm::Symllvm::solveValues(llvm::Function *F, llvm::Instruction *I,
                                   llvm::SmallVectorImpl<uint64_t> &Results) {

  // Test
  solveEdges(F, 1000, I->getParent(), I, 32, Results);
  return !Results.empty();

  // Create DT
  DominatorTree DT(*F);

  // Add GSBase, ESBase, FSBase as default variables
  SmallVector<llvm::Value *, 16> Variables;

  // Get the AST
  SmallVector<llvm::Instruction *, 32> AST;
  this->getAST(&DT, I, AST, Variables, true);

  if (AST.empty()) {
    return false;
  }

  // Check if we have any constantexpr gep
  for (auto &E : AST) {
    for (auto &OP : E->operands()) {
      auto CE = dyn_cast<ConstantExpr>(OP);
      if (!CE)
        continue;

      if (CE->getOpcode() == Instruction::GetElementPtr) {
        // Check if index is constant
        if (!isa<ConstantInt>(CE->getOperand(2)))
          continue;

        // Create a Variable
        Variables.push_back(CE);
      }
    }
  }

  // And now solve the AST
  std::map<std::string, z3::expr *> VarMap;
  bool Error = false;
  auto Z3Expr =
      this->getZ3ExpressionFromAST(AST, Variables, VarMap, false, Error);

  // Check if Z3Expr is empty
  if (Error) {
    return false;
  }

  // z3::tactic t(Z3Ctx, "qflia");  // 0-1ms , "smt" 11ms
  // auto s = t.mk_solver();

  auto t = (z3::tactic(Z3Ctx, "simplify") & z3::tactic(Z3Ctx, "bit-blast") &
            z3::tactic(Z3Ctx, "smt"));
  auto s = t.mk_solver();

  // Convert bool to bitvecror
  if (Z3Expr.is_bool()) {
    Z3Expr = z3::ite(Z3Expr, Z3Ctx.bv_val(1, 32), Z3Ctx.bv_val(0, 32));
  }

  z3::check_result IsSat;
  do {
    // Get BitWidth
    int BitWidth = Z3Expr.get_sort().bv_size();

    // Check if always zero
    z3::expr VResult = Z3Ctx.bv_const("Result", BitWidth);

    outs() << Z3Expr.to_string() << " " << Z3Expr.is_bool() << " "
           << Z3Expr.is_bv() << " " << Z3Expr.get_sort().to_string() << "\n";

    auto expr = ((Z3Expr - VResult) == 0);

    // Add old results to find new ones
    for (auto &R : Results) {
      auto C = Z3Ctx.bv_val(R, BitWidth);
      expr = expr && (C != VResult);
    }

    // Add to solver
    s.reset();
    s.add(expr);

    if (Debug) {
      llvm::outs() << "[SMT2 Start]\n" << s.to_smt2() << "[SMT2 End]\n";
    }

    // Check if in Redis or Cache
    auto SMT = s.to_smt2();
    std::vector<llvm::APInt> Models;
    bool QueryIsSat = false;

    // Solve here
    if (Debug) {
      // Measure time
      auto Start = std::chrono::high_resolution_clock::now();

      // Solve ...
      IsSat = s.check();

      // Measure time
      auto End = std::chrono::high_resolution_clock::now();
      auto Duration =
          std::chrono::duration_cast<std::chrono::milliseconds>(End - Start)
              .count();

      llvm::outs() << "Time: " << Duration << "ms\n";
    } else {
      // Solve ...
      IsSat = s.check();
    }

    if (IsSat == z3::sat) {
      // Get Result
      auto m = s.get_model();
      uint64_t Result64 = m.eval(VResult).as_uint64();

      // Check if we have the result already
      bool Found = false;
      for (auto &R : Results) {
        if (R == Result64) {
          Found = true;
          break;
        }
      }

      if (!Found) {
        Results.push_back(Result64);
      }
    }
  } while (IsSat == z3::sat);

  // Print Results
  if (Debug) {
    outs() << "Results: ";
    for (auto &R : Results) {
      outs() << format_hex(R, 8) << " ";
    }
    outs() << "\n";
  }

  return true;
}

void symllvm::Symllvm::handleIntrinsic(
    llvm::CallInst *I, llvm::DenseMap<llvm::Value *, z3::expr *> &ValueMAP,
    llvm::DenseMap<z3::expr *, unsigned int> &BitMap) {
  if (I->getCalledFunction()->getIntrinsicID() == Intrinsic::ctpop) {
    // ctpop
    // Based on:
    // https://book-of-gehn.github.io/articles/2021/05/17/Verifying-Some-Bithacks.html

    auto v = getZ3Val(this->Z3Ctx, I->getOperand(0), ValueMAP, 0);

    // This works for 1 - 64 bit
    int BitWidth = I->getType()->getIntegerBitWidth();

    auto UL3 = Z3Ctx.bv_val((uint64_t)0x5555555555555555, BitWidth);
    auto UL15 = Z3Ctx.bv_val((uint64_t)0x3333333333333333, BitWidth);
    auto UL255a = Z3Ctx.bv_val((uint64_t)0xf0f0f0f0f0f0f0f, BitWidth);
    auto UL255b = Z3Ctx.bv_val((uint64_t)0x101010101010101, BitWidth);

    *v = *v - ((z3::lshr(*v, 1))&UL3);              // temp
    *v = (*v & UL15) + (z3::lshr(*v, 2) & UL15);    // temp
    *v = (*v + z3::lshr(*v, 4)) & UL255a;           // temp
    auto c = z3::lshr((*v * UL255b), BitWidth - 8); // count

    ValueMAP[I] = new z3::expr(c);
    BitMap[ValueMAP[I]] = I->getType()->getIntegerBitWidth();
  } else if (I->getCalledFunction()->getIntrinsicID() == Intrinsic::fshl) {
    // fshl
    int BitWidth = I->getType()->getIntegerBitWidth();

    auto op1 = getZ3Val(this->Z3Ctx, I->getOperand(0), ValueMAP, 0);
    auto op2 = getZ3Val(this->Z3Ctx, I->getOperand(1), ValueMAP, 0);
    auto op3 = getZ3Val(this->Z3Ctx, I->getOperand(2), ValueMAP, 0);

    // Concat [op1, op2]
    auto Concat = z3::concat(*op1, *op2);

    // Zext op3 to Concat type
    auto ZextOp3 = z3::zext(*op3, BitWidth);

    // Shift left
    auto Shift = z3::shl(Concat, ZextOp3);

    // Extract upper part
    auto Res = Shift.extract((BitWidth * 2 - 1), BitWidth);

    ValueMAP[I] = new z3::expr(Res);
    BitMap[ValueMAP[I]] = I->getType()->getIntegerBitWidth();
  } else {
    I->dump();
    report_fatal_error("[handleIntrinsic] Implement me!", false);
  }
}

z3::expr symllvm::Symllvm::getConstant(uint64_t Value, int BitWidth) {
  return Z3Ctx.bv_val(Value, BitWidth);
}

z3::expr symllvm::Symllvm::boolToBV(z3::expr &BoolExpr, int BitWidth) {
  // Do nothing if already bv
  if (!BoolExpr.get_sort().is_bool()) {
    return BoolExpr;
  }

  auto One = Z3Ctx.bv_val(1, BitWidth);
  auto Zero = Z3Ctx.bv_val(0, BitWidth);

  return z3::ite(BoolExpr, One, Zero);
}
