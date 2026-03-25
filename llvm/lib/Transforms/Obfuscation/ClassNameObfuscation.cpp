// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
#include "llvm/Transforms/Obfuscation/ClassNameObfuscation.h"
#if defined(__has_include)
#if __has_include("llvm/TargetParser/Triple.h")
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif
#else
#include "llvm/ADT/Triple.h"
#endif
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace {

static cl::opt<std::string> ClassNameObfuscationSeed(
    "clsobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic ClassNameObfuscation. "
             "Use the same seed across all translation units in one build."));

#if LLVM_VERSION_MAJOR >= 18
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.starts_with(Prefix);
}
inline bool endsWith(StringRef Name, StringRef Suffix) {
  return Name.ends_with(Suffix);
}
#else
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.startswith(Prefix);
}
inline bool endsWith(StringRef Name, StringRef Suffix) {
  return Name.endswith(Suffix);
}
#endif

bool isSwiftModuleBlacklisted(StringRef Name) {
  static const std::unordered_set<std::string> Blacklist = {
      "Swift", "Foundation", "ObjectiveC", "Dispatch", "CoreFoundation",
      "UIKit", "Combine",
  };
  return Blacklist.count(Name.str()) != 0;
}

size_t findSwiftMangledTokenStart(StringRef Name) {
  size_t Pos = Name.find("$s");
  if (Pos != StringRef::npos) {
    return Pos + 2;
  }
  Pos = Name.find("_$s");
  if (Pos != StringRef::npos) {
    return Pos + 3;
  }
  return StringRef::npos;
}

std::string extractSwiftModuleName(StringRef Name) {
  const size_t Start = findSwiftMangledTokenStart(Name);
  if (Start == StringRef::npos || Start >= Name.size() ||
      !std::isdigit(static_cast<unsigned char>(Name[Start]))) {
    return "";
  }
  size_t I = Start;
  size_t Len = 0;
  while (I < Name.size() && std::isdigit(static_cast<unsigned char>(Name[I]))) {
    Len = Len * 10 + static_cast<size_t>(Name[I] - '0');
    ++I;
  }
  if (Len == 0 || I + Len > Name.size()) {
    return "";
  }
  return Name.substr(I, Len).str();
}

unsigned hexToNibble(char C) {
  if (C >= '0' && C <= '9') {
    return static_cast<unsigned>(C - '0');
  }
  if (C >= 'a' && C <= 'f') {
    return static_cast<unsigned>(10 + C - 'a');
  }
  if (C >= 'A' && C <= 'F') {
    return static_cast<unsigned>(10 + C - 'A');
  }
  return 0;
}

std::string buildEntropyHex(StringRef Domain, StringRef Seed, StringRef Source,
                            unsigned Attempt, size_t MinChars) {
  std::string Entropy;
  Entropy.reserve(MinChars + 32);
  unsigned Block = 0;
  while (Entropy.size() < MinChars) {
    MD5 Hasher;
    Hasher.update(Domain);
    Hasher.update(Seed);
    Hasher.update(Source);
    Hasher.update(std::to_string(Attempt));
    Hasher.update(std::to_string(Block));
    MD5::MD5Result Digest;
    Hasher.final(Digest);
    SmallString<32> Hex;
    MD5::stringifyResult(Digest, Hex);
    Entropy.append(Hex.str().data(), Hex.str().size());
    ++Block;
  }
  return Entropy;
}

