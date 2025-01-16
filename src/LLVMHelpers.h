namespace llvm {
class Function;
class Module;
} // namespace llvm

namespace Squanchy {

void MoveFunctionIntoModule(llvm::Function *func, llvm::Module *dest_module);

}; // namespace Squanchy