// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
/*
  Hikari 's own "Pass Scheduler".
  Because currently there is no way to add dependency to transform passes
  Ref : http://lists.llvm.org/pipermail/llvm-dev/2011-February/038109.html
*/
#include "llvm/Transforms/Obfuscation/Obfuscation.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include <cstdlib>

using namespace llvm;

// Begin Obfuscator Options
static cl::opt<bool>
    EnableIRObfusaction("hikari", cl::init(false), cl::NotHidden,
                        cl::desc("Enable IR Code Obfuscation."),
                        cl::ZeroOrMore);
static cl::opt<uint64_t> AesSeed("aesSeed", cl::init(0x1337),
                                 cl::desc("seed for the PRNG"));
static cl::opt<bool> EnableAntiClassDump("enable-acdobf", cl::init(false),
                                         cl::NotHidden,
                                         cl::desc("Enable AntiClassDump."));
static cl::opt<bool> EnableAntiHooking("enable-antihook", cl::init(false),
                                       cl::NotHidden,
                                       cl::desc("Enable AntiHooking."));
static cl::opt<bool> EnableAntiDebugging("enable-adb", cl::init(false),
                                         cl::NotHidden,
                                         cl::desc("Enable AntiDebugging."));
static cl::opt<bool>
    EnableBogusControlFlow("enable-bcfobf", cl::init(false), cl::NotHidden,
                           cl::desc("Enable BogusControlFlow."));
static cl::opt<bool> EnableFlattening("enable-cffobf", cl::init(false),
                                      cl::NotHidden,
                                      cl::desc("Enable Flattening."));
static cl::opt<bool>
    EnableBasicBlockSplit("enable-splitobf", cl::init(false), cl::NotHidden,
                          cl::desc("Enable BasicBlockSpliting."));
static cl::opt<bool>
    EnableSubstitution("enable-subobf", cl::init(false), cl::NotHidden,
                       cl::desc("Enable Instruction Substitution."));
static cl::opt<bool> EnableAllObfuscation("enable-allobf", cl::init(false),
                                          cl::NotHidden,
                                          cl::desc("Enable All Obfuscation."));
static cl::opt<bool> EnableFunctionCallObfuscate(
    "enable-fco", cl::init(false), cl::NotHidden,
    cl::desc("Enable Function CallSite Obfuscation."));
static cl::opt<bool>
    EnableStringEncryption("enable-strcry", cl::init(false), cl::NotHidden,
                           cl::desc("Enable String Encryption."));
static cl::opt<bool>
    EnableConstantEncryption("enable-constenc", cl::init(false), cl::NotHidden,
                             cl::desc("Enable Constant Encryption."));
static cl::opt<bool>
    EnableIndirectBranching("enable-indibran", cl::init(false), cl::NotHidden,
                            cl::desc("Enable Indirect Branching."));
static cl::opt<bool>
    EnableFunctionNameObfuscation("enable-fnnameobf", cl::init(false),
                                  cl::NotHidden,
                                  cl::desc("Enable Function Name Obfuscation."));
static cl::opt<bool>
    EnableClassNameObfuscation("enable-clsobf", cl::init(false),
                               cl::NotHidden,
                               cl::desc("Enable Class Name Obfuscation."));
static cl::opt<bool>
    EnableObjCSelectorObfuscation("enable-objcselobf", cl::init(false),
                                  cl::NotHidden,
                                  cl::desc("Enable ObjC Selector Obfuscation."));
static cl::opt<bool>
    EnableObjCPropertyObfuscation("enable-objcpropobf", cl::init(false),
                                  cl::NotHidden,
                                  cl::desc("Enable ObjC Property Obfuscation."));
static cl::opt<bool>
    EnableSwiftSymbolObfuscation("enable-swiftsymobf", cl::init(false),
                                 cl::NotHidden,
                                 cl::desc("Enable Swift Symbol Obfuscation."));
static cl::opt<bool>
    EnableMetadataStringObfuscation("enable-metastrobf", cl::init(false),
                                    cl::NotHidden,
                                    cl::desc("Enable Metadata String Obfuscation."));
static cl::opt<bool>
    EnableSymbolVisibilityObfuscation("enable-symvisobf", cl::init(false),
                                      cl::NotHidden,
                                      cl::desc("Enable Symbol Visibility Obfuscation."));
static cl::opt<bool>
    EnableDependencyDiversification("enable-depdivobf", cl::init(false),
                                    cl::NotHidden,
                                    cl::desc("Enable Dependency Diversification."));
static cl::opt<bool>
    EnableFunctionWrapper("enable-funcwra", cl::init(false), cl::NotHidden,
                          cl::desc("Enable Function Wrapper."));
// End Obfuscator Options

