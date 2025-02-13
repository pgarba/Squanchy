#ifndef SYMLLVM_H
#define SYMLLVM_H

#include <map>
#include <string>

#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"

#include <z3++.h>

namespace symllvm {

typedef struct Z3Entry {
  z3::expr *Exp;

  bool isGEP;
  z3::expr *Index;

  Z3Entry() : Exp(nullptr), isGEP(false), Index(nullptr) {};

  Z3Entry(z3::expr *Exp) : Exp(Exp), isGEP(false), Index(nullptr) {};

  Z3Entry(z3::expr *Exp, bool isGEP, z3::expr *Index)
      : Exp(Exp), isGEP(isGEP), Index(Index) {}

} Z3Entry;

typedef struct _ValueMap {
  bool isConceret = false;
  llvm::ConstantInt *ConceretValue;

  _ValueMap(llvm::ConstantInt *CV) : ConceretValue(CV) { isConceret = true; };
  _ValueMap() {};

} ValueMap;

typedef std::map<llvm::Value *, ValueMap> State;

class Symllvm {
public:
  Symllvm() {};

  ~Symllvm() {};

  bool solveValues(llvm::Function *F, llvm::Instruction *I,
                   llvm::SmallVectorImpl<uint64_t> &Results);

  void setDebug(bool Debug = true) { this->Debug = Debug; };

private:
  bool Debug = false;

  z3::context Z3Ctx;

  std::map<llvm::Value *, z3::sort *> MemoryMap;
  std::map<z3::expr *, z3::expr *> CurrentMemoryState;

  llvm::DenseMap<llvm::Value *, Z3Entry> ValueMap;
  llvm::DenseMap<z3::expr *, unsigned int> BitMap;

  bool isSupportedInstruction(llvm::Value *V);
  bool isSupportedIntrinsic(llvm::CallInst *CI);

  z3::expr *getZ3Val(z3::context &Z3Ctx, llvm::Value *V, int OverrideBitWidth);

  bool dominates(llvm::DominatorTree *DT, const llvm::Instruction *a,
                 const llvm::Instruction *b);

  void handleIntrinsic(llvm::CallInst *I);

  void getAST(llvm::DominatorTree *DT, llvm::Instruction *I,
              llvm::SmallVectorImpl<llvm::Instruction *> &AST,
              llvm::SmallVectorImpl<llvm::Value *> &Variables, bool KeepRoot);

  void printAST(llvm::DominatorTree *DT,
                llvm::SmallVectorImpl<llvm::Instruction *> &AST);

  z3::expr
  getZ3ExpressionFromAST(llvm::SmallVectorImpl<llvm::Instruction *> &AST,
                         llvm::SmallVectorImpl<llvm::Value *> &Variables,
                         std::map<std::string, z3::expr *> &VarMap,
                         int OverrideBitWidth, bool &Error);

  z3::expr boolToBV(z3::expr &BoolExpr, int BitWidth);
  z3::expr getConstant(uint64_t Value, int BitWidth);

  // Helpers to handle the memory
  void updateMemoryState(z3::expr *Memory, z3::expr *CurrentState);
  z3::expr *getMemoryState(z3::expr *Memory);

  z3::expr *storeValueLittleEndian(z3::expr *Memory, z3::expr *Index,
                                   z3::expr *Value);
  z3::expr *storeValueLittleEndian(z3::expr *Memory, z3::expr *Index,
                                   llvm::Value *Value);

  z3::expr *storeValueBigEndian(z3::expr *Memory, z3::expr *Index,
                                z3::expr *Value);
  z3::expr *storeValueBigEndian(z3::expr *Memory, z3::expr *Index,
                                llvm::Value *Value);

  z3::expr *loadValueLittleEndian(z3::expr *Memory, z3::expr *Index,
                                  int BitWidth);
  z3::expr *loadValueBigEndian(z3::expr *Memory, z3::expr *Index, int BitWidth);

  z3::expr *storeValueMemCpy(z3::expr *Memory, z3::expr *Index,
                             z3::expr *Value);
  z3::expr *storeValueMemCpy(z3::expr *Memory, z3::expr *Index,
                             llvm::Value *Value);
};

}; // namespace symllvm

#endif