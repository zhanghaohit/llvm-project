// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/MetadataStringObfuscation.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
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

static cl::opt<std::string> MetadataStringObfuscationSeed(
    "metastrobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic MetadataStringObfuscation. "
             "Use the same seed across all translation units in one build."));

bool isAllowedMetadataChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == ':' ||
         C == '.' || C == '$';
}

bool looksLikeMetadataString(StringRef S) {
  if (S.empty() || S.size() < 4 || S.size() > 160) {
    return false;
  }
  bool HasAlphabet = false;
  for (char C : S) {
    if (!isAllowedMetadataChar(C)) {
      return false;
    }
    HasAlphabet |= std::isalpha(static_cast<unsigned char>(C));
  }
  return HasAlphabet;
}

bool isStringBlacklisted(StringRef S) {
  if (S.contains("http") || S.contains("https") || S.contains("://") ||
      S.contains(".com") || S.contains(".cn") || S.contains(".net") ||
      S.contains(".org") || S.contains(".framework") || S.contains("UIKit") ||
      S.contains("Foundation") || S.contains("ObjectiveC") ||
      S.contains("SwiftUI") || S.contains("libswift")) {
    return true;
  }
  static const std::unordered_set<std::string> ExactBlacklist = {
      "initialize",          "load",             "dealloc",
      "retain",              "release",          "autorelease",
      "class",               "superclass",       "hash",
      "description",         "debugDescription", "isEqual:",
      "isKindOfClass:",      "respondsToSelector:",
      "conformsToProtocol:", "main"};
  return ExactBlacklist.count(S.str()) != 0;
}

// Check if any user (transitively through constants) is a GV in an __objc*
// or __swift* section. If so, renaming this string would break ObjC/Swift
// runtime metadata.
bool hasObjCMetadataUser(const GlobalVariable &GV) {
  SmallPtrSet<const Value *, 32> Visited;
  SmallVector<const Value *, 16> Worklist;
  Worklist.push_back(&GV);
  while (!Worklist.empty()) {
    const Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;
    for (const User *U : V->users()) {
      if (const auto *UserGV = dyn_cast<GlobalVariable>(U)) {
        StringRef Sec = UserGV->getSection();
        if (Sec.contains("__objc") || Sec.contains("__swift"))
          return true;
        // Check GV name patterns for ObjC metadata structures
        StringRef N = UserGV->getName();
        if (N.contains("OBJC_") || N.contains("_OBJC_"))
          return true;
      }
      if (isa<Constant>(U) && !isa<GlobalVariable>(U)) {
        Worklist.push_back(U);
      }
    }
  }
  return false;
}

