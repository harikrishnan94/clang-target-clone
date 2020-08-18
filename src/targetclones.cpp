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
#include <string_view>
#include <tuple>
#include <vector>

#include <unistd.h>

using namespace llvm;

#define DEBUG_WAIT()                                                                               \
  do {                                                                                             \
    errs() << getpid() << "\n";                                                                    \
    sleep(10);                                                                                     \
  } while (0)

namespace {
inline auto to_strref(std::string_view str) -> StringRef { return {str.data(), str.length()}; }

#ifndef TARGET_CLONE_ANNOTATION
constexpr std::string_view TARGET_CLONE = "targetclone";
#else
constexpr std::string_view TARGET_CLONE = TARGET_CLONE_ANNOTATION;
#endif

enum class Vendor : unsigned {
// NOLINTNEXTLINE
#define X86_VENDOR(ENUM, STRING) ENUM,
#include <llvm/Support/X86TargetParser.def>
};

enum class CPU : unsigned {
// NOLINTNEXTLINE
#define X86_CPU_TYPE(ARCHNAME, ENUM) ENUM,
#include <llvm/Support/X86TargetParser.def>
};

enum class TargetID : unsigned {
  DUMMY, // Enum Value Cannot be 0. Required by `emitX86CpuIs`.
// NOLINTNEXTLINE
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR) ENUM,
#include <llvm/Support/X86TargetParser.def>
};
struct Target {
  TargetID ID;
  std::string_view Name;
};

std::array TARGETS = {
// NOLINTNEXTLINE
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR) Target{TargetID::ENUM, STR},
#include <llvm/Support/X86TargetParser.def>
};

constexpr auto NUM_TARGETS = TARGETS.size();
constexpr std::string_view TARGET_CPU = "target-cpu";
constexpr std::string_view TARGET_FEATURES = "target-features";
constexpr auto TYPICAL_FEATURE_COUNT = 32;
constexpr auto TYPICAL_FEATURES_LEN = 512;
constexpr auto TEMP_STR_LEN = 128;

using TargetFunctionPair = std::pair<std::reference_wrapper<const Target>, Function *>;

constexpr auto is64Bit(TargetID ID) -> bool {
  switch (ID) {
  case TargetID::AMDFAM10H_BARCELONA:
  case TargetID::AMDFAM10H_SHANGHAI:
  case TargetID::AMDFAM10H_ISTANBUL:
    return false;
  default:
    return true;
  }
}

#ifndef NDEBUG
void Dump(Value &V) { V.print(outs()); }
void Dump(Module &M) { M.print(outs(), nullptr); }
#endif

class TargetClone : public ModulePass {
public:
  static inline char ID = 0;

  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    auto ClonableFunctions = getClonableFunctions(M);
    for (auto *F : ClonableFunctions) {
      SmallVector<TargetFunctionPair, NUM_TARGETS> TargetFunctions;

      for (const auto &Target : TARGETS) {
        if (is64Bit(Target.ID)) {
          auto &Cloned = cloneFunction(*F, to_strref(Target.Name));
          addTargetAttribute(Cloned, Target);
          TargetFunctions.push_back({Target, &Cloned});
        }
      }

      auto &Default = cloneFunction(*F, "default");
      dispatchTargetFunctions(*F, TargetFunctions, Default);
    }

#ifndef NDEBUG
    verifyModule(M, &errs());
#endif
    return !ClonableFunctions.empty();
  }