std::string getClassNameSeed() {
  if (!ClassNameObfuscationSeed.empty()) {
    return ClassNameObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("CLSOBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-clsobf-default-seed";
}

std::string generateDeterministicClassName(StringRef OldClassName,
                                           StringRef Seed, unsigned Attempt) {
  static constexpr char FirstCharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

  if (OldClassName.empty()) {
    return "A";
  }

  std::string Entropy = buildEntropyHex("hikari.clsobf.class", Seed,
                                        OldClassName, Attempt,
                                        OldClassName.size() * 2);
  const size_t Length = OldClassName.size();
  std::string Result;
  Result.reserve(Length);
  const unsigned HeadByte =
      (hexToNibble(Entropy[0]) << 4) | hexToNibble(Entropy[1]);
  Result.push_back(FirstCharSet[HeadByte % (sizeof(FirstCharSet) - 1)]);
  for (size_t I = 1; I < Length; ++I) {
    const size_t J = I * 2;
    const unsigned Byte =
        (hexToNibble(Entropy[J]) << 4) | hexToNibble(Entropy[J + 1]);
    Result.push_back(CharSet[Byte % (sizeof(CharSet) - 1)]);
  }
  return Result;
}

bool replaceSwiftClassTokenInPlace(std::string &S, StringRef OldClassName,
                                   StringRef NewClassName) {
  std::string OldToken = std::to_string(OldClassName.size()) + OldClassName.str();
  std::string NewToken = std::to_string(NewClassName.size()) + NewClassName.str();

  bool Changed = false;
  size_t Pos = 0;
  while ((Pos = S.find(OldToken, Pos)) != std::string::npos) {
    // Ensure the length-prefix digits we matched are a complete number,
    // not the tail of a longer number.  E.g., in "25AgoraChannel...",
    // searching for "5Agora" would falsely match at the '5' that is
    // actually part of '25'.  Guard: preceding char must NOT be a digit.
    if (Pos > 0 &&
        std::isdigit(static_cast<unsigned char>(S[Pos - 1]))) {
      ++Pos;
      continue;
    }
    size_t TokenEnd = Pos + OldToken.size();
    bool LooksLikeClassToken = (TokenEnd == S.size()) || (S[TokenEnd] == 'C');
    if (!LooksLikeClassToken) {
      Pos = TokenEnd;
      continue;
    }
    S.replace(Pos, OldToken.size(), NewToken);
    Pos += NewToken.size();
    Changed = true;
  }
  return Changed;
}

bool ensureClassNameMapping(
    const std::string &OldClassName, StringRef Seed,
    std::unordered_map<std::string, std::string> &ClassNameMap) {
  if (ClassNameMap.count(OldClassName)) {
    return true;
  }

  std::string NewClassName;
  for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
    NewClassName = generateDeterministicClassName(OldClassName, Seed, Attempt);
    if (NewClassName != OldClassName) {
      break;
    }
    NewClassName.clear();
  }
  if (NewClassName.empty()) {
    return false;
  }

  ClassNameMap.emplace(OldClassName, NewClassName);
  return true;
}

/// Helper: try to read a length-prefixed identifier at position I in Name.
/// If followed by 'C' (class kind), add it to Out.  Returns the position
/// after the identifier+kind, or 0 on failure.
size_t tryCollectClassAt(StringRef Name, size_t I,
                         std::unordered_set<std::string> &Out) {
  if (I >= Name.size() ||
      !std::isdigit(static_cast<unsigned char>(Name[I]))) {
    return 0;
  }
  size_t Len = 0;
  size_t J = I;
  while (J < Name.size() &&
         std::isdigit(static_cast<unsigned char>(Name[J]))) {
    Len = Len * 10 + (Name[J] - '0');
    ++J;
  }
  if (Len == 0 || J + Len >= Name.size()) {
    return 0;
  }
  // Reject leading zeros in the length prefix.  Valid Swift mangling uses
  // canonical decimal: "5Agora", never "05Agora".  A leading '0' before
  // another digit typically indicates a Swift mangling context separator
  // (e.g. AA0<len><name> for nested/generic types), not a real length prefix.
  // Parsing "05Agora" as length=5 identifier="Agora" would be a false positive.
  if (J > I + 1 && Name[I] == '0') {
    return 0;
  }
  StringRef Ident = Name.substr(J, Len);
  // Sanity: identifier must look like a Swift type name (start with uppercase
  // letter, reasonable length, no embedded digits at the start).
  if (Ident.empty() ||
      !std::isupper(static_cast<unsigned char>(Ident[0])) ||
      Len > 128) {
    return 0;
  }
  size_t End = J + Len;
  if (Name[End] == 'C') {
    Out.emplace(Ident.str());
  }
  return End + 1; // skip past the kind character
}

void collectSwiftClassNamesFromSymbol(StringRef Name,
                                      std::unordered_set<std::string> &Out,
                                      const std::unordered_set<std::string>
                                          &TargetModules) {
  const std::string ModuleName = extractSwiftModuleName(Name);
  if (ModuleName.empty() || TargetModules.count(ModuleName) == 0) {
    return;
  }

  size_t I = findSwiftMangledTokenStart(Name);
  if (I == StringRef::npos) {
    return;
  }

  // Skip past the module name: <len_digits><module_chars>
  while (I < Name.size() &&
         std::isdigit(static_cast<unsigned char>(Name[I]))) {
    ++I;
  }
  I += ModuleName.size();
  if (I >= Name.size()) {
    return;
  }

  // 1. Collect the FIRST entity after the module name.
  //    This is the top-level type from this module.
  tryCollectClassAt(Name, I, Out);

  // 2. Scan for "AA" substitution patterns.
  //    In Swift ABI mangling, "AA" is a back-reference to the first
  //    substitutable entity (the module).  So "AA<len><name>C" means
  //    a class <name> from the target module.  This catches classes
  //    that appear as parameter types, property types, etc.
  //    We do NOT collect entities after other prefixes (So = ObjC stdlib,
  //    other modules) to avoid renaming system classes.
  for (size_t P = I; P + 2 < Name.size(); ++P) {
    if (Name[P] == 'A' && Name[P + 1] == 'A') {
      size_t Next = P + 2;
      if (Next < Name.size() &&
          std::isdigit(static_cast<unsigned char>(Name[Next]))) {
        tryCollectClassAt(Name, Next, Out);
      }
    }
  }
}

/// Extract a Swift class name from a _TtC-encoded string.
/// _TtC format: _TtC<module_len><module><class_len><class>
/// Returns the extracted class name, or empty if not matching.
std::string extractClassFromTtC(StringRef S,
                                const std::unordered_set<std::string>
                                    &TargetModules) {
  if (!startsWith(S, "_TtC")) {
    return "";
  }
  size_t I = 4; // skip "_TtC"
  if (I >= S.size() ||
      !std::isdigit(static_cast<unsigned char>(S[I]))) {
    return "";
  }
  size_t ModuleLen = 0;
  while (I < S.size() &&
         std::isdigit(static_cast<unsigned char>(S[I]))) {
    ModuleLen = ModuleLen * 10 + (S[I] - '0');
    ++I;
  }
  if (ModuleLen == 0 || I + ModuleLen > S.size()) {
    return "";
  }
  StringRef ModuleName = S.substr(I, ModuleLen);
  I += ModuleLen;
  if (TargetModules.count(ModuleName.str()) == 0) {
    return "";
  }
  if (I >= S.size() ||
      !std::isdigit(static_cast<unsigned char>(S[I]))) {
    return "";
  }
  size_t ClassLen = 0;
  while (I < S.size() &&
         std::isdigit(static_cast<unsigned char>(S[I]))) {
    ClassLen = ClassLen * 10 + (S[I] - '0');
    ++I;
  }
  if (ClassLen == 0 || I + ClassLen > S.size()) {
    return "";
  }
  return S.substr(I, ClassLen).str();
}

/// Scan a global name for _TtC<module><class> patterns anywhere in it.
/// This catches OBJC_CLASS_$_, OBJC_METACLASS_$_, l_OBJC_CLASS_RO_$_, etc.
void collectSwiftClassFromTtCGlobal(
    StringRef GlobalName,
    std::unordered_set<std::string> &Out,
    const std::unordered_set<std::string> &TargetModules) {
  // Find _TtC anywhere in the global name
  size_t Pos = GlobalName.find("_TtC");
  while (Pos != StringRef::npos) {
    StringRef Sub = GlobalName.substr(Pos);
    std::string ClassName = extractClassFromTtC(Sub, TargetModules);
    if (!ClassName.empty()) {
      Out.emplace(ClassName);
    }
    Pos = GlobalName.find("_TtC", Pos + 4);
  }
}

std::string rewriteSwiftEncodedName(
    StringRef Name,
    const std::unordered_map<std::string, std::string> &ClassNameMap) {
  std::string Rewritten = Name.str();
  bool Changed = false;

  for (const auto &KV : ClassNameMap) {
    Changed |= replaceSwiftClassTokenInPlace(Rewritten, KV.first, KV.second);
  }

  if (Name.contains(".str.")) {
    for (const auto &KV : ClassNameMap) {
      std::string OldSuffix = "." + KV.first;
      if (endsWith(Rewritten, OldSuffix)) {
        Rewritten.resize(Rewritten.size() - KV.first.size());
        Rewritten += KV.second;
        Changed = true;
      }
    }
  }

  if (!Changed) {
    return Name.str();
  }
  return Rewritten;
}

bool renameObjCClassNameString(
    GlobalVariable &GV, Module &M,
    const std::unordered_map<std::string, std::string> &ClassNameMap) {
  StringRef Section = GV.getSection();
  StringRef Name = GV.getName();
  if (!GV.hasInitializer() ||
      (!Section.contains("__objc_classname") &&
       !Name.contains("OBJC_CLASS_NAME_"))) {
    return false;
  }

  ConstantDataSequential *CDS =
      dyn_cast<ConstantDataSequential>(GV.getInitializer());
  if (!CDS || !CDS->isCString()) {
    return false;
  }

  std::string OldClassName = CDS->getAsCString().str();
  auto It = ClassNameMap.find(OldClassName);
  if (It == ClassNameMap.end()) {
    return false;
  }

  Constant *NewInitializer =
      ConstantDataArray::getString(M.getContext(), It->second, true);
  if (NewInitializer->getType() != GV.getValueType()) {
    errs() << "[ClassNameObfuscation] Skip class name string rewrite for "
           << OldClassName << " due to size mismatch.\n";
    return false;
  }

  GV.setInitializer(NewInitializer);
  return true;
}

bool renameSwiftMetadataString(
    GlobalVariable &GV, Module &M,
    const std::unordered_map<std::string, std::string> &ClassNameMap) {
  if (!GV.hasInitializer()) {
    return false;
  }

  ConstantDataSequential *CDS =
      dyn_cast<ConstantDataSequential>(GV.getInitializer());
  if (!CDS || !CDS->isCString()) {
    return false;
  }

  StringRef GVName = GV.getName();
  StringRef Section = GV.getSection();
  std::string OldString = CDS->getAsCString().str();

  bool LikelyMetadata =
      Section.contains("__swift") || Section.contains("__objc") ||
      GVName.contains(".str.") || OldString.find("_TtC") != std::string::npos ||
      OldString.find("_TtCV") != std::string::npos ||
      startsWith(OldString, "$s") || startsWith(OldString, "symbolic ");
  if (!LikelyMetadata) {
    return false;
  }

  std::string NewString = rewriteSwiftEncodedName(OldString, ClassNameMap);
  auto It = ClassNameMap.find(OldString);
  if (It != ClassNameMap.end()) {
    NewString = It->second;
  }

  if (NewString == OldString) {
    return false;
  }

  Constant *NewInitializer =
      ConstantDataArray::getString(M.getContext(), NewString, true);
  if (NewInitializer->getType() != GV.getValueType()) {
    errs() << "[ClassNameObfuscation] Skip swift metadata string rewrite for "
           << GV.getName() << " due to size mismatch.\n";
    return false;
  }

  GV.setInitializer(NewInitializer);
  return true;
}

} // namespace

