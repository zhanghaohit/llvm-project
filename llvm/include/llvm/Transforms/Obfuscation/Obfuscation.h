#ifndef _OBFUSCATION_H_
#define _OBFUSCATION_H_

#include "AntiClassDump.h"
#include "AntiDebugging.h"
#include "AntiHook.h"
#include "BogusControlFlow.h"
#include "ClassNameObfuscation.h"
#include "ConstantEncryption.h"
#include "CryptoUtils.h"
#include "DependencyDiversification.h"
#include "Flattening.h"
#include "FunctionCallObfuscate.h"
#include "FunctionNameObfuscation.h"
#include "FunctionWrapper.h"
#include "IndirectBranch.h"
#include "MetadataStringObfuscation.h"
#include "ObjCPropertyObfuscation.h"
#include "ObjCSelectorObfuscation.h"
#include "Split.h"
#include "StringEncryption.h"
#include "Substitution.h"
#include "SwiftSymbolObfuscation.h"
#include "SymbolVisibilityObfuscation.h"
#include "llvm/Support/Timer.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace llvm {

class ObfuscationPass : public PassInfoMixin<ObfuscationPass> {
public:
  ObfuscationPass() {}
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  static bool isRequired() { return true; }
};

ModulePass *createObfuscationLegacyPass();
void initializeObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