private:
  static void dispatchTargetFunctions(Function &F, ArrayRef<TargetFunctionPair> TargetFunctions,
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

  static void addTargetAttribute(Function &F, const Target &T) {
    auto Attrs = F.getAttributes().getFnAttributes();
    auto &C = F.getContext();
    Attrs = Attrs.removeAttribute(C, to_strref(TARGET_CPU))
                .addAttribute(C, to_strref(TARGET_CPU), to_strref(T.Name));
    F.setAttributes(F.getAttributes().addAttributes(C, AttributeList::FunctionIndex, Attrs));

    SmallSet<StringRef, TYPICAL_FEATURE_COUNT> Features;

    // Add features present already
    SmallVector<StringRef, TYPICAL_FEATURE_COUNT> FeaturesVec;
    Attrs.getAttribute(to_strref(TARGET_FEATURES))
        .getValueAsString()
        .split(FeaturesVec, ",", -1, false);
    Features.insert(FeaturesVec.begin(), FeaturesVec.end());

    // Add features from Target
    FeaturesVec.clear();
    getCPUSpecificFeatures(T.Name, FeaturesVec);
    Features.insert(FeaturesVec.begin(), FeaturesVec.end());

    // Concat all feature strings and add to Attributes
    SmallString<TYPICAL_FEATURES_LEN> TargetFeatures;
    for (const auto &F : Features) {
      TargetFeatures += F;
      TargetFeatures += ",";
    }
    // Pop trailing ','
    TargetFeatures.pop_back();

    Attrs = Attrs.removeAttribute(C, to_strref(TARGET_FEATURES))
                .addAttribute(C, to_strref(TARGET_FEATURES), TargetFeatures);
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
  using FunctionSet = SmallPtrSet<Function *, 4>;
  static auto getClonableFunctions(const Module &M) -> FunctionSet {
    FunctionSet Functions;
    if (const auto *global_annos = M.getNamedGlobal("llvm.global.annotations")) {
      auto *AnnoArray = cast<ConstantArray>(global_annos->getOperand(0));
      for (int i = 0; i < AnnoArray->getNumOperands(); i++) {
        auto *e = cast<ConstantStruct>(AnnoArray->getOperand(i));

        if (auto *F = dyn_cast<Function>(e->getOperand(0)->getOperand(0))) {
          auto *Anno = cast<ConstantDataArray>(
              cast<GlobalVariable>(e->getOperand(1)->getOperand(0))->getOperand(0));
          if (Anno->getRawDataValues().startswith(to_strref(TARGET_CLONE)))
            Functions.insert(F);
        }
      }
    }

    return Functions;
  }

  static void emitMultiVersionResolver(Function &F, IRBuilder<> &Builder,
                                       ArrayRef<TargetFunctionPair> TargetFunctions,
                                       Function &Default) {
    emitX86CpuInit(*F.getParent(), Builder);
    uint64_t FeaturesMask = 0;

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

    for (auto &&TargetFunction : TargetFunctions) {
      auto &Cond = emitX86CpuIs(*F.getParent(), Builder, TargetFunction.first.get().Name);
      auto *Call = BasicBlock::Create(F.getContext(),
                                      to_strref(TargetFunction.first.get().Name) + ".call", &F);
      auto *Cont = BasicBlock::Create(F.getContext(), "cont", &F);

      Builder.CreateCondBr(&Cond, Call, Cont);

      Builder.SetInsertPoint(Call);
      CallVariant(TargetFunction.second);

      Builder.SetInsertPoint(Cont);
    }

    CallVariant(&Default);
  }

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

  static void getCPUSpecificFeatures(std::string_view Name, SmallVectorImpl<StringRef> &Features) {
    StringRef WholeList = StringSwitch<StringRef>(cpuSpecificNameDealias(Name))
    // NOLINTNEXTLINE
#define CPU_SPECIFIC(NAME, MANGLING, FEATURES) .Case(NAME, FEATURES)
#include <clang/Basic/X86Target.def>
                              .Default("");
    WholeList.split(Features, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  }

  static auto cpuSpecificNameDealias(std::string_view Name) -> StringRef {
    return StringSwitch<StringRef>(to_strref(Name))
    // NOLINTNEXTLINE
#define CPU_SPECIFIC_ALIAS(NEW_NAME, NAME) .Case(NEW_NAME, NAME)
#include <clang/Basic/X86Target.def>
        .Default(to_strref(Name));
  }

  static auto emitX86CpuIs(Module &M, IRBuilder<> &Builder, std::string_view CPUStr) -> Value & {
    // Calculate the index needed to access the correct field based on the
    // range. Also adjust the expected value.
    unsigned Index;
    unsigned Value;
    std::tie(Index, Value) = StringSwitch<std::pair<unsigned, unsigned>>(to_strref(CPUStr))

// NOLINTNEXTLINE
#define X86_VENDOR(ENUM, STRING) .Case(STRING, {0u, static_cast<unsigned>(Vendor::ENUM)})
// NOLINTNEXTLINE
#define X86_CPU_TYPE_COMPAT_WITH_ALIAS(ARCHNAME, ENUM, STR, ALIAS)                                 \
  .Cases(STR, ALIAS, {1u, static_cast<unsigned>(CPU::ENUM)})
// NOLINTNEXTLINE
#define X86_CPU_TYPE_COMPAT(ARCHNAME, ENUM, STR) .Case(STR, {1u, static_cast<unsigned>(CPU::ENUM)})
// NOLINTNEXTLINE
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR)                                                \
  .Case(STR, {2u, static_cast<unsigned>(TargetID::ENUM)})
#include <llvm/Support/X86TargetParser.def>
                                 .Default({0, 0});
    assert(Value != 0 && "Invalid CPUStr passed to CpuIs");

    // Grab the appropriate field from __cpu_model.
    auto &CpuModel = getCpuModel(M, Builder);
    auto *Int32Ty = Builder.getInt32Ty();
    std::array<llvm::Value *, 2> Idxs = {ConstantInt::get(Int32Ty, 0),
                                         ConstantInt::get(Int32Ty, Index)};
    llvm::Value *CpuValue =
        Builder.CreateGEP(CpuModel.getType()->getPointerElementType(), &CpuModel, Idxs);
    CpuValue = Builder.CreateAlignedLoad(CpuValue, 4);

    // Check the value of the field against the requested value.
    return *Builder.CreateICmpEQ(CpuValue, ConstantInt::get(Int32Ty, Value));
  }
};
} // namespace

// NOLINTNEXTLINE
static RegisterPass<TargetClone> X("target-clone", "Target Clone Pass");

// NOLINTNEXTLINE
static RegisterStandardPasses Y(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                [](const PassManagerBuilder & /*Builder*/,
                                   legacy::PassManagerBase &PM) {
                                  PM.add(new TargetClone()); // NOLINT
                                });