namespace llvm {
struct ClassNameObfuscation : public ModulePass {
  static char ID;
  bool flag;
  ClassNameObfuscation() : ModulePass(ID) { this->flag = true; }
  ClassNameObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override { return "ClassNameObfuscation"; }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    errs() << "Running ClassNameObfuscation pass on "
           << M.getSourceFileName() << "\n";
    const std::string Seed = getClassNameSeed();
    errs() << "[ClassNameObfuscation] seed: " << Seed << "\n";

    Triple triple(M.getTargetTriple());
    if (triple.getVendor() != Triple::VendorType::Apple) {
      errs() << "[ClassNameObfuscation] Ignore unsupported target "
             << M.getTargetTriple() << "\n";
      return false;
    }

    // Build TargetModules: prefer CLSOBF_TARGET_MODULES env var for cross-TU
    // consistency (same as FNNAMEOBF_TARGET_MODULES for FunctionNameObfuscation).
    // When env var is set, every TU uses the same target modules regardless of
    // whether it has definitions, preventing linker errors from inconsistent rename.
    std::unordered_set<std::string> TargetModules;
    if (const char *EnvModules = std::getenv("CLSOBF_TARGET_MODULES")) {
      std::string ModStr(EnvModules);
      size_t Start = 0;
      while (Start < ModStr.size()) {
        size_t End = ModStr.find(',', Start);
        if (End == std::string::npos)
          End = ModStr.size();
        std::string Mod = ModStr.substr(Start, End - Start);
        while (!Mod.empty() && Mod.front() == ' ')
          Mod = Mod.substr(1);
        while (!Mod.empty() && Mod.back() == ' ')
          Mod.pop_back();
        if (!Mod.empty())
          TargetModules.emplace(Mod);
        Start = End + 1;
      }
      errs() << "[ClassNameObfuscation] Using CLSOBF_TARGET_MODULES: ";
      for (const auto &M2 : TargetModules)
        errs() << M2 << " ";
      errs() << "\n";
    } else {
      // Fallback: infer from definitions in this TU
      for (Function &F : M) {
        if (F.isDeclaration() || !F.hasName()) {
          continue;
        }
        std::string ModuleName = extractSwiftModuleName(F.getName());
        if (ModuleName.empty() || isSwiftModuleBlacklisted(ModuleName)) {
          continue;
        }
        TargetModules.emplace(ModuleName);
      }
    }

