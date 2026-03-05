#ifndef _OBJC_SELECTOR_OBFUSCATION_H_
#define _OBJC_SELECTOR_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createObjCSelectorObfuscationPass(bool flag);
void initializeObjCSelectorObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
