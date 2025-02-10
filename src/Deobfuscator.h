#include <memory>
#include <string>
#include <vector>

namespace llvm {
class DominatorTree;
class Function;
class Module;
class LLVMContext;
class Evaluator;
class TargetLibraryInfoImpl;
class TargetLibraryInfo;
class Type;
} // namespace llvm

namespace squanchy {

class Deobfuscator {
public:
  Deobfuscator(const std::string &filename, const std::string &OutputFile);

  ~Deobfuscator();

  /*
   * Deobfuscate the input file
   */
  bool deobfuscate();

  /*
   * Parse the input file
   */
  std::unique_ptr<llvm::Module> parse(const std::string &filename);

private:
  llvm::TargetLibraryInfoImpl *TLII;
  std::unique_ptr<llvm::TargetLibraryInfo> TLI;

  std::unique_ptr<llvm::Module> M;
  std::unique_ptr<llvm::Module> RuntimeModule;

  std::string InputFile = "";

  std::string OutputFile = "";

  int InstructionCountBefore = 0;

  int getInstructionCount(llvm::Module *M);
  int getInstructionCount(llvm::Function *F);

  bool deobfuscateFunction(llvm::Function *F);

  bool isWasm2CFunction(llvm::Function *F);

  void linkRuntime();

  void optimizeFunction(llvm::Function *F);
  void optimizeFunctionWithCustomPipeline(llvm::Function *F,
                                          bool SimplifyCFG = true);
  void optimizeModule(llvm::Module *M);

  void inlineFunctions(llvm::Function *F);

  void removeCallASMSideEffects(llvm::Function *F);
  void removeCallASMSideEffects(std::string FunctionName);

  std::vector<std::string> AIFunctionNames;
  void setFunctionAlwayInline(llvm::Function *F);
  void setFunctionAlwayInline(std::string FunctionName);
  void setFunctionsAlwayInline();
  void removeAlwayInlineAttribute();

  void injectInitializer(llvm::Function *F);
  void handle_funcref_table_init(llvm::Function *F);

  void replaceCallocs(llvm::Function *F);
  void replaceInstanceRefs(llvm::Function *F);
  void replaceFUNCREF_TABLE(llvm::Function *F);

  void overrideTarget(llvm::Module *M);

  void writeOutput();
};

} // namespace squanchy