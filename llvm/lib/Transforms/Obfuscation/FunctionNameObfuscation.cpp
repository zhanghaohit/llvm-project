// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/FunctionNameObfuscation.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdlib>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace {

static cl::opt<std::string> FunctionNameObfuscationSeed(
    "fnnameobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic FunctionNameObfuscation. "
             "Use the same seed across all translation units in one build."));

// Comma-separated list of Swift module names whose functions should be
// obfuscated.  When set, a function is renamed if its mangled name CONTAINS
// any of the specified module names (handles both direct module functions and
// cross-module extensions such as `extension RxSwift.Reactive` defined in
// `Wearfit_Pro`).  This option is required for correct cross-TU consistency
// when the project contains extensions on third-party types.
//
// Example: -fnnameobf-target-modules=Wearfit_Pro
// Or set env var FNNAMEOBF_TARGET_MODULES=Wearfit_Pro
static cl::opt<std::string> FunctionNameObfuscationTargetModules(
    "fnnameobf-target-modules", cl::init(""), cl::NotHidden,
    cl::desc("Comma-separated Swift module names to obfuscate. "
             "When set, replaces per-TU SwiftModules inference with a "
             "consistent cross-TU criterion: rename if the mangled symbol "
             "contains any listed module name."));

#if LLVM_VERSION_MAJOR >= 18
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.starts_with(Prefix);
}
#else
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.startswith(Prefix);
}
#endif

bool isSwiftModuleBlacklisted(StringRef Name) {
  static const std::unordered_set<std::string> Blacklist = {
      "Swift", "Foundation", "ObjectiveC", "Dispatch", "CoreFoundation",
      "UIKit", "Combine",
  };
  return Blacklist.count(Name.str()) != 0;
}

std::optional<std::string> extractSwiftModuleName(StringRef Name) {
  size_t I = StringRef::npos;
  if (startsWith(Name, "$s")) {
    I = 2;
  } else if (startsWith(Name, "_$s")) {
    I = 3;
  } else {
    return std::nullopt;
  }

  if (I >= Name.size() || !std::isdigit(static_cast<unsigned char>(Name[I]))) {
    return std::nullopt;
  }

  size_t Len = 0;
  while (I < Name.size() && std::isdigit(static_cast<unsigned char>(Name[I]))) {
    Len = Len * 10 + static_cast<size_t>(Name[I] - '0');
    ++I;
  }
  if (Len == 0 || I + Len > Name.size()) {
    return std::nullopt;
  }
  return Name.substr(I, Len).str();
}

bool isRuntimeOrSystemName(StringRef Name) {
  if (Name == "main") {
    return true;
  }
  if (startsWith(Name, "llvm.")) {
    return true;
  }
  // Filter Swift runtime and compiler-generated symbols (single and double underscore)
  if (startsWith(Name, "swift_") || startsWith(Name, "_swift") ||
      startsWith(Name, "__swift")) {
    return true;
  }
  // Filter Objective-C runtime symbols
  if (startsWith(Name, "OBJC_") || startsWith(Name, "_OBJC_") ||
      startsWith(Name, "__objc") || startsWith(Name, "objc_")) {
    return true;
  }
  return false;
}

/// Returns true if F is a Swift async function.
static bool isSwiftAsyncFunction(const Function &F) {
  for (const Argument &A : F.args()) {
    if (A.hasAttribute(Attribute::SwiftAsync)) {
      return true;
    }
  }
  return false;
}

/// Returns true if F is a synchronous coroutine.
static bool isCoroutineFunction(const Function &F) {
  if (F.isDeclaration()) return false;
  for (const BasicBlock &BB : F) {
    for (const Instruction &I : BB) {
      if (const auto *CI = dyn_cast<CallInst>(&I)) {
        if (const Function *Callee = CI->getCalledFunction()) {
          if (Callee->getName().starts_with("llvm.coro.id")) {
            return true;
          }
        }
      }
    }
  }
  return false;
}

/// Name-based heuristic for known Swift coroutine entry points.
static bool isSwiftCoroutineByName(StringRef Name) {
  if (Name.ends_with("vM")) return true;
  if (Name.ends_with("vr")) return true;
  if (Name.ends_with("AM")) return true;
  if (Name.ends_with("Ar")) return true;
  return false;
}

// ---------------------------------------------------------------------------
// Target-module list support
//
// When -fnnameobf-target-modules (or env FNNAMEOBF_TARGET_MODULES) is set,
// we use a "contains" check instead of the per-TU SwiftModules inference.
// This correctly handles cross-module extensions such as
//   $s12CoreGraphics7CGFloatV11Wearfit_ProE20floorToDecimalPlaces...
// which has first-component "CoreGraphics" but is *defined in* "Wearfit_Pro".
// ---------------------------------------------------------------------------

