#include "SolverCache.h"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/StringExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/PrettyStackTrace.h>

#include <string>

// Use Redis option
llvm::cl::opt<bool> UseRedis(
    "use-redis", llvm::cl::Optional,
    llvm::cl::desc("Use redis cache for saturn solver queries"),
    llvm::cl::value_desc("use-redis"), llvm::cl::init(true));

// SolveCache instance
SolverCache SaturnSC;

void SolverCache::printSaturnSolverStats() {
  llvm::outs() << "[SolverCache] Solver queries: " << SolverQueries
               << " Cache hits: " << (CacheHits) << " ("
               << llvm::format("%0.2f",
                               ((float)(CacheHits)*100. / (float)SolverQueries))
               << "%) "
               << " Redis hits: " << RedisHits
               << " Unique Replacements: " << UniqueReplacements
               << " Missed cached/redis hits: "
               << SolverQueries - CacheHits - RedisHits << "\n";
}

bool SolverCache::getSolverQuery(llvm::StringRef Query, SolverQuery &SQ) {
  ++SolverQueries;

  auto Q = Cache.find(Query);
  if (Q == Cache.end()) return false;

  ++CacheHits;

  SQ = Q->second;

  return true;
}

bool SolverCache::getReplacement(llvm::StringRef Query, std::string &Result) {
  ++SolverQueries;

  if (!Ctx) return false;

  if (hGet(Query, "result", Result)) {
    ++RedisHits;

    // Check if empty result
    if (Result == "<empty>") Result = "";

    return true;
  }

  return false;
}

void SolverCache::setReplacement(llvm::StringRef Query,
                                 llvm::StringRef Result) {
  if (!Ctx) return;

  hSet(Query, "result", Result);

  ++UniqueReplacements;
}

void SolverCache::insertSolverQuery(llvm::StringRef Query, bool IsSat,
                                    std::vector<llvm::APInt> *Models,
                                    std::error_code EC, bool UpdateRedis) {
  // Update internal Cache
  Cache[Query] = SolverQuery(IsSat, Models, EC);

  ++UniqueReplacements;

  // Update redis only when no EC exists
  if (!UpdateRedis || EC || !Ctx || !UseRedis) {
    return;
  }

  // Build result string
  std::string Result;

  // Add IsSat result
  if (IsSat == true) {
    Result += "1";
  } else {
    Result += "0";
  }

  // Add Models
  if (Models) {
    for (auto &I : *Models) {
      Result +=
          "," + llvm::utostr(I.getBitWidth()) + ":" + toString(I, 10, false);
    }
  }

  // Update redis
  hSet(Query, "result", Result);
}

std::error_code SolverCache::isSatisfiableCached(
    llvm::StringRef Query, bool &Result, unsigned NumModels,
    std::vector<llvm::APInt> *Models, unsigned Timeout) {
  std::error_code EC;

  // Check if we have a cached result
  SolverQuery SQ;
  if (SaturnSC.getSolverQuery(Query, SQ)) {
    Result = SQ.IsSat;

    // Clone APInts
    if (Models) {
      for (auto &I : SQ.ModelVals) {
        Models->push_back(I);
      }
    }
    return SQ.EC;
  }

  // Check Redis cache
  std::string ResultStr;
  if (UseRedis && Ctx && hGet(Query, "result", ResultStr)) {
    ++RedisHits;
    if (ResultStr != "") {
      // Parse Result
      llvm::SmallVector<llvm::StringRef, 8> VResult;
      llvm::StringRef R = ResultStr;
      R.split(VResult, ",");

      // Set IsSat
      Result = false;
      if (VResult[0].trim() == "1") Result = true;

      // Add models
      for (int i = 1; i < VResult.size(); i++) {
        auto RSplit = VResult[i].split(':');
        llvm::APInt I(std::stoi(RSplit.first.str()), RSplit.second, 10);
        Models->push_back(I);
      }

      // Update internal cache
      SaturnSC.insertSolverQuery(Query, Result, Models, EC, false);

      return EC;
    }
  }

  // No cache entry or empty result so solve it
  EC = US.get()->isSatisfiable(Query, Result, NumModels, Models, Timeout);

  // Update cache
  SaturnSC.insertSolverQuery(Query, Result, Models, EC);

  return EC;
}

bool SolverCache::isCached(llvm::StringRef Query, bool &Result,
                           std::vector<llvm::APInt> *Models) {
  // Check if we have a cached result
  SolverQuery SQ;
  if (SaturnSC.getSolverQuery(Query, SQ)) {
    Result = SQ.IsSat;

    // Clone APInts
    if (Models) {
      for (auto &I : SQ.ModelVals) {
        Models->push_back(I);
      }
    }
    return true;
  }

  // Check Redis cache
  std::string ResultStr;
  if (UseRedis && Ctx && hGet(Query, "result", ResultStr)) {
    ++RedisHits;
    if (ResultStr != "") {
      // Parse Result
      llvm::SmallVector<llvm::StringRef, 8> VResult;
      llvm::StringRef R = ResultStr;
      R.split(VResult, ",");

      // Set IsSat
      Result = false;
      if (VResult[0].trim() == "1") Result = true;

      // Add models
      for (int i = 1; i < VResult.size(); i++) {
        auto RSplit = VResult[i].split(':');
        llvm::APInt I(std::stoi(RSplit.first.str()), RSplit.second, 10);
        Models->push_back(I);
      }

      // Update internal cache
      SaturnSC.insertSolverQuery(Query, Result, Models, std::error_code(),
                                  false);

      return true;
    }
  }

  return false;
}

bool SolverCache::hGet(llvm::StringRef Key, llvm::StringRef Field,
                       std::string &Value) {
  std::lock_guard<std::mutex> guard(RedisMutex);

  redisReply *reply =
      (redisReply *)redisCommand(Ctx, "HGET %s %s", Key.data(), Field.data());
  if (!reply || Ctx->err) {
    llvm::report_fatal_error((llvm::StringRef) "[SolverCache] Redis error: " +
                             Ctx->errstr);
  }
  if (reply->type == REDIS_REPLY_NIL) {
    freeReplyObject(reply);
    return false;
  } else if (reply->type == REDIS_REPLY_STRING) {
    Value = reply->str;
    freeReplyObject(reply);
    return true;
  } else {
    llvm::outs()
        << (("[SolverCache] Redis protocol error for cache lookup, didn't "
             "expect "
             "reply type " +
             std::to_string(reply->type))
                .c_str());
    return false;
  }
}

void SolverCache::hSet(llvm::StringRef Key, llvm::StringRef Field,
                       llvm::StringRef Value) {
  std::lock_guard<std::mutex> guard(RedisMutex);

  redisReply *reply = (redisReply *)redisCommand(
      Ctx, "HSET %s %s %s", Key.data(), Field.data(), Value.data());

  if (!reply || Ctx->err) {
    llvm::report_fatal_error((llvm::StringRef) "Redis error: " + Ctx->errstr);
  }

  if (reply->type != REDIS_REPLY_INTEGER) {
    llvm::outs()
        << "[SolverCache] Redis protocol error for cache fill, didn't expect "
           "reply type: "
        << std::to_string(reply->type) << "\n";
  }

  freeReplyObject(reply);
}