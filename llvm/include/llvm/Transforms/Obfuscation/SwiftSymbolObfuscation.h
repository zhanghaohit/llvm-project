#ifndef _SWIFT_SYMBOL_OBFUSCATION_H_
#define _SWIFT_SYMBOL_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createSwiftSymbolObfuscationPass(bool flag);
void initializeSwiftSymbolObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