    // Collect Swift class names from all symbol kinds (globals, functions,
    // aliases) to maximize cross-TU coverage.  Every TU that references a
    // Swift class from a target module will have some mangled symbol (type
    // metadata descriptor, accessor function, etc.) containing the class name.
    //
    // We intentionally do NOT collect pure ObjC classes (OBJC_CLASS_$_*)
    // because ObjC class names lack embedded module information, making it
    // impossible to achieve cross-TU consistency: the defining TU would rename
    // the class while referencing TUs (which only see a declaration) would not,
    // causing linker errors.  Swift classes that are @objc still get their
    // OBJC_CLASS_$_ globals renamed because the class name is discovered here
    // via Swift mangled symbols and the rename phase handles all ObjC prefixes.
    std::unordered_set<std::string> SwiftClasses;

    for (GlobalVariable &GV : M.globals()) {
      StringRef Name = GV.getName();
      // Scan $s-mangled names for class identifiers.
      collectSwiftClassNamesFromSymbol(Name, SwiftClasses, TargetModules);
      // Scan for _TtC<module><class> anywhere in global names.
      // This catches OBJC_CLASS_$_, OBJC_METACLASS_$_, l_OBJC_CLASS_RO_$_, etc.
      collectSwiftClassFromTtCGlobal(Name, SwiftClasses, TargetModules);
    }
    for (Function &F : M) {
      if (F.hasName()) {
        collectSwiftClassNamesFromSymbol(F.getName(), SwiftClasses,
                                         TargetModules);
      }
    }
    for (GlobalAlias &GA : M.aliases()) {
      if (GA.hasName()) {
        collectSwiftClassNamesFromSymbol(GA.getName(), SwiftClasses,
                                         TargetModules);
      }
    }