static void LoadEnv(void) {
  if (getenv("SPLITOBF")) {
    EnableBasicBlockSplit = true;
  }
  if (getenv("SUBOBF")) {
    EnableSubstitution = true;
  }
  if (getenv("ALLOBF")) {
    EnableAllObfuscation = true;
  }
  if (getenv("FCO")) {
    EnableFunctionCallObfuscate = true;
  }
  if (getenv("STRCRY")) {
    EnableStringEncryption = true;
  }
  if (getenv("INDIBRAN")) {
    EnableIndirectBranching = true;
  }
  if (getenv("FNNAMEOBF")) {
    EnableFunctionNameObfuscation = true;
  }
  if (getenv("CLSOBF")) {
    EnableClassNameObfuscation = true;
  }
  if (getenv("OBJCSELOBF")) {
    EnableObjCSelectorObfuscation = true;
  }
  if (getenv("OBJCPROPOBF")) {
    EnableObjCPropertyObfuscation = true;
  }
  if (getenv("SWIFTSYMOBF")) {
    EnableSwiftSymbolObfuscation = true;
  }
  if (getenv("METASTROBF")) {
    EnableMetadataStringObfuscation = true;
  }
  if (getenv("SYMVISOBF")) {
    EnableSymbolVisibilityObfuscation = true;
  }
  if (getenv("DEPDIVOBF")) {
    EnableDependencyDiversification = true;
  }
  if (getenv("FUNCWRA")) {
    EnableFunctionWrapper = true; // Broken
  }
  if (getenv("BCFOBF")) {
    EnableBogusControlFlow = true;
  }
  if (getenv("ACDOBF")) {
    EnableAntiClassDump = true;
  }
  if (getenv("CFFOBF")) {
    EnableFlattening = true;
  }
  if (getenv("CONSTENC")) {
    EnableConstantEncryption = true;
  }
  if (getenv("ANTIHOOK")) {
    EnableAntiHooking = true;
  }
  if (getenv("ADB")) {
    EnableAntiDebugging = true;
  }
}
namespace llvm {
struct Obfuscation : public ModulePass {
  static char ID;
  Obfuscation() : ModulePass(ID) {
    initializeObfuscationPass(*PassRegistry::getPassRegistry());
  }
  StringRef getPassName() const override {
    return "HikariObfuscationScheduler";
  }
  bool runOnModule(Module &M) override {
    if (!EnableIRObfusaction)
      return 0;
    TimerGroup *tg =
        new TimerGroup("Obfuscation Timer Group", "Obfuscation Timer Group");
    Timer *timer = new Timer("Obfuscation Timer", "Obfuscation Timer", *tg);
    timer->startTimer();

    errs() << "Running Hikari On " << M.getSourceFileName() << "\n";

    annotation2Metadata(M);

    ModulePass *MP = createAntiHookPass(EnableAntiHooking);
    MP->doInitialization(M);
    MP->runOnModule(M);
    delete MP;
    // Initial ACD Pass
    if (EnableAllObfuscation || EnableAntiClassDump) {
      ModulePass *P = createAntiClassDumpPass();
      P->doInitialization(M);
      P->runOnModule(M);
      delete P;
    }
    // Now do FCO
    FunctionPass *FP = createFunctionCallObfuscatePass(
        EnableAllObfuscation || EnableFunctionCallObfuscate);
    for (Function &F : M)
      if (!F.isDeclaration())
        FP->runOnFunction(F);
    delete FP;
    MP = createAntiDebuggingPass(EnableAntiDebugging);
    MP->runOnModule(M);
    delete MP;
    // Now perform Function-Level Obfuscation
    for (Function &F : M)
      if (!F.isDeclaration()) {
        FunctionPass *P = nullptr;
        P = createSplitBasicBlockPass(EnableAllObfuscation ||
                                      EnableBasicBlockSplit);
        P->runOnFunction(F);
        delete P;
        P = createBogusControlFlowPass(EnableAllObfuscation ||
                                       EnableBogusControlFlow);
        P->runOnFunction(F);
        delete P;
        P = createFlatteningPass(EnableAllObfuscation || EnableFlattening);
        P->runOnFunction(F);
        delete P;
        P = createSubstitutionPass(EnableAllObfuscation || EnableSubstitution);
        P->runOnFunction(F);
        delete P;
      }
    MP = createConstantEncryptionPass(EnableConstantEncryption);
    MP->runOnModule(M);
    delete MP;
    errs() << "Doing Post-Run Cleanup\n";
    FunctionPass *P = createIndirectBranchPass(EnableAllObfuscation ||
                                               EnableIndirectBranching);
    for (Function &F : M)
      if (!F.isDeclaration())
        P->runOnFunction(F);
    delete P;
    MP = createFunctionWrapperPass(EnableAllObfuscation ||
                                   EnableFunctionWrapper);
    MP->runOnModule(M);
    delete MP;
    // Encrypt Strings AFTER all function-level passes (SplitBasicBlock,
    // BogusControlFlow, Flattening, Substitution, ConstantEncryption,
    // IndirectBranch, FunctionWrapper) to prevent them from modifying the
    // string decryption blocks, which would corrupt XOR decryption logic.
    MP = createStringEncryptionPass(EnableAllObfuscation ||
                                    EnableStringEncryption);
    MP->runOnModule(M);
    delete MP;
    MP = createFunctionNameObfuscationPass(EnableAllObfuscation ||
                                           EnableFunctionNameObfuscation);
    MP->runOnModule(M);
    delete MP;
    // Run selobf/propobf BEFORE clsobf: clsobf modifies __objc_classname GV
    // values, which invalidates use-chain analysis that selobf/propobf depend on.
    MP = createObjCSelectorObfuscationPass(EnableAllObfuscation ||
                                           EnableObjCSelectorObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createObjCPropertyObfuscationPass(EnableAllObfuscation ||
                                           EnableObjCPropertyObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createClassNameObfuscationPass(EnableAllObfuscation ||
                                        EnableClassNameObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createSwiftSymbolObfuscationPass(EnableAllObfuscation ||
                                          EnableSwiftSymbolObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createMetadataStringObfuscationPass(EnableAllObfuscation ||
                                             EnableMetadataStringObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createSymbolVisibilityObfuscationPass(EnableAllObfuscation ||
                                               EnableSymbolVisibilityObfuscation);
    MP->runOnModule(M);
    delete MP;
    MP = createDependencyDiversificationPass(
        EnableAllObfuscation || EnableDependencyDiversification);
    MP->runOnModule(M);
    delete MP;
    // Fix PHI nodes with duplicate entries for the same basic block.
    // Obfuscation passes (BogusControlFlow, IndirectBranch) can create
    // conditional branches where both targets are the same block, producing
    // PHI nodes with multiple entries from the same predecessor but different
    // SSA values. The LLVM verifier rejects this. Fix by setting duplicate
    // entries to use the same value as the first entry for that predecessor.
    for (Function &F : M) {
      if (F.isDeclaration())
        continue;
      for (BasicBlock &BB : F) {
        for (auto It = BB.begin(); It != BB.end(); ++It) {
          PHINode *PN = dyn_cast<PHINode>(&*It);
          if (!PN) break; // PHIs are always at the start of a BB
          SmallDenseMap<BasicBlock *, unsigned, 8> Seen;
          for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i) {
            BasicBlock *Pred = PN->getIncomingBlock(i);
            auto Res = Seen.try_emplace(Pred, i);
            if (!Res.second) {
              // Duplicate predecessor: set this entry's value to match the
              // first entry from the same predecessor.
              unsigned FirstIdx = Res.first->second;
              PN->setIncomingValue(i, PN->getIncomingValue(FirstIdx));
            }
          }
        }
      }
    }

    // Cleanup Flags
    SmallVector<Function *, 8> toDelete;
    for (Function &F : M)
      if (F.isDeclaration() && F.hasName() &&
#if LLVM_VERSION_MAJOR >= 18
          F.getName().starts_with("hikari_")) {
#else
          F.getName().startswith("hikari_")) {
#endif
        for (User *U : F.users())
          if (Instruction *Inst = dyn_cast<Instruction>(U))
            Inst->eraseFromParent();
        toDelete.emplace_back(&F);
      }
    for (Function *F : toDelete)
      F->eraseFromParent();

    timer->stopTimer();
    errs() << "Hikari Out\n";
    errs() << "Spend Time: "
           << format("%.7f", timer->getTotalTime().getWallTime()) << "s"
           << "\n";
    tg->clearAll();
    return true;
  } // End runOnModule
};
ModulePass *createObfuscationLegacyPass() {
  LoadEnv();
  if (AesSeed != 0x1337) {
    cryptoutils->prng_seed(AesSeed);
  } else {
    cryptoutils->prng_seed();
  }
  errs() << "Initializing Hikari Core with Revision ID:" << GIT_COMMIT_HASH
         << "\n";
  return new Obfuscation();
}

PreservedAnalyses ObfuscationPass::run(Module &M, ModuleAnalysisManager &MAM) {
  if (createObfuscationLegacyPass()->runOnModule(M)) {
    return PreservedAnalyses::none();
  }
  return PreservedAnalyses::all();
}

} // namespace llvm
char Obfuscation::ID = 0;
INITIALIZE_PASS_BEGIN(Obfuscation, "obfus", "Enable Obfuscation", false, false)
INITIALIZE_PASS_DEPENDENCY(AntiClassDump);
INITIALIZE_PASS_DEPENDENCY(BogusControlFlow);
INITIALIZE_PASS_DEPENDENCY(Flattening);
INITIALIZE_PASS_DEPENDENCY(FunctionCallObfuscate);
INITIALIZE_PASS_DEPENDENCY(IndirectBranch);
INITIALIZE_PASS_DEPENDENCY(FunctionNameObfuscation);
INITIALIZE_PASS_DEPENDENCY(ClassNameObfuscation);
INITIALIZE_PASS_DEPENDENCY(ObjCSelectorObfuscation);
INITIALIZE_PASS_DEPENDENCY(ObjCPropertyObfuscation);
INITIALIZE_PASS_DEPENDENCY(SwiftSymbolObfuscation);
INITIALIZE_PASS_DEPENDENCY(MetadataStringObfuscation);
INITIALIZE_PASS_DEPENDENCY(SymbolVisibilityObfuscation);
INITIALIZE_PASS_DEPENDENCY(DependencyDiversification);
INITIALIZE_PASS_DEPENDENCY(SplitBasicBlock);
INITIALIZE_PASS_DEPENDENCY(StringEncryption);
INITIALIZE_PASS_DEPENDENCY(Substitution);
INITIALIZE_PASS_END(Obfuscation, "obfus", "Enable Obfuscation", false, false)

#if LLVM_VERSION_MAJOR >= 18

namespace llvm {

PassPluginLibraryInfo getHikariPluginInfo() {
  return {
      LLVM_PLUGIN_API_VERSION, "Hikari", LLVM_VERSION_STRING,
      [](PassBuilder &PB) {
        PB.registerPipelineParsingCallback(
            [](StringRef Name, ModulePassManager &FPM,
               ArrayRef<PassBuilder::PipelineElement> InnerPipeline) {
              if (Name == EnableIRObfusaction.ArgStr) {
                EnableIRObfusaction = true;
                for (const auto &Element : InnerPipeline) {
                  if (Element.Name == EnableAntiClassDump.ArgStr) {
                    EnableAntiClassDump = true;
                  } else if (Element.Name == EnableAntiHooking.ArgStr) {
                    EnableAntiHooking = true;
                  } else if (Element.Name == EnableAntiDebugging.ArgStr) {
                    EnableAntiDebugging = true;
                  } else if (Element.Name == EnableBogusControlFlow.ArgStr) {
                    EnableBogusControlFlow = true;
                  } else if (Element.Name == EnableFlattening.ArgStr) {
                    EnableFlattening = true;
                  } else if (Element.Name == EnableBasicBlockSplit.ArgStr) {
                    EnableBasicBlockSplit = true;
                  } else if (Element.Name == EnableSubstitution.ArgStr) {
                    EnableSubstitution = true;
                  } else if (Element.Name == EnableAllObfuscation.ArgStr) {
                    EnableAllObfuscation = true;
                  } else if (Element.Name ==
                             EnableFunctionCallObfuscate.ArgStr) {
                    EnableFunctionCallObfuscate = true;
                  } else if (Element.Name == EnableStringEncryption.ArgStr) {
                    EnableStringEncryption = true;
                  } else if (Element.Name == EnableConstantEncryption.ArgStr) {
                    EnableConstantEncryption = true;
                  } else if (Element.Name == EnableIndirectBranching.ArgStr) {
                    EnableIndirectBranching = true;
                  } else if (Element.Name ==
                             EnableFunctionNameObfuscation.ArgStr) {
                    EnableFunctionNameObfuscation = true;
                  } else if (Element.Name ==
                             EnableClassNameObfuscation.ArgStr) {
                    EnableClassNameObfuscation = true;
                  } else if (Element.Name == EnableObjCSelectorObfuscation.ArgStr) {
                    EnableObjCSelectorObfuscation = true;
                  } else if (Element.Name == EnableObjCPropertyObfuscation.ArgStr) {
                    EnableObjCPropertyObfuscation = true;
                  } else if (Element.Name == EnableSwiftSymbolObfuscation.ArgStr) {
                    EnableSwiftSymbolObfuscation = true;
                  } else if (Element.Name ==
                             EnableMetadataStringObfuscation.ArgStr) {
                    EnableMetadataStringObfuscation = true;
                  } else if (Element.Name ==
                             EnableSymbolVisibilityObfuscation.ArgStr) {
                    EnableSymbolVisibilityObfuscation = true;
                  } else if (Element.Name ==
                             EnableDependencyDiversification.ArgStr) {
                    EnableDependencyDiversification = true;
                  } else if (Element.Name == EnableFunctionWrapper.ArgStr) {
                    EnableFunctionWrapper = true;
                  }
                }

                FPM.addPass(ObfuscationPass());
                return true;
              } else {
                return false;
              }
            });
      }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getHikariPluginInfo();
}

} // namespace llvm

#endif
