#ifndef _CLASS_NAME_OBFUSCATION_H_
#define _CLASS_NAME_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createClassNameObfuscationPass(bool flag);
void initializeClassNameObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