bool shouldProcessStringGV(const GlobalVariable &GV, StringRef S) {
  StringRef Section = GV.getSection();
  StringRef Name = GV.getName();

  // NEVER rename method names (selectors), method type encodings, or class
  // names. Selectors and types are used by the ObjC runtime for dispatch and
  // must match system framework methods. Class names are referenced by
  // categories, protocols, and NSClassFromString-style lookups; renaming them
  // here without updating all cross-references causes "class 'nil' not linked"
  // runtime errors.
  if (Section.contains("__objc_methname") ||
      Section.contains("__objc_methtype") ||
      Section.contains("__objc_classname")) {
    return false;
  }

  // Skip any string in an __objc* section — these are runtime metadata.
  if (Section.contains("__objc")) {
    return false;
  }

  // Only process GVs that look like metadata references
  bool IsCandidate = Section.contains("__swift") ||
                     Name.contains("OBJC_") || Name.contains(".str.") ||
                     S.contains("$s") || S.contains("_Tt");
  if (!IsCandidate)
    return false;

  // Safety: reject if any user chain leads to ObjC/Swift metadata structures
  if (hasObjCMetadataUser(GV))
    return false;

  return true;
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

std::string getMetadataStringSeed() {
  if (!MetadataStringObfuscationSeed.empty()) {
    return MetadataStringObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("METASTROBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-metastrobf-default-seed";
}

std::string obfuscateWithPreservedDelimiters(StringRef OldString, StringRef Seed,
                                             unsigned Attempt) {
  static constexpr char FirstCharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

  std::string Entropy = buildEntropyHex("hikari.metastrobf.string", Seed,
                                        OldString, Attempt,
                                        OldString.size() * 2 + 2);
  std::string NewString = OldString.str();
  size_t EntropyPos = 0;
  for (size_t I = 0; I < NewString.size(); ++I) {
    const char Old = OldString[I];
    if (Old == ':' || Old == '.' || Old == '$') {
      NewString[I] = Old;
      continue;
    }
    if (!std::isalnum(static_cast<unsigned char>(Old)) && Old != '_') {
      NewString[I] = Old;
      continue;
    }
    const bool NeedHeadChar = (I == 0 || OldString[I - 1] == ':' ||
                               OldString[I - 1] == '.' ||
                               OldString[I - 1] == '$');
    const char *PickSet = NeedHeadChar ? FirstCharSet : CharSet;
    const size_t PickSize =
        NeedHeadChar ? sizeof(FirstCharSet) - 1 : sizeof(CharSet) - 1;
    const unsigned Byte =
        (hexToNibble(Entropy[EntropyPos]) << 4) |
        hexToNibble(Entropy[EntropyPos + 1]);
    EntropyPos += 2;
    NewString[I] = PickSet[Byte % PickSize];
  }
  return NewString;
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
struct MetadataStringObfuscation : public ModulePass {
  static char ID;
  bool flag;
  MetadataStringObfuscation() : ModulePass(ID) { this->flag = true; }
  MetadataStringObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override {
    return "MetadataStringObfuscation";
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

    // Skip Swift modules — renaming __swift section strings and $s/$Tt metadata
    // breaks Swift runtime type resolution, protocol conformance, and
    // constraint-based layout (e.g. SnapKit Invalid constraint crash).
    if (isSwiftModule(M)) {
      errs() << "[MetadataStringObfuscation] Skipping Swift module: "
             << M.getSourceFileName() << "\n";
      return false;
    }

    errs() << "Running MetadataStringObfuscation pass on "
           << M.getSourceFileName() << "\n";
    const std::string Seed = getMetadataStringSeed();
    errs() << "[MetadataStringObfuscation] seed: " << Seed << "\n";

    std::unordered_map<std::string, std::string> RewriteMap;

    bool Changed = false;
    for (GlobalVariable &GV : M.globals()) {
      if (!GV.hasInitializer()) {
        continue;
      }
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString()) {
        continue;
      }
      std::string OldString = CDS->getAsCString().str();
      if (!shouldProcessStringGV(GV, OldString) ||
          !looksLikeMetadataString(OldString) || isStringBlacklisted(OldString)) {
        continue;
      }

      if (RewriteMap.count(OldString) == 0) {
        std::string NewString;
        for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
          NewString = obfuscateWithPreservedDelimiters(OldString, Seed, Attempt);
          if (NewString != OldString && !isStringBlacklisted(NewString)) {
            break;
          }
          NewString.clear();
        }
        if (NewString.empty()) {
          continue;
        }
        RewriteMap.emplace(OldString, NewString);
      }

      const std::string &NewString = RewriteMap[OldString];
      if (tryRewriteCStringInitializer(GV, M, NewString)) {
        Changed = true;
      }
    }

    errs() << "[MetadataStringObfuscation] rewritten string count: "
           << RewriteMap.size() << "\n";
    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createMetadataStringObfuscationPass(bool flag) {
  return new MetadataStringObfuscation(flag);
}

char MetadataStringObfuscation::ID = 0;
INITIALIZE_PASS(MetadataStringObfuscation, "metastrobf",
                "Obfuscate Objective-C/Swift Metadata Strings.", false, false)
