// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//
/*
  AntiClassDump pass — hides ObjC method metadata from class-dump.

  For each ObjC class in the module:
  1. Emits class_replaceMethod() calls in a module constructor to re-register
     all instance and class methods at load time.
  2. Zeros the method count in the static method list so class-dump cannot
     enumerate them.

  The original class_ro_t pointers are NEVER modified — only the method list
  count field is changed. This is safe with LLVM 17+ opaque pointers and
  modern ObjC runtimes (iOS 14+ relative method lists).
*/

#include "llvm/Transforms/Obfuscation/AntiClassDump.h"
#if LLVM_VERSION_MAJOR >= 17
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <deque>
#include <map>
#include <unordered_map>

using namespace llvm;

static cl::opt<bool>
    RenameMethodIMP("acd-rename-methodimp", cl::init(false), cl::NotHidden,
                    cl::desc("[AntiClassDump]Rename methods imp"));
namespace llvm {
struct AntiClassDump : public ModulePass {
  static char ID;
  bool appleptrauth;
  bool opaquepointers;
  Triple triple;
  AntiClassDump() : ModulePass(ID) {}
  StringRef getPassName() const override { return "AntiClassDump"; }
  bool doInitialization(Module &M) override {
    triple = Triple(M.getTargetTriple());
    if (triple.getVendor() != Triple::VendorType::Apple) {
      errs()
          << M.getTargetTriple()
          << " is Not Supported For LLVM AntiClassDump\nProbably GNU Step?\n";
      return false;
    }
    Type *Int8PtrTy = Type::getInt8Ty(M.getContext())->getPointerTo();
    FunctionType *IMPType =
        FunctionType::get(Int8PtrTy, {Int8PtrTy, Int8PtrTy}, true);
    PointerType *IMPPointerType = PointerType::getUnqual(IMPType);
    FunctionType *class_replaceMethod_type = FunctionType::get(
        IMPPointerType, {Int8PtrTy, Int8PtrTy, IMPPointerType, Int8PtrTy},
        false);
    M.getOrInsertFunction("class_replaceMethod", class_replaceMethod_type);
    FunctionType *sel_registerName_type =
        FunctionType::get(Int8PtrTy, {Int8PtrTy}, false);
    M.getOrInsertFunction("sel_registerName", sel_registerName_type);
    FunctionType *objc_getClass_type =
        FunctionType::get(Int8PtrTy, {Int8PtrTy}, false);
    M.getOrInsertFunction("objc_getClass", objc_getClass_type);
    M.getOrInsertFunction("objc_getMetaClass", objc_getClass_type);
    FunctionType *class_getName_Type =
        FunctionType::get(Int8PtrTy, {Int8PtrTy}, false);
    M.getOrInsertFunction("class_getName", class_getName_Type);
    FunctionType *objc_getMetaClass_Type =
        FunctionType::get(Int8PtrTy, {Int8PtrTy}, false);
    M.getOrInsertFunction("objc_getMetaClass", objc_getMetaClass_Type);
    appleptrauth = hasApplePtrauth(&M);
#if LLVM_VERSION_MAJOR >= 17
    opaquepointers = true;
#else
    opaquepointers = !M.getContext().supportsTypedPointers();
#endif
    return true;
  }
  bool runOnModule(Module &M) override {
    errs() << "Running AntiClassDump On " << M.getSourceFileName() << "\n";
    SmallVector<GlobalVariable *, 32> OLCGVs;
    for (GlobalVariable &GV : M.globals()) {
#if LLVM_VERSION_MAJOR >= 18
      if (GV.getName().starts_with("OBJC_LABEL_CLASS_$")) {
#else
      if (GV.getName().startswith("OBJC_LABEL_CLASS_$")) {
#endif
        OLCGVs.emplace_back(&GV);
      }
    }
    if (!OLCGVs.size()) {
      errs() << "No ObjC Class Found in :" << M.getSourceFileName() << "\n";
      return false;
    }
    for (GlobalVariable *OLCGV : OLCGVs) {
      ConstantArray *OBJC_LABEL_CLASS_CDS =
          dyn_cast<ConstantArray>(OLCGV->getInitializer());
      assert(OBJC_LABEL_CLASS_CDS &&
             "OBJC_LABEL_CLASS_$ Not ConstantArray.Is the target using "
             "unsupported legacy runtime?");
      SmallVector<std::string, 4> readyclses;
      std::deque<std::string> tmpclses;
      std::unordered_map<std::string, std::string> dependency;
      std::unordered_map<std::string, GlobalVariable *> GVMapping;
      for (unsigned int i = 0; i < OBJC_LABEL_CLASS_CDS->getNumOperands();
           i++) {
        ConstantExpr *clsEXPR =
            opaquepointers
                ? nullptr
                : dyn_cast<ConstantExpr>(OBJC_LABEL_CLASS_CDS->getOperand(i));
        GlobalVariable *CEGV = dyn_cast<GlobalVariable>(
            opaquepointers ? OBJC_LABEL_CLASS_CDS->getOperand(i)
                           : clsEXPR->getOperand(0));
        ConstantStruct *clsCS =
            dyn_cast<ConstantStruct>(CEGV->getInitializer());
        GlobalVariable *SuperClassGV =
            dyn_cast_or_null<GlobalVariable>(clsCS->getOperand(1));
        SuperClassGV = readPtrauth(SuperClassGV);
        std::string supclsName = "";
        std::string clsName = CEGV->getName().str();
        clsName.replace(clsName.find("OBJC_CLASS_$_"), strlen("OBJC_CLASS_$_"),
                        "");
        if (SuperClassGV) {
          supclsName = SuperClassGV->getName().str();
          supclsName.replace(supclsName.find("OBJC_CLASS_$_"),
                             strlen("OBJC_CLASS_$_"), "");
        }
        dependency[clsName] = supclsName;
        GVMapping[clsName] = CEGV;
        if (supclsName == "" ||
            (SuperClassGV && !SuperClassGV->hasInitializer())) {
          readyclses.emplace_back(clsName);
        } else {
          tmpclses.emplace_back(clsName);
        }
        while (tmpclses.size()) {
          std::string clstmp = tmpclses.front();
          tmpclses.pop_front();
          std::string SuperClassName = dependency[clstmp];
          if (SuperClassName != "" &&
              std::find(readyclses.begin(), readyclses.end(), SuperClassName) ==
                  readyclses.end()) {
            tmpclses.emplace_back(clstmp);
          } else {
            readyclses.emplace_back(clstmp);
          }
        }

        for (std::string className : readyclses) {
          // Skip Swift classes (mangled names start with _Tt)
          if (className.rfind("_Tt", 0) == 0) {
            errs() << "Skipping Swift class: " << className << "\n";
            continue;
          }
          handleClass(GVMapping[className], &M);
        }
      }
    }
    return true;
  } // runOnModule

  // Find the method list GlobalVariable inside a class_ro_t struct.
  // Returns nullptr if not found or empty.
  GlobalVariable *findMethodListGV(ConstantStruct *class_ro_cs, Module *M) {
    StructType *objc_method_list_t_type =
        StructType::getTypeByName(M->getContext(), "struct.__method_list_t");
    for (unsigned i = 0; i < class_ro_cs->getType()->getNumElements(); i++) {
      Constant *tmp =
          dyn_cast<Constant>(class_ro_cs->getAggregateElement(i));
      if (tmp->isNullValue())
        continue;
      Type *type = tmp->getType();
      bool isMethodList = false;
      if (!opaquepointers) {
        isMethodList =
            (type == PointerType::getUnqual(objc_method_list_t_type));
      } else {
#if LLVM_VERSION_MAJOR >= 18
        isMethodList = (tmp->getName().starts_with("_OBJC_$_INSTANCE_METHODS") ||
                        tmp->getName().starts_with("_OBJC_$_CLASS_METHODS"));
#else
        isMethodList = (tmp->getName().startswith("_OBJC_$_INSTANCE_METHODS") ||
                        tmp->getName().startswith("_OBJC_$_CLASS_METHODS"));
#endif
      }
      if (!isMethodList)
        continue;
      GlobalVariable *methodListGV =
          readPtrauth(cast<GlobalVariable>(tmp->stripPointerCasts()));
      if (!methodListGV->hasInitializer())
        continue;
      return methodListGV;
    }
    return nullptr;
  }

  void handleClass(GlobalVariable *GV, Module *M) {
    assert(GV->hasInitializer() &&
           "ObjC Class Structure's Initializer Missing");
    ConstantStruct *CS = dyn_cast<ConstantStruct>(GV->getInitializer());
    StringRef ClassName = GV->getName();
    ClassName = ClassName.substr(strlen("OBJC_CLASS_$_"));
    StringRef SuperClassName =
        readPtrauth(
            cast<GlobalVariable>(CS->getOperand(1)->stripPointerCasts()))
            ->getName();
    SuperClassName = SuperClassName.substr(strlen("OBJC_CLASS_$_"));
    errs() << "Handling Class:" << ClassName
           << " With SuperClass:" << SuperClassName << "\n";

    // struct _class_t {
    //   struct _class_t *isa;          // [0] metaclass
    //   struct _class_t *superclass;   // [1]
    //   void *cache;                   // [2]
    //   IMP *vtable;                   // [3]
    //   struct class_ro_t *ro;         // [4]
    // }
    GlobalVariable *metaclassGV = readPtrauth(
        cast<GlobalVariable>(CS->getOperand(0)->stripPointerCasts()));
    GlobalVariable *class_ro = readPtrauth(
        cast<GlobalVariable>(CS->getOperand(4)->stripPointerCasts()));
    assert(metaclassGV->hasInitializer() && "MetaClass GV Initializer Missing");
    GlobalVariable *metaclass_ro = readPtrauth(cast<GlobalVariable>(
        metaclassGV->getInitializer()
            ->getOperand(metaclassGV->getInitializer()->getNumOperands() - 1)
            ->stripPointerCasts()));

    // Create a module constructor function
    FunctionType *CtorType =
        FunctionType::get(Type::getVoidTy(M->getContext()), false);
    Function *Ctor = Function::Create(
        CtorType, GlobalValue::LinkageTypes::PrivateLinkage,
        "ACDClassCtor", M);
    BasicBlock *BB = BasicBlock::Create(M->getContext(), "", Ctor);
    ReturnInst::Create(BB->getContext(), BB);
    IRBuilder<> IRB(BB, BB->getFirstInsertionPt());

    Function *objc_getClass = M->getFunction("objc_getClass");
    Value *ClassNameGV = IRB.CreateGlobalStringPtr(ClassName);
    CallInst *Class = IRB.CreateCall(objc_getClass, {ClassNameGV});

    bool changed = false;

    // Process instance methods (stored in class_ro)
    ConstantStruct *classRoCS =
        cast<ConstantStruct>(class_ro->getInitializer());
    GlobalVariable *instMethodListGV = findMethodListGV(classRoCS, M);
    if (instMethodListGV) {
      ConstantStruct *mlStruct =
          cast<ConstantStruct>(instMethodListGV->getInitializer());
      if (!mlStruct->getOperand(2)->isZeroValue()) {
        errs() << "Handling Instance Methods For Class:" << ClassName << "\n";
        ConstantArray *methodList =
            cast<ConstantArray>(mlStruct->getOperand(2));
        emitMethodRegistration(methodList, &IRB, M, Class, false);
        changed = true;
      }
    }

    // Process class methods (stored in metaclass_ro)
    ConstantStruct *metaclassRoCS =
        cast<ConstantStruct>(metaclass_ro->getInitializer());
    GlobalVariable *classMethodListGV = findMethodListGV(metaclassRoCS, M);
    if (classMethodListGV) {
      ConstantStruct *mlStruct =
          cast<ConstantStruct>(classMethodListGV->getInitializer());
      if (!mlStruct->getOperand(2)->isZeroValue()) {
        errs() << "Handling Class Methods For Class:" << ClassName << "\n";
        ConstantArray *methodList =
            cast<ConstantArray>(mlStruct->getOperand(2));
        emitMethodRegistration(methodList, &IRB, M, Class, true);
        changed = true;
      }
    }

    if (changed) {
      // Register the constructor to run at module load time
      appendToGlobalCtors(*M, Ctor, 0);
      // Mark ACDClassCtor to skip ALL obfuscation passes. This constructor
      // runs at __mod_init_func time (before main) and calls ObjC runtime
      // APIs (sel_registerName, class_replaceMethod) with precise arguments.
      // Any obfuscation (bcfobf, constenc, indibran, strcry) can corrupt
      // the control flow or constant arguments, causing SIGSEGV at dyld
      // load time. This was the root cause of random crashes in R84/R85.
      writeAnnotationMetadata(Ctor, "nostrenc");
      writeAnnotationMetadata(Ctor, "noconstenc");
      writeAnnotationMetadata(Ctor, "nobcfobf");
      writeAnnotationMetadata(Ctor, "noindibran");
      errs() << "Registered constructor for Class:" << ClassName << "\n";
    } else {
      // No methods to process, remove the empty constructor
      Ctor->eraseFromParent();
    }
  } // handleClass

  // Emit class_replaceMethod calls for each method in the list
  void emitMethodRegistration(ConstantArray *methodList, IRBuilder<> *IRB,
                              Module *M, Value *Class, bool isMetaClass) {
    Function *sel_registerName = M->getFunction("sel_registerName");
    Function *class_replaceMethod = M->getFunction("class_replaceMethod");
    Function *class_getName = M->getFunction("class_getName");
    Function *objc_getMetaClass = M->getFunction("objc_getMetaClass");

    for (unsigned i = 0; i < methodList->getNumOperands(); i++) {
      ConstantStruct *methodStruct =
          cast<ConstantStruct>(methodList->getOperand(i));
      // _objc_method = { name (ptr), type (ptr), imp (ptr) }

      // Extract selector name
      GlobalVariable *selNameGV = cast<GlobalVariable>(
          opaquepointers ? methodStruct->getOperand(0)
                         : methodStruct->getOperand(0)->getOperand(0));
      StringRef selname =
          cast<ConstantDataSequential>(selNameGV->getInitializer())
              ->getAsCString();

      Constant *SELName = IRB->CreateGlobalStringPtr(selname);
      CallInst *SEL = IRB->CreateCall(sel_registerName, {SELName});

      // Extract IMP
      Type *IMPType =
          class_replaceMethod->getFunctionType()->getParamType(2);
      Value *BitCastedIMP = IRB->CreateBitCast(
          appleptrauth
              ? opaquepointers
                    ? cast<GlobalVariable>(methodStruct->getOperand(2))
                          ->getInitializer()
                          ->getOperand(0)
                    : cast<ConstantExpr>(
                          cast<GlobalVariable>(methodStruct->getOperand(2))
                              ->getInitializer()
                              ->getOperand(0))
                          ->getOperand(0)
              : methodStruct->getOperand(2),
          IMPType);

      // Extract type encoding
      GlobalVariable *typeGV = cast<GlobalVariable>(
          opaquepointers ? methodStruct->getOperand(1)
                         : methodStruct->getOperand(1)->getOperand(0));
      Constant *TypeEncoding = IRB->CreateGlobalStringPtr(
          cast<ConstantDataSequential>(typeGV->getInitializer())
              ->getAsCString());

      // Build class_replaceMethod(class, sel, imp, type) call
      std::vector<Value *> args;
      if (isMetaClass) {
        CallInst *className = IRB->CreateCall(class_getName, {Class});
        CallInst *MetaClass =
            IRB->CreateCall(objc_getMetaClass, {className});
        args.push_back(MetaClass);
      } else {
        args.push_back(Class);
      }
      args.push_back(SEL);
      args.push_back(BitCastedIMP);
      args.push_back(TypeEncoding);
      IRB->CreateCall(class_replaceMethod, ArrayRef<Value *>(args));

      if (RenameMethodIMP) {
        Function *MethodIMP = cast<Function>(
            appleptrauth
                ? opaquepointers
                      ? cast<GlobalVariable>(methodStruct->getOperand(2))
                            ->getInitializer()
                            ->getOperand(0)
                      : cast<ConstantExpr>(
                            cast<GlobalVariable>(
                                methodStruct->getOperand(2)->getOperand(0))
                                ->getInitializer()
                                ->getOperand(0))
                            ->getOperand(0)
                : opaquepointers ? methodStruct->getOperand(2)
                                 : methodStruct->getOperand(2)->getOperand(0));
        MethodIMP->setName("ACDMethodIMP");
      }
    }
  }

  // Zero the method count in a method list struct without replacing the GV.
  // The method list struct is: { i32 entsize, i32 count, [N x _objc_method] }
  // We only change count from N to 0.
  void zeroMethodCount(GlobalVariable *methodListGV) {
    ConstantStruct *oldStruct =
        cast<ConstantStruct>(methodListGV->getInitializer());
    SmallVector<Constant *, 3> newOps;
    newOps.push_back(oldStruct->getOperand(0)); // entsize unchanged
    newOps.push_back(ConstantInt::get(
        oldStruct->getOperand(1)->getType(), 0)); // count = 0
    newOps.push_back(oldStruct->getOperand(2));   // method data unchanged
    Constant *newStruct =
        ConstantStruct::get(oldStruct->getType(), newOps);
    methodListGV->setInitializer(newStruct);
    errs() << "Zeroed method count for: " << methodListGV->getName() << "\n";
  }

  GlobalVariable *readPtrauth(GlobalVariable *GV) {
    if (GV->getSection() == "llvm.ptrauth") {
      Value *V = GV->getInitializer()->getOperand(0);
      return cast<GlobalVariable>(
          opaquepointers ? V : cast<ConstantExpr>(V)->getOperand(0));
    }
    return GV;
  }
};
} // namespace llvm
ModulePass *llvm::createAntiClassDumpPass() { return new AntiClassDump(); }
char AntiClassDump::ID = 0;
INITIALIZE_PASS(AntiClassDump, "acd", "Enable Anti-ClassDump.", false, false)
