#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <string>
#include <tuple>

#ifndef _WIN32
#include <unistd.h>

#define DEBUG_WAIT()                                                                               \
  do {                                                                                             \
    errs() << getpid() << "\n";                                                                    \
    sleep(10);                                                                                     \
  } while (0)
#endif

using namespace llvm;

namespace {
#ifndef TARGET_CLONE_ANNOTATION
const StringRef TARGET_CLONE = "targetclone";
#else
const StringRef TARGET_CLONE = TARGET_CLONE_ANNOTATION;
#endif

enum class Vendor : unsigned {
#define X86_VENDOR(ENUM, STRING) ENUM,
#include <llvm/Support/X86TargetParser.def>
};

enum class CPU : unsigned {
#define X86_CPU_TYPE(ARCHNAME, ENUM) ENUM,
#include <llvm/Support/X86TargetParser.def>
};

enum class TargetID : unsigned {
  DUMMY, // Enum Value Cannot be 0. Required by `emitX86CpuIs`.
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR) ENUM,
#include <llvm/Support/X86TargetParser.def>
};

struct Target {
  TargetID ID;
  StringRef Name;
};

const Target TARGETS[] = {
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR) Target{TargetID::ENUM, STR},
#include <llvm/Support/X86TargetParser.def>
};

const char CPU_MODEL_BC[] = {
#include "cpumodel.bc.h"
};

constexpr auto NUM_TARGETS = sizeof(CPU_MODEL_BC) / sizeof(CPU_MODEL_BC[0]);
const StringRef TARGET_CPU = "target-cpu";
const StringRef TARGET_FEATURES = "target-features";

struct TargetFunctionPair {
  std::reference_wrapper<const Target> Targt;
  Function *Func;

  friend bool operator>(const TargetFunctionPair &TF1, const TargetFunctionPair &TF2) noexcept {
    return TF1.Targt.get().ID > TF2.Targt.get().ID;
  }
};

constexpr auto needsVersioning(TargetID ID) -> bool {
  switch (ID) {
  case TargetID::INTEL_COREI7_HASWELL:
  case TargetID::INTEL_COREI7_SKYLAKE_AVX512:
    return true;

  case TargetID::DUMMY:
  case TargetID::INTEL_COREI7_NEHALEM:
  case TargetID::INTEL_COREI7_WESTMERE:
  case TargetID::INTEL_COREI7_SANDYBRIDGE:
  case TargetID::AMDFAM10H_BARCELONA:
  case TargetID::AMDFAM10H_SHANGHAI:
  case TargetID::AMDFAM10H_ISTANBUL:
  case TargetID::AMDFAM15H_BDVER1:
  case TargetID::AMDFAM15H_BDVER2:
  case TargetID::AMDFAM15H_BDVER3:
  case TargetID::AMDFAM15H_BDVER4:
  case TargetID::AMDFAM17H_ZNVER1:
  case TargetID::INTEL_COREI7_IVYBRIDGE:
  case TargetID::INTEL_COREI7_BROADWELL:
  case TargetID::INTEL_COREI7_SKYLAKE:
  case TargetID::INTEL_COREI7_CANNONLAKE:
  case TargetID::INTEL_COREI7_ICELAKE_CLIENT:
  case TargetID::INTEL_COREI7_ICELAKE_SERVER:
    return false;
  }

  llvm_unreachable("cannot reach here");
}

#ifndef NDEBUG
LLVM_ATTRIBUTE_UNUSED
void Dump(const Value &V) { V.print(outs()); }
LLVM_ATTRIBUTE_UNUSED
void Dump(const Module &M) { M.print(outs(), nullptr); }
#endif

class TargetClone : public ModulePass {
public:
  static char ID;

  TargetClone() : ModulePass(ID) {}

