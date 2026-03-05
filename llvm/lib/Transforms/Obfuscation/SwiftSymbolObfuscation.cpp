// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/SwiftSymbolObfuscation.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace {

static cl::opt<std::string> SwiftSymbolObfuscationSeed(
    "swiftsymobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic SwiftSymbolObfuscation. "
             "Use the same seed across all translation units in one build."));

#if LLVM_VERSION_MAJOR >= 18
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.starts_with(Prefix);
}
#else
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.startswith(Prefix);
}
#endif

bool isSwiftIdentifierChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_';
}

bool isSwiftIdentifier(StringRef S) {
  if (S.empty()) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(S.front())) && S.front() != '_') {
    return false;
  }
  for (char C : S) {
    if (!isSwiftIdentifierChar(C)) {
      return false;
    }
  }
  return true;
}

bool isSwiftIdentifierBlacklisted(StringRef Name) {
  static const std::unordered_set<std::string> Blacklist = {
      "Swift",        "Foundation", "ObjectiveC", "Dispatch",
      "CoreFoundation", "UIKit",    "Combine",    "main",
      "deinit",       "init",       "Type",       "Protocol",
  };
  return Blacklist.count(Name.str()) != 0;
}

bool looksLikeSwiftMangledName(StringRef Name) {
  return Name.contains("$s") || startsWith(Name, "_Tt") ||
         startsWith(Name, "$s");
}

