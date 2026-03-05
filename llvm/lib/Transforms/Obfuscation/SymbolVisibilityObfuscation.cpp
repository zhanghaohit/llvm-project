// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/SymbolVisibilityObfuscation.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

#if LLVM_VERSION_MAJOR >= 18
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.starts_with(Prefix);
}
#else
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.startswith(Prefix);
}
#endif

bool shouldKeepVisibility(StringRef Name) {
  return Name == "main" || startsWith(Name, "llvm.") ||
         startsWith(Name, "OBJC_") || startsWith(Name, "_OBJC_") ||
         startsWith(Name, "__objc") || startsWith(Name, "_swift") ||
         startsWith(Name, "swift_");
}

template <typename T> bool updateVisibility(T &V) {
  if (!V.hasName() || shouldKeepVisibility(V.getName()) ||
      V.getVisibility() != GlobalValue::DefaultVisibility) {
    return false;
  }
  V.setVisibility(GlobalValue::HiddenVisibility);
  return true;
}

} // namespace

namespace llvm {
struct SymbolVisibilityObfuscation : public ModulePass {
  static char ID;
  bool flag;
  SymbolVisibilityObfuscation() : ModulePass(ID) { this->flag = true; }
  SymbolVisibilityObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override {
    return "SymbolVisibilityObfuscation";
  }

  bool isSwiftModule(Module &M) {
    for (const Function &Fn : M) {
      if (Fn.getCallingConv() == CallingConv::Swift)
        return true;
      StringRef N = Fn.getName();
#if LLVM_VERSION_MAJOR >= 18
      if (N.starts_with("$s") || N.starts_with("_$s"))
#else
      if (N.startswith("$s") || N.startswith("_$s"))
#endif
        return true;
    }
    return false;
  }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    // Skip Swift modules — hiding visibility of $s-prefixed Swift symbols
    // breaks cross-TU Swift linking and runtime dispatch.
    if (isSwiftModule(M)) {
      errs() << "[SymbolVisibilityObfuscation] Skipping Swift module: "
             << M.getSourceFileName() << "\n";
      return false;
    }

    errs() << "Running SymbolVisibilityObfuscation pass on "
           << M.getSourceFileName() << "\n";

    bool Changed = false;
    unsigned Updated = 0;

    for (Function &F : M) {
      if (F.isDeclaration() || F.hasAvailableExternallyLinkage()) {
        continue;
      }
      // Skip functions with local linkage - they are already invisible
      // outside the module. Changing their visibility breaks the Swift
      // batch-mode module splitter and causes duplicate symbol errors.
      if (F.hasLocalLinkage()) {
        continue;
      }
      if (updateVisibility(F)) {
        Changed = true;
        ++Updated;
      }
    }

    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer()) {
        continue;
      }
      if (GV.getSection().contains("__objc") || GV.getSection().contains("__swift")) {
        continue;
      }
      // Skip globals with local linkage (private/internal) - they are
      // already invisible. Changing visibility on these causes the MachO
      // linker to reject duplicate private-external symbols when the
      // Swift module splitter copies them across .o files.
      if (GV.hasLocalLinkage()) {
        continue;
      }
      if (updateVisibility(GV)) {
        Changed = true;
        ++Updated;
      }
    }

    for (GlobalAlias &GA : M.aliases()) {
      if (GA.hasLocalLinkage()) {
        continue;
      }
      if (updateVisibility(GA)) {
        Changed = true;
        ++Updated;
      }
    }

    errs() << "[SymbolVisibilityObfuscation] updated visibility count: "
           << Updated << "\n";
    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createSymbolVisibilityObfuscationPass(bool flag) {
  return new SymbolVisibilityObfuscation(flag);
}

char SymbolVisibilityObfuscation::ID = 0;
INITIALIZE_PASS(SymbolVisibilityObfuscation, "symvisobf",
                "Hide Global Symbol Visibility.", false, false)