  auto runOnModule(Module &M) -> bool override {
    auto ClonableFunctions = getClonableFunctions(M);

    if (ClonableFunctions.empty())
      return false;

    injectCpuModelUtils(M);
    for (auto *F : ClonableFunctions) {
      SmallVector<TargetFunctionPair, NUM_TARGETS> TargetFunctions;

      for (const auto &Target : TARGETS) {
        if (needsVersioning(Target.ID)) {
          auto &Cloned = cloneFunction(*F, Target.Name);
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
    return true;
  }

private:
  static void dispatchTargetFunctions(Function &F,
                                      SmallVectorImpl<TargetFunctionPair> &TargetFunctions,
                                      Function &Default) {
    for (auto BB = F.begin(); BB != F.end();) {
      BB = BB->eraseFromParent();
    }

    auto &C = F.getContext();
    IRBuilder<> Builder(C);
    auto *BB = BasicBlock::Create(C, "entry", &F);
    Builder.SetInsertPoint(BB);

    // Sort TargetFunctions by reverse order based on Target
    std::sort(TargetFunctions.begin(), TargetFunctions.end(), std::greater<>());
    emitMultiVersionResolver(F, Builder, TargetFunctions, Default);
  }

  static void addTargetAttribute(Function &F, const Target &T) {
    auto Attrs = F.getAttributes().getFnAttributes();
    auto &C = F.getContext();
    Attrs = Attrs.removeAttribute(C, TARGET_CPU).addAttribute(C, TARGET_CPU, T.Name);
    F.setAttributes(F.getAttributes().addAttributes(C, AttributeList::FunctionIndex, Attrs));

    SmallSet<StringRef, 32> Features;

    // Add features present already
    SmallVector<StringRef, 32> FeaturesVec;
    Attrs.getAttribute(TARGET_FEATURES).getValueAsString().split(FeaturesVec, ",", -1, false);
    Features.insert(FeaturesVec.begin(), FeaturesVec.end());

    // Add features from Target
    FeaturesVec.clear();
    getCPUSpecificFeatures(T.Name, FeaturesVec);
    Features.insert(FeaturesVec.begin(), FeaturesVec.end());

    // Concat all feature strings and add to Attributes
    SmallString<512> TargetFeatures;
    for (const auto &F : Features) {
      TargetFeatures += F;
      TargetFeatures += ",";
    }
    // Pop trailing ','
    TargetFeatures.pop_back();

    Attrs =
        Attrs.removeAttribute(C, TARGET_FEATURES).addAttribute(C, TARGET_FEATURES, TargetFeatures);
    F.setAttributes(F.getAttributes().addAttributes(C, AttributeList::FunctionIndex, Attrs));
  }

  static auto cloneFunction(Function &Src, StringRef Suffix) -> Function & {
    auto *M = Src.getParent();
    SmallString<128> Name = Src.getName();
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
      for (unsigned i = 0; i < AnnoArray->getNumOperands(); i++) {
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

  static void emitMultiVersionResolver(Function &F, IRBuilder<> &Builder,
                                       ArrayRef<TargetFunctionPair> TargetFunctions,
                                       Function &Default) {
    SmallVector<Value *, 10> Args;
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
      auto &Cond = emitX86IsCpuCompat(*F.getParent(), Builder, TargetFunction.Targt.get().Name);
      auto *Call =
          BasicBlock::Create(F.getContext(), (TargetFunction.Targt.get().Name) + ".call", &F);
      auto *Cont = BasicBlock::Create(F.getContext(), "cont", &F);

      Builder.CreateCondBr(&Cond, Call, Cont);

      Builder.SetInsertPoint(Call);
      CallVariant(TargetFunction.Func);

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
    V.setDLLStorageClass(GlobalValue::DefaultStorageClass);

    return V;
  }

  static auto getCpuModel(Module &M) -> GlobalVariable & {
    return *M.getGlobalVariable("target_clone_cpu_model", true);
  }

  static auto getCpuFeatures2(Module &M) -> GlobalVariable & {
    return *M.getGlobalVariable("target_clone_cpu_features2", true);
  }

  static void getCPUSpecificFeatures(StringRef Name, SmallVectorImpl<StringRef> &Features) {
    StringRef WholeList = StringSwitch<StringRef>(cpuSpecificNameDealias(Name))
#define CPU_SPECIFIC(NAME, MANGLING, FEATURES) .Case(NAME, FEATURES)
#include "X86Target.def"
                              .Default("");
    WholeList.split(Features, ',', /*MaxSplit=*/-1, /*KeepEmpty=*/false);
  }

  static auto cpuSpecificNameDealias(StringRef Name) -> StringRef {
    return StringSwitch<StringRef>(Name)
#define CPU_SPECIFIC_ALIAS(NEW_NAME, NAME) .Case(NEW_NAME, NAME)
#include "X86Target.def"
        .Default(Name);
  }

  static auto emitX86IsCpuCompat(Module &M, IRBuilder<> &Builder, StringRef CPUStr) -> Value & {
    // Calculate the index needed to access the correct field based on the
    // range. Also adjust the expected value.
    unsigned Index;
    unsigned Value;
    std::tie(Index, Value) = StringSwitch<std::pair<unsigned, unsigned>>(CPUStr)

#define X86_VENDOR(ENUM, STRING) .Case(STRING, {0u, static_cast<unsigned>(Vendor::ENUM)})
#define X86_CPU_TYPE_COMPAT_WITH_ALIAS(ARCHNAME, ENUM, STR, ALIAS)                                 \
  .Cases(STR, ALIAS, {1u, static_cast<unsigned>(CPU::ENUM)})
#define X86_CPU_TYPE_COMPAT(ARCHNAME, ENUM, STR) .Case(STR, {1u, static_cast<unsigned>(CPU::ENUM)})
#define X86_CPU_SUBTYPE_COMPAT(ARCHNAME, ENUM, STR)                                                \
  .Case(STR, {2u, static_cast<unsigned>(TargetID::ENUM)})
#include <llvm/Support/X86TargetParser.def>
                                 .Default({0, 0});
    assert(Value != 0 && "Invalid CPUStr passed to IsCpuCompat");

    // Grab the appropriate field from __cpu_model.
    auto &CpuModel = getCpuModel(M);
    auto *Int32Ty = Builder.getInt32Ty();
    std::array<llvm::Value *, 2> Idxs = {ConstantInt::get(Int32Ty, 0),
                                         ConstantInt::get(Int32Ty, Index)};
    llvm::Value *CpuValue =
        Builder.CreateGEP(CpuModel.getType()->getPointerElementType(), &CpuModel, Idxs);
    CpuValue = Builder.CreateAlignedLoad(CpuValue, 4);

    // Check the value of the field against the requested value.
    return *Builder.CreateICmpUGE(CpuValue, ConstantInt::get(Int32Ty, Value), CPUStr + ".check");
  }

  static void injectCpuModelUtils(Module &M) {
    SMDiagnostic error;
    auto Buffer = MemoryBuffer::getMemBuffer({CPU_MODEL_BC, sizeof(CPU_MODEL_BC)});
    auto CpuModelModule = parseIR(*Buffer, error, M.getContext());
    if (!CpuModelModule) {
      error.print("cpumodel.bc", errs());
      std::terminate();
    }
    Linker::linkModules(M, std::move(CpuModelModule));
  }
};

char TargetClone::ID = 0;
} // namespace

static RegisterPass<TargetClone> X("target-clone", "Target Clone Pass");

static RegisterStandardPasses Y(PassManagerBuilder::EP_ModuleOptimizerEarly,
                                [](const PassManagerBuilder & /*Builder*/,
                                   legacy::PassManagerBase &PM) { PM.add(new TargetClone()); });
