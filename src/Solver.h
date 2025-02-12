#ifndef SOLVER_H
#define SOLVER_H

#include <llvm/IR/Instruction.h>
#include <souper/Extractor/Solver.h>

std::unique_ptr<souper::SMTLIBSolver> GetUnderlyingSolver();

souper::CandidateReplacement *getSouperReplacement(
    souper::FunctionCandidateSet &CS, llvm::Value *Candidate,
    int MaxCandidateInstCount);

souper::CandidateReplacement *getSouperCandidate(
    souper::FunctionCandidateSet &FCS, llvm::Value *I, int CandidateBitWidth);

#endif