#ifndef _OBJC_PROPERTY_OBFUSCATION_H_
#define _OBJC_PROPERTY_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createObjCPropertyObfuscationPass(bool flag);
void initializeObjCPropertyObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