    if (SwiftClasses.empty()) {
      errs() << "[ClassNameObfuscation] No Swift class found in "
             << M.getSourceFileName() << "\n";
    }

    std::unordered_map<std::string, std::string> ClassNameMap;
    ClassNameMap.reserve(SwiftClasses.size());

    for (const std::string &OldClassName : SwiftClasses) {
      if (!ensureClassNameMapping(OldClassName, Seed, ClassNameMap)) {
        errs() << "[ClassNameObfuscation] Failed to generate obfuscated class "
                  "name for Swift class "
               << OldClassName << "\n";
        return false;
      }
    }

    // Collect pure ObjC class names from OBJC_CLASS_$_ definitions into a
    // SEPARATE map.  These names must NOT be used for $s symbol renaming
    // because referencing TUs may not collect them (no module info in ObjC),
    // causing cross-TU linker mismatches.  They are only used to rename
    // __objc_classname metadata strings in the defining TU.
    //
    // SAFETY: Only rename ObjC classes that match a user-specified prefix
    // (CLSOBF_OBJC_PREFIX env var).  Without a prefix filter, ALL ObjC class
    // definitions in the TU get renamed, which breaks Swift superclass
    // resolution and cross-framework compatibility when Pods are compiled
    // with -enable-clsobf.
    std::unordered_map<std::string, std::string> ObjCOnlyClassNameMap;
    std::string ObjCPrefix;
    if (const char *EnvPrefix = std::getenv("CLSOBF_OBJC_PREFIX")) {
      ObjCPrefix = EnvPrefix;
    }
    for (GlobalVariable &GV : M.globals()) {
      StringRef Name = GV.getName();
      if (!startsWith(Name, "OBJC_CLASS_$_"))
        continue;
      if (GV.isDeclaration())
        continue;
      StringRef ClassName = Name.drop_front(strlen("OBJC_CLASS_$_"));
      if (ClassName.contains("_TtC") || ClassName.contains("$s"))
        continue;
      if (ClassName.empty())
        continue;
      // Skip if already collected by Swift path
      if (SwiftClasses.count(ClassName.str()))
        continue;
      // If a prefix filter is set, only rename classes with that prefix.
      // This prevents renaming framework/Pod classes that are expected by
      // other code (e.g., Swift subclasses referencing ObjC superclasses).
      if (!ObjCPrefix.empty() && !startsWith(ClassName, ObjCPrefix)) {
        continue;
      }
      if (!ensureClassNameMapping(ClassName.str(), Seed, ObjCOnlyClassNameMap)) {
        continue;
      }
    }

