#ifndef _SYMBOL_VISIBILITY_OBFUSCATION_H_
#define _SYMBOL_VISIBILITY_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createSymbolVisibilityObfuscationPass(bool flag);
void initializeSymbolVisibilityObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
