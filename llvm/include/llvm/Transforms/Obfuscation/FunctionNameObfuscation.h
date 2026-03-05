#ifndef _FUNCTION_NAME_OBFUSCATION_H_
#define _FUNCTION_NAME_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createFunctionNameObfuscationPass(bool flag);
void initializeFunctionNameObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
