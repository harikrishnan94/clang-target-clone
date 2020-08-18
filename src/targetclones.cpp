#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/IRBuilder.h>
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

void Dump(Value &V) { V.print(outs()); }

#define DEBUG_WAIT()                                                                               \
  do {                                                                                             \
    errs() << getpid() << "\n";                                                                    \
    sleep(10);                                                                                     \
  } while (0)

namespace {
#ifndef TARGET_CLONE_ANNOTATION
const char *TARGET_CLONE = "targetclone";
#else
const char *TARGET_CLONE = TARGET_CLONE_ANNOTATION;
#endif

using FunctionSet = SmallPtrSet<Function *, 4>;

enum class TargetFeatureKind { SSE4_2 = 0, AVX, AVX2, AVX512F, LAST };

const char *TARGET_FEATURES = "target-features";
constexpr auto TYPICAL_FEATURE_COUNT = 20;
constexpr auto TYPICAL_FEATURES_LEN = 512;
constexpr auto TEMP_STR_LEN = 128;

struct TargetFeature {
  StringRef Name;
  std::array<StringRef, TYPICAL_FEATURE_COUNT> Value;
};

const std::array
    // NOLINTNEXTLINE - Maintain this in sorted in reverse order.
    KNOWN_FEATURES = {
        TargetFeature{"avx512f",
                      {"+avx", "+avx2", "+avx512f", "+f16c", "+fma", "+fxsr", "+mmx", "+popcnt",
                       "+sse", "+sse2", "+sse3", "+sse4.1", "+sse4.2", "+ssse3", "+x87", "+xsave"}},
        TargetFeature{"avx2",
                      {"+avx", "+avx2", "+fxsr", "+mmx", "+popcnt", "+sse", "+sse2", "+sse3",
                       "+sse4.1", "+sse4.2", "+ssse3", "+x87", "+xsave"}},
        TargetFeature{"avx",
                      {"+avx", "+fxsr", "+mmx", "+popcnt", "+sse", "+sse2", "+sse3", "+sse4.1",
                       "+sse4.2", "+ssse3", "+x87", "+xsave"}},
        TargetFeature{"sse4.2",
                      {"+fxsr", "+mmx", "+popcnt", "+sse", "+sse2", "+sse3", "+sse4.1", "+sse4.2",
                       "+ssse3", "+x87"}}};
constexpr auto NUM_TARGETS = KNOWN_FEATURES.size();

using TargetFeatureFunctionPair =
    std::pair<std::reference_wrapper<const TargetFeature>, Function *>;

class TargetClone : public ModulePass {
public:
  static inline char ID = 0;

  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    for (auto *F : getClonableFunctions(M)) {
      SmallVector<TargetFeatureFunctionPair, NUM_TARGETS> TargetFunctions;

      for (const auto &Feature : KNOWN_FEATURES) {
        auto &Cloned = cloneFunction(*F, Feature.Name);
        addTargetAttribute(Cloned, Feature);
        TargetFunctions.push_back({Feature, &Cloned});
      }

      auto &Default = cloneFunction(*F, "default");
      dispatchTargetFunctions(*F, TargetFunctions, Default);
    }

#ifndef NDEBUG
    verifyModule(M, &errs());
#endif
    return true;
  }

private:
  static auto createGlobalFunction(Module &M, StringRef Name, FunctionType *FTy) -> Function & {
    auto &F = *cast<Function>(M.getOrInsertFunction(Name, FTy));

    // In Windows Itanium environments, try to mark runtime functions
    // dllimport. For Mingw and MSVC, don't. We don't really know if the user
    // will link their standard library statically or dynamically. Marking
    // functions imported when they are not imported can cause linker errors
    // and warnings.
#ifdef _WIN32
    F.setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    F.setLinkage(GlobalValue::ExternalLinkage);
#endif

    F.setDSOLocal(true);
    F.setLinkage(Function::ExternalLinkage);
    F.setDLLStorageClass(GlobalValue::DefaultStorageClass);

    return F;
  }