    if (SwiftClasses.empty() && ObjCOnlyClassNameMap.empty()) {
      errs() << "[ClassNameObfuscation] No class found in "
             << M.getSourceFileName() << "\n";
      return false;
    }

    // ---- Phase 2: Rename __objc_classname strings ONLY ----
    //
    // We ONLY rename __objc_classname metadata strings (the C strings that the
    // ObjC runtime uses as class names).  This changes the runtime-visible class
    // name, which is what the similarity analysis tool reads from the binary.
    //
    // We intentionally do NOT rename $s-mangled symbol names, function names,
    // global alias names, or embedded Swift type-metadata strings because:
    //   1. Each Swift file / batch is compiled as a separate TU.  The class set
    //      collected by collectSwiftClassNamesFromSymbol differs between TUs
    //      (since each TU has different $s symbols), leading to inconsistent
    //      renames and undefined-symbol linker errors.
    //   2. Renaming embedded type-metadata strings (symbolic references,
    //      __swift5_typeref) can corrupt Swift's runtime reflection machinery.
    //   3. The OBJC_CLASS_$_ linker symbols must stay unchanged for cross-TU
    //      and cross-language (ObjC↔Swift) linking to work.
    //
    // The $s symbols still contain original class names in the symbol table,
    // but the symbol table can be stripped from the release binary.
    bool Changed = false;
    for (GlobalVariable &GV : M.globals()) {
      StringRef GVName = GV.getName();

      bool IsClassNameString =
          GV.getSection().contains("__objc_classname") ||
          GVName.contains("OBJC_CLASS_NAME_");
      if (IsClassNameString) {
        // renameObjCClassNameString: exact-match rename for bare class names
        // (pure ObjC classes like "WKTestClass" → "obU6yF5M0Lq").
        // Note: ClassNameMap contains Swift bare names (e.g. "WKSomeView"),
        // but __objc_classname strings for Swift classes use _TtC encoding
        // (e.g. "_TtC11Wearfit_Pro10WKSomeView"), so this won't match them.
        // This effectively only renames bare ObjC class names that happen to
        // also exist in the Swift class map (unlikely).
        Changed |= renameObjCClassNameString(GV, M, ClassNameMap);
        Changed |= renameObjCClassNameString(GV, M, ObjCOnlyClassNameMap);
        // DO NOT call renameSwiftMetadataString on __objc_classname strings.
        // Changing the _TtC-encoded runtime class name breaks Swift's runtime
        // class resolution: the ObjC runtime registers the class under the
        // obfuscated name, but Swift type metadata, protocol conformances,
        // and swift_getClass() still use the original name, causing SIGBUS.
      }
    }

