#ifndef _METADATA_STRING_OBFUSCATION_H_
#define _METADATA_STRING_OBFUSCATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createMetadataStringObfuscationPass(bool flag);
void initializeMetadataStringObfuscationPass(PassRegistry &Registry);

} // namespace llvm

#endif