  static auto createGlobalVariable(Module &M, StringRef Name, Type *T) -> GlobalVariable & {
    auto &V = *cast<GlobalVariable>(M.getOrInsertGlobal(Name, T));

    // In Windows Itanium environments, try to mark runtime functions
    // dllimport. For Mingw and MSVC, don't. We don't really know if the user
    // will link their standard library statically or dynamically. Marking
    // functions imported when they are not imported can cause linker errors
    // and warnings.
#ifdef _WIN32
    V.setDLLStorageClass(GlobalValue::DLLImportStorageClass);
    V.setLinkage(GlobalValue::ExternalLinkage);
#endif

    V.setDSOLocal(true);
    V.setLinkage(Function::ExternalLinkage);

    return V;
  }

  static void emitX86CpuInit(Module &M, IRBuilder<> &Builder) {
    auto *FTy = FunctionType::get(Builder.getVoidTy(), /*Variadic*/ false);
    auto &Func = createGlobalFunction(M, "__cpu_indicator_init", FTy);
    Builder.CreateCall(&Func);
  }

  static auto getCpuModel(Module &M, IRBuilder<> &B) -> Constant & {
    auto &Int32Ty = *B.getInt32Ty();

    // Matching the struct layout from the compiler-rt/libgcc structure that is
    // filled in:
    // unsigned int __cpu_vendor;
    // unsigned int __cpu_type;
    // unsigned int __cpu_subtype;
    // unsigned int __cpu_features[1];
    auto &STy = *StructType::get(&Int32Ty, &Int32Ty, &Int32Ty, ArrayType::get(&Int32Ty, 1));

    // Grab the global __cpu_model.
    auto &CpuModel = createGlobalVariable(M, "__cpu_model", &STy);
    return *cast<Constant>(&CpuModel);
  }

  static auto getCpuFeatures2(Module &M, IRBuilder<> &B) -> Constant & {
    auto &Int32Ty = *B.getInt32Ty();
    // Grab the global __cpu_features2.
    auto &CpuFeatures2 = createGlobalVariable(M, "__cpu_features2", &Int32Ty);
    return *cast<Constant>(&CpuFeatures2);
  }

  static auto getX86CpuSupportsMask(StringRef FeatureStr) -> uint64_t {
    // Processor features and mapping to processor feature value.
    unsigned Feature = StringSwitch<unsigned>(FeatureStr)
    // NOLINTNEXTLINE
#define X86_FEATURE_COMPAT(VAL, ENUM, STR) .Case(STR, VAL)
#include <llvm/Support/X86TargetParser.def>
        ;
    return (1ULL << Feature);
  }

  static auto emitX86CpuSupportsMask(Module &M, IRBuilder<> &Builder, uint64_t FeaturesMask)
      -> Value & {
    uint32_t Features1 = Lo_32(FeaturesMask);
    uint32_t Features2 = Hi_32(FeaturesMask);

    auto *Int32Ty = Builder.getInt32Ty();
    Value *BitsetLo = Builder.getInt32(0);
    Value *BitsetHi = Builder.getInt32(0);

    if (Features1 != 0) {
      auto &CpuModel = getCpuModel(M, Builder);

      // Grab the first (0th) element from the field __cpu_features off of the
      // global in the struct STy.
      std::array<Value *, 3> Idxs = {Builder.getInt32(0), Builder.getInt32(3), Builder.getInt32(0)};
      Value *CpuFeatures =
          Builder.CreateGEP(CpuModel.getType()->getPointerElementType(), &CpuModel, Idxs);
      auto *Features = Builder.CreateAlignedLoad(CpuFeatures, 4);

      // Check the value of the bit corresponding to the feature requested.
      auto *Mask = Builder.getInt32(Features1);
      BitsetLo = Builder.CreateAnd(Features, Mask);
    }

    if (Features2 != 0) {
      auto &CpuFeatures2 = getCpuFeatures2(M, Builder);
      auto *Features = Builder.CreateAlignedLoad(&CpuFeatures2, 4);

      // Check the value of the bit corresponding to the feature requested.
      auto *Mask = Builder.getInt32(Features2);
      BitsetHi = Builder.CreateAnd(Features, Mask);
    }

    constexpr auto INT32_BITS = 32;
    BitsetHi = Builder.CreateZExt(BitsetHi, Builder.getInt64Ty());
    BitsetHi = Builder.CreateShl(BitsetHi, Builder.getInt32(INT32_BITS));
    BitsetLo = Builder.CreateZExt(BitsetLo, Builder.getInt64Ty());

    return *Builder.CreateOr(BitsetHi, BitsetLo);
  }