    // ---- Phase 3: Patch property attribute strings ----
    //
    // Property attributes contain class references like T@"WKFoo",N,V_foo.
    // When Phase 2 renames WKFoo → obfXyz in __objc_classname, other TUs'
    // property attrs still reference T@"WKFoo". The ObjC runtime's
    // property_copyAttributeValue("T") returns @"WKFoo", causing
    // NSClassFromString(@"WKFoo") → nil (class registered as "obfXyz").
    // Fix: find all T@"OldClass" patterns and replace with T@"NewClass".
    // Since generateDeterministicClassName produces same-length names,
    // the replacement is exact-length and the GV type is preserved.
    unsigned PropPatched = 0;
    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer())
        continue;
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString())
        continue;
      std::string Str = CDS->getAsCString().str();
      if (Str.find("T@\"") == std::string::npos)
        continue;

      bool StrChanged = false;
      for (const auto &KV : ObjCOnlyClassNameMap) {
        std::string Pattern = "T@\"" + KV.first + "\"";
        std::string Replacement = "T@\"" + KV.second + "\"";
        size_t Pos = 0;
        while ((Pos = Str.find(Pattern, Pos)) != std::string::npos) {
          Str.replace(Pos, Pattern.size(), Replacement);
          Pos += Replacement.size();
          StrChanged = true;
        }
      }

      if (StrChanged) {
        Constant *NewInit =
            ConstantDataArray::getString(M.getContext(), Str, true);
        if (NewInit->getType() == GV.getValueType()) {
          GV.setInitializer(NewInit);
          ++PropPatched;
          Changed = true;
        }
      }
    }
    if (PropPatched > 0) {
      errs() << "[ClassNameObfuscation] Phase 3: patched " << PropPatched
             << " property attribute strings\n";
    }

    for (const auto &KV : ClassNameMap) {
      errs() << "[ClassNameObfuscation] " << KV.first << " -> " << KV.second
             << "\n";
    }
    for (const auto &KV : ObjCOnlyClassNameMap) {
      errs() << "[ClassNameObfuscation:ObjC] " << KV.first << " -> "
             << KV.second << "\n";
    }

    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createClassNameObfuscationPass(bool flag) {
  return new ClassNameObfuscation(flag);
}

char ClassNameObfuscation::ID = 0;
INITIALIZE_PASS(ClassNameObfuscation, "clsobf",
                "Rename Objective-C/Swift Class Names.", false, false)
