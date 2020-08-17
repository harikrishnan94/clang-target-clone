#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <array>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

using namespace llvm;

namespace {
#ifndef TARGET_CLONE_ANNOTATION
const char *TARGET_CLONE = "targetclone";
#else
const char *TARGET_CLONE = TARGET_CLONE_ANNOTATION;
#endif

using FunctionSet = SmallPtrSet<Function *, 4>;

enum class TargetFeatureKind { NONE = 0, SSE4_2, AVX, AVX2, AVX512F, LAST };

constexpr auto NUM_TARGETS = static_cast<std::size_t>(TargetFeatureKind::LAST);
const char *TARGET_FEATURES = "target-features";
constexpr auto TYPICAL_FEATURE_COUNT = 20;
constexpr auto TYPICAL_FEATURES_LEN = 512;
constexpr auto TEMP_STR_LEN = 128;

struct TargetFeature {
  TargetFeatureKind Kind;
  StringRef Name;
  std::array<StringRef, TYPICAL_FEATURE_COUNT> Value;
};

const std::array<TargetFeature, NUM_TARGETS>
    // NOLINTNEXTLINE
    KNOWN_FEATURES = {TargetFeature{TargetFeatureKind::NONE, "default"},
                      TargetFeature{TargetFeatureKind::SSE4_2,
                                    "sse4.2",
                                    {"+fxsr", "+mmx", "+popcnt", "+sse", "+sse2", "+sse3",
                                     "+sse4.1", "+sse4.2", "+ssse3", "+x87"}},
                      TargetFeature{TargetFeatureKind::AVX,
                                    "avx",
                                    {"+avx", "+fxsr", "+mmx", "+popcnt", "+sse", "+sse2", "+sse3",
                                     "+sse4.1", "+sse4.2", "+ssse3", "+x87", "+xsave"}},
                      TargetFeature{TargetFeatureKind::AVX2,
                                    "avx2",
                                    {"+avx", "+avx2", "+fxsr", "+mmx", "+popcnt", "+sse", "+sse2",
                                     "+sse3", "+sse4.1", "+sse4.2", "+ssse3", "+x87", "+xsave"}},
                      TargetFeature{TargetFeatureKind::AVX512F,
                                    "avx512f",
                                    {"+avx", "+avx2", "+avx512f", "+f16c", "+fma", "+fxsr", "+mmx",
                                     "+popcnt", "+sse", "+sse2", "+sse3", "+sse4.1", "+sse4.2",
                                     "+ssse3", "+x87", "+xsave"}}};

using TargetFunctionPair = std::pair<std::reference_wrapper<const TargetFeature>, Function *>;

class TargetClone : public ModulePass {
public:
  static inline char ID = 0;

  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    for (auto *F : getClonableFunctions(M)) {
      SmallVector<TargetFunctionPair, NUM_TARGETS> TargetFunctions;

      for (const auto &Feature : KNOWN_FEATURES) {
        auto &Cloned = cloneFunction(*F, Feature.Name);
        addTargetAttribute(Cloned, Feature);
        TargetFunctions.push_back({Feature, &Cloned});
      }

      dispatchTargetFunctions(*F, TargetFunctions);
    }

    verifyModule(M, &errs());
    return true;
  }

private:
  static void dispatchTargetFunctions(Function &F, ArrayRef<TargetFunctionPair> TargetFunctions) {}

  static void addTargetAttribute(Function &F, const TargetFeature &TF) {
    assert(TF.Kind != TargetFeatureKind::LAST);
    if (TF.Kind == TargetFeatureKind::NONE)
      return;

    SmallSet<StringRef, TYPICAL_FEATURE_COUNT> Features;

    // Add features present already
    SmallVector<StringRef, TYPICAL_FEATURE_COUNT> ExistingFeatures;
    auto Attrs = F.getAttributes().getFnAttributes();
    Attrs.getAttribute(TARGET_FEATURES).getValueAsString().split(ExistingFeatures, ",", -1, false);
    Features.insert(ExistingFeatures.begin(), ExistingFeatures.end());

    // Add features from Target
    Features.insert(TF.Value.begin(), TF.Value.end());

    // Concat all feature strings and add to Attributes
    SmallString<TYPICAL_FEATURES_LEN> TargetFeatures;
    for (const auto &F : Features) {
      TargetFeatures += F;
      TargetFeatures += ",";
    }
    // Pop trailing ',,'
    TargetFeatures.resize(TargetFeatures.size() - 2);

    auto &C = F.getContext();
    Attrs =
        Attrs.removeAttribute(C, TARGET_FEATURES).addAttribute(C, TARGET_FEATURES, TargetFeatures);
    F.setAttributes(F.getAttributes().addAttributes(C, AttributeList::FunctionIndex, Attrs));
  }

  static auto cloneFunction(Function &Src, StringRef Suffix) -> Function & {
    auto *M = Src.getParent();
    SmallString<TEMP_STR_LEN> Name = Src.getName();
    auto *Type = Src.getFunctionType();
    auto Attrs = Src.getAttributes();
    auto &Dst = *cast<Function>(M->getOrInsertFunction((Name += ".") += Suffix, Type, Attrs));

    SmallVector<ReturnInst *, 4> Returns;
    ValueToValueMapTy VMap;

    for (auto SrcArg = Src.arg_begin(), DstArg = Dst.arg_begin(); SrcArg != Src.arg_end();
         ++SrcArg, ++DstArg) {
      VMap[&*SrcArg] = DstArg;
    }

    CloneFunctionInto(&Dst, &Src, VMap, true, Returns);

    return Dst;
  }

  // Based on http://bholt.org/posts/llvm-quick-tricks.html
  static auto getClonableFunctions(const Module &M) -> FunctionSet {
    FunctionSet Functions;
    if (const auto *global_annos = M.getNamedGlobal("llvm.global.annotations")) {
      auto *AnnoArray = cast<ConstantArray>(global_annos->getOperand(0));
      for (int i = 0; i < AnnoArray->getNumOperands(); i++) {
        auto *e = cast<ConstantStruct>(AnnoArray->getOperand(i));

        if (auto *F = dyn_cast<Function>(e->getOperand(0)->getOperand(0))) {
          auto *Anno = cast<ConstantDataArray>(
              cast<GlobalVariable>(e->getOperand(1)->getOperand(0))->getOperand(0));
          if (Anno->getRawDataValues().startswith(TARGET_CLONE))
            Functions.insert(F);
        }
      }
    }

    return Functions;
  }
}; // end of struct Hello
} // end of anonymous namespace

// NOLINTNEXTLINE
static RegisterPass<TargetClone> X("target-clone", "Target Clone Pass");

// NOLINTNEXTLINE
static RegisterStandardPasses Y(PassManagerBuilder::EP_EnabledOnOptLevel0,
                                [](const PassManagerBuilder & /*Builder*/,
                                   legacy::PassManagerBase &PM) {
                                  PM.add(new TargetClone()); // NOLINT
                                });