  static void emitMultiVersionResolver(Function &F, IRBuilder<> &Builder,
                                       ArrayRef<TargetFeatureFunctionPair> TargetFunctions,
                                       Function &Default) {
    emitX86CpuInit(*F.getParent(), Builder);
    uint64_t FeaturesMask = 0;
    SmallVector<uint64_t, NUM_TARGETS> FeatureMasks;
    for (const auto &P : TargetFunctions) {
      auto Mask = getX86CpuSupportsMask(P.first.get().Name);
      FeaturesMask |= Mask;
      FeatureMasks.push_back(Mask);
    }
    auto &FeaturesMaskRT = emitX86CpuSupportsMask(*F.getParent(), Builder, FeaturesMask);

    SmallVector<Value *, 10> Args; // NOLINT
    for (auto &&Arg : F.args()) {
      Args.push_back(&cast<Value>(Arg));
    }

    auto CallVariant = [&](Function *Func) {
      if (F.getReturnType()->isVoidTy()) {
        Builder.CreateCall(Func, Args);
        Builder.CreateRetVoid();
      } else {
        auto *Ret = Builder.CreateCall(Func, Args);
        Ret->setTailCallKind(CallInst::TCK_Tail);
        Builder.CreateRet(Ret);
      }
    };

    for (auto [FeatureMask, TargetFunction] : zip(FeatureMasks, TargetFunctions)) {
      auto *Cond = Builder.CreateAnd(&FeaturesMaskRT, Builder.getInt64(FeatureMask));
      auto *Call =
          BasicBlock::Create(F.getContext(), TargetFunction.first.get().Name + ".call", &F);
      auto *Cont = BasicBlock::Create(F.getContext(), "cont", &F);

      Builder.CreateCondBr(Builder.CreateICmpNE(Cond, Builder.getInt64(0)), Call, Cont);

      Builder.SetInsertPoint(Call);
      CallVariant(TargetFunction.second);

      Builder.SetInsertPoint(Cont);
    }

    CallVariant(&Default);
  }

  static void dispatchTargetFunctions(Function &F,
                                      ArrayRef<TargetFeatureFunctionPair> TargetFunctions,
                                      Function &Default) {
    for (auto BB = F.begin(); BB != F.end();) {
      BB = BB->eraseFromParent();
    }

    auto &C = F.getContext();
    IRBuilder<> Builder(C);
    auto *BB = BasicBlock::Create(C, "entry", &F);
    Builder.SetInsertPoint(BB);

    emitMultiVersionResolver(F, Builder, TargetFunctions, Default);
  }

  static void addTargetAttribute(Function &F, const TargetFeature &TF) {
    if (TF.Name == "default")
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
    Dst.setLinkage(GlobalValue::InternalLinkage);
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

  static void emitTrapCall(Module &M, IRBuilder<> &Builder) {
    Builder.CreateCall(Intrinsic::getDeclaration(&M, Intrinsic::trap, None));
  }

}; // end of struct Hello
} // end of anonymous namespace

// NOLINTNEXTLINE
static RegisterPass<TargetClone> X("target-clone", "Target Clone Pass");

// NOLINTNEXTLINE
static RegisterStandardPasses Y(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                [](const PassManagerBuilder & /*Builder*/,
                                   legacy::PassManagerBase &PM) {
                                  PM.add(new TargetClone()); // NOLINT
                                });