/// Parse a comma-separated module list string into a vector.
static std::vector<std::string> parseModuleList(const std::string &Str) {
  std::vector<std::string> Mods;
  std::stringstream SS(Str);
  std::string Tok;
  while (std::getline(SS, Tok, ',')) {
    // Trim whitespace
    size_t S = Tok.find_first_not_of(" \t");
    size_t E = Tok.find_last_not_of(" \t");
    if (S != std::string::npos)
      Mods.push_back(Tok.substr(S, E - S + 1));
  }
  return Mods;
}

/// Returns the target module list (from CLI opt or env var), or empty if not
/// configured.
static std::vector<std::string> getTargetModuleList() {
  std::string ModStr = FunctionNameObfuscationTargetModules;
  if (ModStr.empty()) {
    if (const char *Env = std::getenv("FNNAMEOBF_TARGET_MODULES")) {
      if (Env && *Env != '\0')
        ModStr = Env;
    }
  }
  if (ModStr.empty())
    return {};
  return parseModuleList(ModStr);
}

/// Returns true if the mangled Swift symbol Name contains any of the target
/// module names in their mangled form (e.g. "11Wearfit_Pro" for "Wearfit_Pro").
/// This correctly identifies both direct module functions ($s11Wearfit_Pro...)
/// AND cross-module extensions ($s12CoreGraphics...11Wearfit_ProE...).
static bool containsTargetModule(StringRef Name,
                                 const std::vector<std::string> &TargetMods) {
  for (const auto &Mod : TargetMods) {
    // The mangled form of module "Foo" is "<len>Foo", e.g. "7RxSwift".
    std::string Pattern = std::to_string(Mod.size()) + Mod;
    if (Name.contains(Pattern))
      return true;
  }
  return false;
}

/// Common safety checks shared by both definition and declaration paths.
static bool passesCommonFilters(StringRef Name) {
  if (!startsWith(Name, "$s") && !startsWith(Name, "_$s"))
    return false;
  if (isRuntimeOrSystemName(Name))
    return false;
  if (isSwiftCoroutineByName(Name))
    return false;
  return true;
}

// ---------------------------------------------------------------------------
// Per-TU SwiftModules inference (legacy path, used when no target-module list)
// ---------------------------------------------------------------------------

bool shouldRenameDefinition(const Function &F,
                            const std::unordered_set<std::string> &SwiftMods) {
  if (F.isDeclaration() || !F.hasName() || F.isIntrinsic()) {
    return false;
  }
  if (F.hasAvailableExternallyLinkage()) {
    return false;
  }
  if (F.hasLocalLinkage()) {
    return false;
  }
  if (isSwiftAsyncFunction(F)) {
    return false;
  }
  if (isCoroutineFunction(F)) {
    return false;
  }

  StringRef Name = F.getName();
  if (isRuntimeOrSystemName(Name)) {
    return false;
  }
  if (isSwiftCoroutineByName(Name)) {
    return false;
  }

  std::optional<std::string> Mod = extractSwiftModuleName(Name);
  return Mod.has_value() && SwiftMods.count(*Mod) != 0;
}

bool shouldRenameDeclaration(
    const Function &F, const std::unordered_set<std::string> &SwiftMods) {
  if (!F.isDeclaration() || !F.hasName() || F.isIntrinsic()) {
    return false;
  }
  if (isSwiftAsyncFunction(F)) {
    return false;
  }

  StringRef Name = F.getName();
  if (isRuntimeOrSystemName(Name)) {
    return false;
  }
  if (isSwiftCoroutineByName(Name)) {
    return false;
  }

  std::optional<std::string> Mod = extractSwiftModuleName(Name);
  return Mod.has_value() && SwiftMods.count(*Mod) != 0;
}

// ---------------------------------------------------------------------------
// Target-module-list path: rename if symbol contains any target module name
// ---------------------------------------------------------------------------

static bool shouldRenameDefinitionTM(const Function &F,
                                     const std::vector<std::string> &TM) {
  if (F.isDeclaration() || !F.hasName() || F.isIntrinsic())
    return false;
  if (F.hasAvailableExternallyLinkage())
    return false;
  if (F.hasLocalLinkage())
    return false;
  if (isSwiftAsyncFunction(F))
    return false;
  if (isCoroutineFunction(F))
    return false;
  StringRef Name = F.getName();
  if (!passesCommonFilters(Name))
    return false;
  return containsTargetModule(Name, TM);
}

