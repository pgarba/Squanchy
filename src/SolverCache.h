#ifndef SOLVERCACHE_H
#define SOLVERCACHE_H

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/raw_ostream.h>

#include <hiredis/hiredis.h>

#include "Solver.h"

#ifdef _WIN32
struct timeval {
  long tv_sec;  /* seconds */
  long tv_usec; /* and microseconds */
};
#endif

extern llvm::cl::opt<bool> UseRedis;

/*
  Saturn own caching to speed up SAT queries
*/
typedef struct SolverQuery {
  std::error_code EC;
  bool IsSat;
  std::vector<llvm::APInt> ModelVals;

  SolverQuery(bool pIsSat, std::vector<llvm::APInt> *pModelVals,
              std::error_code EC) {
    this->EC = EC;
    this->IsSat = pIsSat;

    // Clone APInts
    if (!pModelVals) return;

    for (auto &I : *pModelVals) {
      ModelVals.push_back(I);
    }
  }

  SolverQuery(){};
} SolverQuery;

class SolverCache {
 public:
  SolverCache() {
    // Connect to CachedSolver
    US = GetUnderlyingSolver();

    if (UseRedis) {
      const char *hostname = "127.0.0.1";
      struct timeval Timeout = {1, 500000};  // 1.5 seconds
      Ctx = redisConnectWithTimeout(hostname, 6379, Timeout);
      if (!Ctx) {
        llvm::report_fatal_error(
            "[SolverCache] Can't allocate redis context\n");
      }
      if (Ctx->err) {
        llvm::report_fatal_error(
            (llvm::StringRef) "[SolverCache] Redis connection error: " +
            Ctx->errstr + "\n");
      }
    }
  };
  ~SolverCache(){};

  void printSaturnSolverStats();

  /*
    Return true on cache hit
  */
  bool getSolverQuery(llvm::StringRef Query, SolverQuery &SQ);

  /*
    Return true on cache hit
  */
  bool getReplacement(llvm::StringRef Query, std::string &Result);

  /*
    Return true on cache hit
  */
  void setReplacement(llvm::StringRef Query, llvm::StringRef Result);

  /*
    Insert a new query/result into the cache
  */
  void insertSolverQuery(llvm::StringRef Query, bool IsSat,
                         std::vector<llvm::APInt> *Models,
                         std::error_code EC = std::error_code(),
                         bool UpdateRedis = true);

  /*
   Cached query isSatisfiable method
  */
  std::error_code isSatisfiableCached(llvm::StringRef Query, bool &Result,
                                      unsigned NumModels,
                                      std::vector<llvm::APInt> *Models,
                                      unsigned Timeout);

  /*
   get Cached query
  */
  bool isCached(llvm::StringRef Query, bool &Result,
                std::vector<llvm::APInt> *Models);

 private:
  int SolverQueries = 0;
  int CacheHits = 0;
  int RedisHits = 0;
  int UniqueReplacements = 0;

  /*
    Redis context
  */
  redisContext *Ctx = nullptr;

  /*
      SMT Solver
  */
  std::unique_ptr<souper::SMTLIBSolver> US;

  /*
    Saturn solver cache to speed up the solving
  */
  llvm::StringMap<SolverQuery> Cache;

  /*
    Lock guard mutex
  */
  std::mutex RedisMutex;

  bool hGet(llvm::StringRef Key, llvm::StringRef Field, std::string &Value);
  void hSet(llvm::StringRef Key, llvm::StringRef Field, llvm::StringRef Value);
};

// The global instance
extern SolverCache SaturnSC;

#endif