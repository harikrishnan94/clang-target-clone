#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>

#include <string>
#include <vector>

using namespace llvm;

namespace {
class TargetClone : public ModulePass {
public:
  static inline char ID = 0;
  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    std::vector<Function *> Functions;
    for (auto &F : M) {
      Functions.push_back(&F);
    }
    for (auto *F : Functions) {
      cloneFunction(*F);
    }
    return true;
  }

private:
  static auto cloneFunction(Function &Src) -> Function & {
    auto *M = Src.getParent();
    auto Name = Src.getName().str() + "_clone";
    auto *Type = Src.getFunctionType();
    auto Attrs = Src.getAttributes();
    auto &Dst = *cast<Function>(M->getOrInsertFunction(Name, Type, Attrs));

    SmallVector<ReturnInst *, 4> Returns;
    ValueToValueMapTy VMap;

    for (auto SrcArg = Src.arg_begin(), DstArg = Dst.arg_begin(); SrcArg != Src.arg_end();
         ++SrcArg, ++DstArg) {
      VMap[&*SrcArg] = DstArg;
    }

    CloneFunctionInto(&Dst, &Src, VMap, true, Returns);

    return Dst;
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