static bool shouldRenameDeclarationTM(const Function &F,
                                      const std::vector<std::string> &TM) {
  if (!F.isDeclaration() || !F.hasName() || F.isIntrinsic())
    return false;
  if (isSwiftAsyncFunction(F))
    return false;
  StringRef Name = F.getName();
  if (!passesCommonFilters(Name))
    return false;
  return containsTargetModule(Name, TM);
}

// ---------------------------------------------------------------------------

std::string getCrossTUSeed() {
  if (!FunctionNameObfuscationSeed.empty()) {
    return FunctionNameObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("FNNAMEOBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-fnnameobf-default-seed";
}

std::string makeDeterministicFunctionName(StringRef OldName, StringRef Seed) {
  MD5 Hasher;
  Hasher.update("hikari.fnnameobf.v2");
  Hasher.update(Seed);
  Hasher.update(OldName);

  MD5::MD5Result Digest;
  Hasher.final(Digest);

  SmallString<32> Hex;
  MD5::stringifyResult(Digest, Hex);

  // Prefix must NOT start with "hikari_" — the Obfuscation scheduler's
  // cleanup code (Obfuscation.cpp) deletes every declaration whose name
  // starts with "hikari_" together with all its call-site instructions.
  // Using "hikari_fn_" would cause renamed declarations to be erased and
  // their users to become dangling references, corrupting the IR.
  std::string NewName = "xob_fn_";
  NewName += Hex.str().str();
  return NewName;
}

} // namespace

namespace llvm {
struct FunctionNameObfuscation : public ModulePass {
  static char ID;
  bool flag;
  FunctionNameObfuscation() : ModulePass(ID) { this->flag = true; }
  FunctionNameObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override { return "FunctionNameObfuscation"; }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    errs() << "Running FunctionNameObfuscation pass on "
           << M.getSourceFileName() << "\n";

    // Determine which path to use: target-module-list or per-TU SwiftModules.
    const std::vector<std::string> TargetModList = getTargetModuleList();
    const bool UseTargetModules = !TargetModList.empty();

    if (UseTargetModules) {
      errs() << "[FunctionNameObfuscation] target-modules: ";
      for (size_t I = 0; I < TargetModList.size(); ++I) {
        if (I) errs() << ",";
        errs() << TargetModList[I];
      }
      errs() << "\n";
    }

    // Legacy path: build per-TU SwiftModules from definitions.
    std::unordered_set<std::string> SwiftModules;
    if (!UseTargetModules) {
      SwiftModules.reserve(16);
      for (Function &F : M) {
        if (F.isDeclaration() || !F.hasName()) {
          continue;
        }
        std::optional<std::string> Mod = extractSwiftModuleName(F.getName());
        if (!Mod.has_value() || isSwiftModuleBlacklisted(*Mod)) {
          continue;
        }
        SwiftModules.emplace(*Mod);
      }
    }

    const std::string Seed = getCrossTUSeed();
    errs() << "[FunctionNameObfuscation] seed: " << Seed << "\n";

    bool Changed = false;
    unsigned RenamedDefs = 0;
    unsigned RenamedDecls = 0;

    std::unordered_map<std::string, std::string> RenameMap;
    RenameMap.reserve(M.size() * 2 + 1);
    for (Function &F : M) {
      if (!F.hasName()) {
        continue;
      }

      bool RenameDef, RenameDecl;
      if (UseTargetModules) {
        RenameDef  = shouldRenameDefinitionTM(F, TargetModList);
        RenameDecl = shouldRenameDeclarationTM(F, TargetModList);
      } else {
        RenameDef  = shouldRenameDefinition(F, SwiftModules);
        RenameDecl = shouldRenameDeclaration(F, SwiftModules);
      }

      if (!RenameDef && !RenameDecl) {
        continue;
      }

      std::string OldName = F.getName().str();
      auto It = RenameMap.find(OldName);
      if (It == RenameMap.end()) {
        It = RenameMap.emplace(OldName,
                               makeDeterministicFunctionName(OldName, Seed))
                 .first;
      }

      errs() << "Running FunctionNameObfuscation from " << OldName
             << " to " << It->second
             << " (" << (RenameDef ? "def" : "decl") << ")\n";
      F.setName(It->second);
      Changed = true;
      if (RenameDef) {
        ++RenamedDefs;
      } else {
        ++RenamedDecls;
      }
    }

    errs() << "[FunctionNameObfuscation] renamed defs=" << RenamedDefs
           << ", decls=" << RenamedDecls
           << (UseTargetModules ? " [target-modules mode]" : " [per-TU mode]")
           << "\n";
    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createFunctionNameObfuscationPass(bool flag) {
  return new FunctionNameObfuscation(flag);
}

char FunctionNameObfuscation::ID = 0;
INITIALIZE_PASS(FunctionNameObfuscation, "fnnameobf", "Rename Function Names.",
                false, false)
