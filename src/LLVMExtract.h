#include <string>
#include <vector>

namespace llvm {
class Module;
}

int LLVMExtract(llvm::Module *M, std::vector<std::string> ExtractFuncs,
                std::vector<std::string> ExtractRegExpGlobals, bool Recursive);