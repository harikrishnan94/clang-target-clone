#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

using namespace llvm;

namespace {
struct TargetClone : public ModulePass {
  static inline char ID = 0;
  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    errs() << "Hello: ";
    errs().write_escaped(M.getName()) << '\n';
    return false;
  }
}; // end of struct Hello
} // end of anonymous namespace

// NOLINTNEXTLINE
static RegisterPass<TargetClone> X("target-clone", "Target Clone Pass");

// NOLINTNEXTLINE
static RegisterStandardPasses Y(PassManagerBuilder::EP_ModuleOptimizerEarly, // NOLINTNEXTLINE
                                [](const PassManagerBuilder &Builder, legacy::PassManagerBase &PM) {
                                  PM.add(new TargetClone()); // NOLINT
                                });