size_t findSwiftMangledTokenStart(StringRef Name) {
  size_t Pos = Name.find("$s");
  if (Pos != StringRef::npos) {
    return Pos + 2;
  }
  Pos = Name.find("_Tt");
  if (Pos != StringRef::npos) {
    return Pos + 3;
  }
  return StringRef::npos;
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

std::string getSwiftSymbolSeed() {
  if (!SwiftSymbolObfuscationSeed.empty()) {
    return SwiftSymbolObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("SWIFTSYMOBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-swiftsymobf-default-seed";
}

std::string generateSameLengthIdentifier(StringRef OldName, StringRef Seed,
                                         unsigned Attempt) {
  static constexpr char FirstCharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

  if (OldName.empty()) {
    return "_";
  }

  std::string Entropy = buildEntropyHex("hikari.swiftsymobf.ident", Seed,
                                        OldName, Attempt, OldName.size() * 2);
  std::string NewName;
  NewName.reserve(OldName.size());
  const unsigned HeadByte =
      (hexToNibble(Entropy[0]) << 4) | hexToNibble(Entropy[1]);
  NewName.push_back(FirstCharSet[HeadByte % (sizeof(FirstCharSet) - 1)]);
  for (size_t I = 1; I < OldName.size(); ++I) {
    const size_t J = I * 2;
    const unsigned Byte =
        (hexToNibble(Entropy[J]) << 4) | hexToNibble(Entropy[J + 1]);
    NewName.push_back(CharSet[Byte % (sizeof(CharSet) - 1)]);
  }
  return NewName;
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

bool shouldRewriteMangledName(StringRef Name,
                              const std::unordered_set<std::string> &Modules) {
  if (!looksLikeSwiftMangledName(Name)) {
    return false;
  }
  std::string ModuleName = extractSwiftModuleName(Name);
  return !ModuleName.empty() && Modules.count(ModuleName) != 0;
}

void collectIdentifiersFromSwiftMangledName(
    StringRef Name, std::unordered_set<std::string> &Identifiers) {
  const size_t Start = findSwiftMangledTokenStart(Name);
  if (Start == StringRef::npos) {
    return;
  }

  size_t I = Start;
  unsigned TokenIndex = 0;
  while (I < Name.size()) {
    if (!std::isdigit(static_cast<unsigned char>(Name[I]))) {
      ++I;
      continue;
    }
    const size_t LenStart = I;
    size_t Len = 0;
    while (I < Name.size() && std::isdigit(static_cast<unsigned char>(Name[I]))) {
      Len = Len * 10 + static_cast<size_t>(Name[I] - '0');
      ++I;
    }
    if (Len == 0 || I + Len > Name.size()) {
      I = LenStart + 1;
      continue;
    }
    StringRef Ident = Name.substr(I, Len);
    I += Len;
    if (TokenIndex == 0) {
      ++TokenIndex;
      continue;
    }
    ++TokenIndex;
    if (!isSwiftIdentifier(Ident) || isSwiftIdentifierBlacklisted(Ident) ||
        Ident.size() < 3) {
      continue;
    }
    Identifiers.emplace(Ident.str());
  }
}

bool rewriteSwiftMangledName(
    std::string &Name, StringRef Seed,
    std::unordered_map<std::string, std::string> &IdentifierMap) {
  const size_t Start = findSwiftMangledTokenStart(Name);
  if (Start == StringRef::npos) {
    return false;
  }

  auto ensureMappedName = [&](StringRef OldName) -> bool {
    if (IdentifierMap.count(OldName.str()) != 0) {
      return true;
    }
    std::string NewName;
    for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
      NewName = generateSameLengthIdentifier(OldName, Seed, Attempt);
      if (NewName != OldName && !isSwiftIdentifierBlacklisted(NewName)) {
        break;
      }
      NewName.clear();
    }
    if (NewName.empty()) {
      return false;
    }
    IdentifierMap.emplace(OldName.str(), NewName);
    return true;
  };

  bool Changed = false;
  size_t I = Start;
  unsigned TokenIndex = 0;
  while (I < Name.size()) {
    if (!std::isdigit(static_cast<unsigned char>(Name[I]))) {
      ++I;
      continue;
    }
    const size_t LenStart = I;
    size_t Len = 0;
    while (I < Name.size() && std::isdigit(static_cast<unsigned char>(Name[I]))) {
      Len = Len * 10 + static_cast<size_t>(Name[I] - '0');
      ++I;
    }
    if (Len == 0 || I + Len > Name.size()) {
      I = LenStart + 1;
      continue;
    }

    StringRef Ident(&Name[I], Len);
    I += Len;
    if (TokenIndex == 0) {
      ++TokenIndex;
      continue;
    }
    ++TokenIndex;
    if (!isSwiftIdentifier(Ident) || isSwiftIdentifierBlacklisted(Ident) ||
        Ident.size() < 3) {
      continue;
    }
    if (!ensureMappedName(Ident)) {
      continue;
    }
    const std::string &NewIdent = IdentifierMap[Ident.str()];
    if (NewIdent != Ident) {
      Name.replace(I - Len, Len, NewIdent);
      Changed = true;
    }
  }

  return Changed;
}

bool isLikelySwiftMetadataString(const GlobalVariable &GV, StringRef S) {
  StringRef Section = GV.getSection();
  if (Section.contains("__swift")) {
    return true;
  }
  if (startsWith(S, "$s") || startsWith(S, "_Tt") || S.contains("$s") ||
      S.contains("_Tt")) {
    return true;
  }
  return S.contains("symbolic ");
}

bool tryRewriteCStringInitializer(GlobalVariable &GV, Module &M,
                                  StringRef NewString) {
  if (!GV.hasInitializer()) {
    return false;
  }
  ConstantDataSequential *CDS =
      dyn_cast<ConstantDataSequential>(GV.getInitializer());
  if (!CDS || !CDS->isCString()) {
    return false;
  }
  Constant *NewInitializer =
      ConstantDataArray::getString(M.getContext(), NewString, true);
  if (NewInitializer->getType() != GV.getValueType()) {
    return false;
  }
  GV.setInitializer(NewInitializer);
  return true;
}

} // namespace

namespace llvm {
struct SwiftSymbolObfuscation : public ModulePass {
  static char ID;
  bool flag;
  SwiftSymbolObfuscation() : ModulePass(ID) { this->flag = true; }
  SwiftSymbolObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override { return "SwiftSymbolObfuscation"; }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    // SwiftSymbolObfuscation renames identifiers inside $s mangled symbol
    // names.  This breaks linking in Swift batch compilation because each TU
    // infers a different set of "target modules" from its definitions, leading
    // to inconsistent renames across TUs.
    //
    // Until a proper cross-TU-consistent approach is implemented, allow
    // disabling via env var (same pattern as FunctionNameObfuscation).
    if (const char *Env = std::getenv("SWIFTSYMOBF_TARGET_MODULES")) {
      std::string Val(Env);
      if (Val == "__DISABLED__") {
        return false;
      }
    }

    errs() << "Running SwiftSymbolObfuscation pass on "
           << M.getSourceFileName() << "\n";

    const std::string Seed = getSwiftSymbolSeed();
    errs() << "[SwiftSymbolObfuscation] seed: " << Seed << "\n";

    std::unordered_set<std::string> TargetModules;
    for (Function &F : M) {
      if (F.isDeclaration() || !F.hasName() ||
          !looksLikeSwiftMangledName(F.getName())) {
        continue;
      }
      std::string ModuleName = extractSwiftModuleName(F.getName());
      if (ModuleName.empty() || isSwiftIdentifierBlacklisted(ModuleName)) {
        continue;
      }
      TargetModules.emplace(ModuleName);
    }
    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasName() || !GV.hasInitializer() ||
          !looksLikeSwiftMangledName(GV.getName())) {
        continue;
      }
      std::string ModuleName = extractSwiftModuleName(GV.getName());
      if (ModuleName.empty() || isSwiftIdentifierBlacklisted(ModuleName)) {
        continue;
      }
      TargetModules.emplace(ModuleName);
    }

    std::unordered_set<std::string> ExistingIdentifiers;
    std::unordered_map<std::string, std::string> IdentifierMap;

    for (Function &F : M) {
      if (!F.hasName() || !shouldRewriteMangledName(F.getName(), TargetModules)) {
        continue;
      }
      collectIdentifiersFromSwiftMangledName(F.getName(), ExistingIdentifiers);
    }
    for (GlobalVariable &GV : M.globals()) {
      if (GV.hasName() && shouldRewriteMangledName(GV.getName(), TargetModules)) {
        collectIdentifiersFromSwiftMangledName(GV.getName(),
                                               ExistingIdentifiers);
      }
      if (!GV.hasInitializer()) {
        continue;
      }
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString()) {
        continue;
      }
      StringRef Str = CDS->getAsCString();
      if (!isLikelySwiftMetadataString(GV, Str)) {
        continue;
      }
      if (shouldRewriteMangledName(Str, TargetModules)) {
        collectIdentifiersFromSwiftMangledName(Str, ExistingIdentifiers);
      }
    }
    for (GlobalAlias &GA : M.aliases()) {
      if (GA.hasName() && shouldRewriteMangledName(GA.getName(), TargetModules)) {
        collectIdentifiersFromSwiftMangledName(GA.getName(),
                                               ExistingIdentifiers);
      }
    }
    for (StructType *ST : M.getIdentifiedStructTypes()) {
      if (!ST->hasName() ||
          !shouldRewriteMangledName(ST->getName(), TargetModules)) {
        continue;
      }
      collectIdentifiersFromSwiftMangledName(ST->getName(),
                                             ExistingIdentifiers);
    }

    bool Changed = false;
    for (Function &F : M) {
      if (!F.hasName() || !shouldRewriteMangledName(F.getName(), TargetModules)) {
        continue;
      }
      std::string OldName = F.getName().str();
      std::string NewName = OldName;
      if (!rewriteSwiftMangledName(NewName, Seed, IdentifierMap) ||
          NewName == OldName) {
        continue;
      }
      F.setName(NewName);
      Changed = true;
    }
    for (GlobalVariable &GV : M.globals()) {
      if (GV.hasName() && GV.hasInitializer() &&
          shouldRewriteMangledName(GV.getName(), TargetModules)) {
        std::string OldName = GV.getName().str();
        std::string NewName = OldName;
        if (rewriteSwiftMangledName(NewName, Seed, IdentifierMap) &&
            NewName != OldName) {
          GV.setName(NewName);
          Changed = true;
        }
      }
      if (!GV.hasInitializer()) {
        continue;
      }
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString()) {
        continue;
      }
      std::string OldString = CDS->getAsCString().str();
      if (!isLikelySwiftMetadataString(GV, OldString)) {
        continue;
      }
      if (!shouldRewriteMangledName(OldString, TargetModules)) {
        continue;
      }
      std::string NewString = OldString;
      if (!rewriteSwiftMangledName(NewString, Seed, IdentifierMap) ||
          NewString == OldString) {
        continue;
      }
      if (tryRewriteCStringInitializer(GV, M, NewString)) {
        Changed = true;
      }
    }
    for (GlobalAlias &GA : M.aliases()) {
      if (!GA.hasName() ||
          !shouldRewriteMangledName(GA.getName(), TargetModules)) {
        continue;
      }
      std::string OldName = GA.getName().str();
      std::string NewName = OldName;
      if (!rewriteSwiftMangledName(NewName, Seed, IdentifierMap) ||
          NewName == OldName) {
        continue;
      }
      GA.setName(NewName);
      Changed = true;
    }
    for (StructType *ST : M.getIdentifiedStructTypes()) {
      if (!ST->hasName() ||
          !shouldRewriteMangledName(ST->getName(), TargetModules)) {
        continue;
      }
      std::string OldName = ST->getName().str();
      std::string NewName = OldName;
      if (!rewriteSwiftMangledName(NewName, Seed, IdentifierMap) ||
          NewName == OldName) {
        continue;
      }
      ST->setName(NewName);
      Changed = true;
    }

    errs() << "[SwiftSymbolObfuscation] obfuscated identifier count: "
           << IdentifierMap.size() << "\n";
    unsigned Printed = 0;
    for (const auto &KV : IdentifierMap) {
      if (Printed >= 120) {
        errs() << "[SwiftSymbolObfuscation] ... truncated ...\n";
        break;
      }
      errs() << "[SwiftSymbolObfuscation] " << KV.first << " -> " << KV.second
             << "\n";
      ++Printed;
    }
    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createSwiftSymbolObfuscationPass(bool flag) {
  return new SwiftSymbolObfuscation(flag);
}

char SwiftSymbolObfuscation::ID = 0;
INITIALIZE_PASS(SwiftSymbolObfuscation, "swiftsymobf", "Rename Swift Symbols.",
                false, false)
