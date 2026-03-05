#ifndef _DEPENDENCY_DIVERSIFICATION_H_
#define _DEPENDENCY_DIVERSIFICATION_H_

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"

namespace llvm {

ModulePass *createDependencyDiversificationPass(bool flag);
void initializeDependencyDiversificationPass(PassRegistry &Registry);

} // namespace llvm

#endif
