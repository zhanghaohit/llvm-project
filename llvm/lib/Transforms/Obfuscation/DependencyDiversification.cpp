// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/DependencyDiversification.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#if defined(__has_include)
#if __has_include("llvm/TargetParser/Triple.h")
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif
#else
#include "llvm/ADT/Triple.h"
#endif
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <string>
#include <vector>

using namespace llvm;

namespace {

std::string randomToken(size_t Length) {
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
  std::string Out;
  Out.reserve(Length);
  for (size_t I = 0; I < Length; ++I) {
    Out.push_back(CharSet[cryptoutils->get_uint64_t() % (sizeof(CharSet) - 1)]);
  }
  return Out;
}

} // namespace

namespace llvm {
struct DependencyDiversification : public ModulePass {
  static char ID;
  bool flag;
  DependencyDiversification() : ModulePass(ID) { this->flag = true; }
  DependencyDiversification(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override {
    return "DependencyDiversification";
  }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    errs() << "Running DependencyDiversification pass on "
           << M.getSourceFileName() << "\n";

    Triple TripleInfo(M.getTargetTriple());
    if (TripleInfo.getVendor() != Triple::VendorType::Apple) {
      return false;
    }

    std::vector<GlobalValue *> NewGlobals;
    for (unsigned I = 0; I < 3; ++I) {
      std::string Framework = randomToken(10);
      std::string DylibPath = "@rpath/" + Framework + ".framework/" + Framework;
      Constant *Str = ConstantDataArray::getString(M.getContext(), DylibPath, true);
      auto *GV = new GlobalVariable(
          M, Str->getType(), true, GlobalValue::PrivateLinkage, Str,
          "__hikari_dep_noise_" + std::to_string(I));
      GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
      GV->setSection("__TEXT,__cstring,cstring_literals");
      NewGlobals.emplace_back(GV);
    }

    appendToCompilerUsed(M, NewGlobals);
    errs() << "[DependencyDiversification] injected dependency-like strings: "
           << NewGlobals.size() << "\n";
    return !NewGlobals.empty();
  }
};
} // namespace llvm

ModulePass *llvm::createDependencyDiversificationPass(bool flag) {
  return new DependencyDiversification(flag);
}

char DependencyDiversification::ID = 0;
INITIALIZE_PASS(DependencyDiversification, "depdivobf",
                "Inject Dependency-Like Noise Strings.", false, false)
