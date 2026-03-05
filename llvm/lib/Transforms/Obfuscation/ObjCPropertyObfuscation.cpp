// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/ObjCPropertyObfuscation.h"
#if defined(__has_include)
#if __has_include("llvm/TargetParser/Triple.h")
#include "llvm/TargetParser/Triple.h"
#else
#include "llvm/ADT/Triple.h"
#endif
#else
#include "llvm/ADT/Triple.h"
#endif
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace llvm;

namespace {

static cl::opt<std::string> ObjCPropertyObfuscationSeed(
    "objcpropobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic ObjCPropertyObfuscation. "
             "Use the same seed across all translation units in one build."));

#if LLVM_VERSION_MAJOR >= 18
inline bool containsStr(StringRef Name, StringRef Substr) {
  return Name.contains(Substr);
}
#else
inline bool containsStr(StringRef Name, StringRef Substr) {
  return Name.contains(Substr);
}
#endif

bool isPropertyIdentifier(StringRef Name) {
  if (Name.empty() || Name.size() > 96) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
      Name.front() != '_') {
    return false;
  }
  for (char C : Name) {
    if (!std::isalnum(static_cast<unsigned char>(C)) && C != '_') {
      return false;
    }
  }
  return true;
}

bool isPropertyNameBlacklisted(StringRef Name) {
  static const std::unordered_set<std::string> Blacklist = {
      // NSObject fundamentals
      "class", "superclass", "hash", "description", "debugDescription",
      "retain", "release", "autorelease", "retainCount",
      "self", "isProxy", "copy", "mutableCopy", "new", "init",
      "dealloc", "zone", "version", "main",
      // UIView / UIViewController common properties
      "view", "title", "navigationItem", "navigationController",
      "tabBarItem", "tabBarController",
      "parentViewController", "presentedViewController",
      "presentingViewController", "childViewControllers",
      "storyboard", "nibName", "nibBundle",
      "modalPresentationStyle", "modalTransitionStyle",
      "isBeingPresented", "isBeingDismissed",
      "isMovingToParentViewController", "isMovingFromParentViewController",
      "edgesForExtendedLayout", "extendedLayoutIncludesOpaqueBars",
      "preferredContentSize",
      "definesPresentationContext", "providesPresentationContextTransitionStyle",
      // UIView properties
      "frame", "bounds", "center", "transform",
      "backgroundColor", "alpha", "opaque", "clipsToBounds",
      "hidden", "contentMode", "tag",
      "layer", "tintColor",
      "userInteractionEnabled", "multipleTouchEnabled",
      "autoresizingMask", "autoresizesSubviews",
      "superview", "subviews", "window",
      "constraints", "translatesAutoresizingMaskIntoConstraints",
      "layoutMargins", "preservesSuperviewLayoutMargins",
      "insetsLayoutMarginsFromSafeArea",
      "safeAreaInsets", "safeAreaLayoutGuide",
      "traitCollection",
      "semanticContentAttribute",
      "isFirstResponder",
      "intrinsicContentSize",
      // UIControl
      "enabled", "selected", "highlighted",
      "contentVerticalAlignment", "contentHorizontalAlignment",
      "state", "isTracking", "isTouchInside",
      // UILabel
      "text", "font", "textColor", "textAlignment",
      "numberOfLines", "lineBreakMode",
      "attributedText", "adjustsFontSizeToFitWidth",
      "minimumScaleFactor", "preferredMaxLayoutWidth",
      // UIButton
      "buttonType", "titleLabel", "imageView",
      "currentTitle", "currentTitleColor",
      "currentImage", "currentBackgroundImage",
      // UIImageView
      "image", "highlightedImage", "animationImages",
      "highlightedAnimationImages", "animationDuration",
      "animationRepeatCount", "isAnimating",
      // UITextField / UITextView
      "placeholder", "borderStyle", "clearButtonMode",
      "leftView", "rightView", "leftViewMode", "rightViewMode",
      "inputView", "inputAccessoryView",
      "delegate", "keyboardType", "returnKeyType",
      "secureTextEntry", "textContentType",
      "editable", "selectable", "dataDetectorTypes",
      "textContainer", "textContainerInset",
      "layoutManager", "textStorage",
      // UIScrollView
      "contentOffset", "contentSize", "contentInset",
      "scrollEnabled", "pagingEnabled",
      "bounces", "alwaysBounceVertical", "alwaysBounceHorizontal",
      "showsVerticalScrollIndicator", "showsHorizontalScrollIndicator",
      "scrollIndicatorInsets", "decelerationRate",
      "zoomScale", "minimumZoomScale", "maximumZoomScale",
      "bouncesZoom", "isZooming", "isZoomBouncing",
      "scrollsToTop", "directionalLockEnabled",
      "contentInsetAdjustmentBehavior",
      "adjustedContentInset",
      "refreshControl",
      // UITableView
      "dataSource", "tableHeaderView", "tableFooterView",
      "rowHeight", "estimatedRowHeight",
      "sectionHeaderHeight", "sectionFooterHeight",
      "estimatedSectionHeaderHeight", "estimatedSectionFooterHeight",
      "separatorStyle", "separatorColor", "separatorInset",
      "allowsSelection", "allowsMultipleSelection",
      "allowsSelectionDuringEditing",
      "indexPathForSelectedRow", "indexPathsForSelectedRows",
      "isEditing",
      // UICollectionView
      "collectionViewLayout",
      // UINavigationBar / UINavigationItem
      "topItem", "backItem", "items",
      "barTintColor", "shadowImage", "barStyle",
      "translucent", "prefersLargeTitles",
      "backBarButtonItem", "leftBarButtonItem", "rightBarButtonItem",
      "leftBarButtonItems", "rightBarButtonItems",
      "titleView", "largeTitleDisplayMode",
      "hidesBackButton", "backButtonTitle",
      // UITabBar / UITabBarItem
      "barTintColor", "selectedItem",
      "badgeValue", "badgeColor",
      // UISearchBar
      "searchTextField", "scopeButtonTitles",
      "showsCancelButton", "showsSearchResultsButton",
      // UISwitch
      "on", "onTintColor", "thumbTintColor",
      // UISlider
      "value", "minimumValue", "maximumValue",
      "minimumTrackTintColor", "maximumTrackTintColor",
      "thumbTintColor", "isContinuous",
      // UIActivityIndicatorView
      "animating", "hidesWhenStopped",
      // UIPageControl
      "numberOfPages", "currentPage",
      "hidesForSinglePage", "pageIndicatorTintColor",
      "currentPageIndicatorTintColor",
      // Gesture recognizers
      "gestureRecognizers",
      // UIApplication
      "keyWindow", "windows", "applicationState",
      "statusBarStyle", "statusBarHidden",
      "networkActivityIndicatorVisible",
      "applicationIconBadgeNumber",
      // UIDevice
      "name", "model", "systemName", "systemVersion",
      "identifierForVendor", "orientation",
      "batteryLevel", "batteryState", "batteryMonitoringEnabled",
      "proximityMonitoringEnabled", "proximityState",
      "isMultitaskingSupported",
      // Foundation
      "count", "length", "objectAtIndex:",
      "string", "intValue", "floatValue", "doubleValue", "boolValue",
      "integerValue", "unsignedIntegerValue",
      "allKeys", "allValues",
      // NSError
      "domain", "code", "userInfo", "localizedDescription",
      "localizedFailureReason", "localizedRecoverySuggestion",
      // Accessibility
      "isAccessibilityElement", "accessibilityLabel",
      "accessibilityHint", "accessibilityValue",
      "accessibilityTraits", "accessibilityFrame",
      "accessibilityIdentifier",
      "accessibilityLanguage",
      "shouldGroupAccessibilityChildren",
      // CALayer
      "cornerRadius", "borderWidth", "borderColor",
      "shadowColor", "shadowOffset", "shadowRadius", "shadowOpacity",
      "masksToBounds", "shadowPath",
      "anchorPoint", "position", "zPosition",
      "contentsScale", "rasterizationScale",
      "shouldRasterize",
  };
  return Blacklist.count(Name.str()) != 0;
}

// Check if a property attr global is referenced by any protocol-related global.
bool isUsedInProtocolContext(GlobalVariable &GV) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> Worklist;
  Worklist.push_back(&GV);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *UserGV = dyn_cast<GlobalVariable>(U)) {
        StringRef GVName = UserGV->getName();
        if (containsStr(GVName, "PROTOCOL"))
          return true;
      }
      if (isa<Constant>(U) && !isa<GlobalVariable>(U)) {
        Worklist.push_back(U);
      }
    }
  }
  return false;
}

unsigned hexToNibble(char C) {
  if (C >= '0' && C <= '9') {
    return static_cast<unsigned>(C - '0');
  }
  if (C >= 'a' && C <= 'f') {
    return static_cast<unsigned>(10 + C - 'a');
  }
  if (C >= 'A' && C <= 'F') {
    return static_cast<unsigned>(10 + C - 'A');
  }
  return 0;
}

std::string buildEntropyHex(StringRef Domain, StringRef Seed, StringRef Source,
                            unsigned Attempt, size_t MinChars) {
  std::string Entropy;
  Entropy.reserve(MinChars + 32);
  unsigned Block = 0;
  while (Entropy.size() < MinChars) {
    MD5 Hasher;
    Hasher.update(Domain);
    Hasher.update(Seed);
    Hasher.update(Source);
    Hasher.update(std::to_string(Attempt));
    Hasher.update(std::to_string(Block));
    MD5::MD5Result Digest;
    Hasher.final(Digest);
    SmallString<32> Hex;
    MD5::stringifyResult(Digest, Hex);
    Entropy.append(Hex.str().data(), Hex.str().size());
    ++Block;
  }
  return Entropy;
}

std::string getObjCPropertySeed() {
  if (!ObjCPropertyObfuscationSeed.empty()) {
    return ObjCPropertyObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("OBJCPROPOBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-objcpropobf-default-seed";
}

std::string generateSameLengthIdentifier(StringRef OldName, StringRef Seed,
                                         StringRef Domain, unsigned Attempt) {
  static constexpr char FirstCharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

  if (OldName.empty()) {
    return "_";
  }

  std::string Entropy =
      buildEntropyHex(Domain, Seed, OldName, Attempt, OldName.size() * 2 + 2);
  std::string NewName;
  NewName.reserve(OldName.size());
  const unsigned HeadByte =
      (hexToNibble(Entropy[0]) << 4) | hexToNibble(Entropy[1]);
  NewName.push_back(FirstCharSet[HeadByte % (sizeof(FirstCharSet) - 1)]);
  for (size_t I = 1; I < OldName.size(); ++I) {
    const size_t J = I * 2;
    const unsigned Byte =
        (hexToNibble(Entropy[J]) << 4) | hexToNibble(Entropy[J + 1]);
    NewName.push_back(CharSet[Byte % (sizeof(CharSet) - 1)]);
  }
  return NewName;
}

std::vector<std::string> splitCSV(StringRef S) {
  std::vector<std::string> Tokens;
  size_t Pos = 0;
  while (Pos <= S.size()) {
    size_t Comma = S.find(',', Pos);
    if (Comma == StringRef::npos) {
      Tokens.emplace_back(S.substr(Pos).str());
      break;
    }
    Tokens.emplace_back(S.substr(Pos, Comma - Pos).str());
    Pos = Comma + 1;
  }
  return Tokens;
}

std::string joinCSV(const std::vector<std::string> &Tokens) {
  std::string Out;
  for (size_t I = 0; I < Tokens.size(); ++I) {
    if (I != 0) {
      Out.push_back(',');
    }
    Out += Tokens[I];
  }
  return Out;
}

bool tryRewriteCStringInitializer(GlobalVariable &GV, Module &M,
                                  StringRef NewString) {
  if (!GV.hasInitializer()) {
    return false;
  }
  ConstantDataSequential *CDS =
      dyn_cast<ConstantDataSequential>(GV.getInitializer());
  if (!CDS || !CDS->isCString()) {
    return false;
  }
  Constant *NewInitializer =
      ConstantDataArray::getString(M.getContext(), NewString, true);
  if (NewInitializer->getType() != GV.getValueType()) {
    return false;
  }
  GV.setInitializer(NewInitializer);
  return true;
}

bool isObjCPropertyAttrGV(const GlobalVariable &GV) {
  return GV.getName().contains("OBJC_PROP_NAME_ATTR_") ||
         GV.getSection().contains("__objc_const");
}

std::string replaceTrailingField(StringRef SymbolName,
                                 const std::unordered_map<std::string, std::string>
                                     &NameMap) {
  size_t DotPos = SymbolName.rfind('.');
  if (DotPos == StringRef::npos || DotPos + 1 >= SymbolName.size()) {
    return SymbolName.str();
  }
  StringRef Tail = SymbolName.substr(DotPos + 1);
  auto It = NameMap.find(Tail.str());
  if (It == NameMap.end()) {
    return SymbolName.str();
  }
  std::string NewName = SymbolName.substr(0, DotPos + 1).str();
  NewName += It->second;
  return NewName;
}

} // namespace

namespace llvm {
struct ObjCPropertyObfuscation : public ModulePass {
  static char ID;
  bool flag;
  ObjCPropertyObfuscation() : ModulePass(ID) { this->flag = true; }
  ObjCPropertyObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override { return "ObjCPropertyObfuscation"; }

  bool isSwiftModule(Module &M) {
    for (const Function &Fn : M) {
      if (Fn.getCallingConv() == CallingConv::Swift)
        return true;
      StringRef N = Fn.getName();
#if LLVM_VERSION_MAJOR >= 18
      if (N.starts_with("$s") || N.starts_with("_$s"))
#else
      if (N.startswith("$s") || N.startswith("_$s"))
#endif
        return true;
    }
    return false;
  }

  bool runOnModule(Module &M) override {
    if (!flag) {
      return false;
    }

    // Skip Swift modules — Swift @objc property metadata interacts with
    // other frameworks via ObjC runtime and renaming breaks cross-module usage.
    if (isSwiftModule(M)) {
      errs() << "[ObjCPropertyObfuscation] Skipping Swift module: "
             << M.getSourceFileName() << "\n";
      return false;
    }

    errs() << "Running ObjCPropertyObfuscation pass on "
           << M.getSourceFileName() << "\n";
    const std::string Seed = getObjCPropertySeed();
    errs() << "[ObjCPropertyObfuscation] seed: " << Seed << "\n";

    Triple TripleInfo(M.getTargetTriple());
    if (TripleInfo.getVendor() != Triple::VendorType::Apple) {
      return false;
    }

    std::unordered_map<std::string, std::string> IvarNameMap;
    unsigned SkippedProtocol = 0;

    auto ensureMappedName = [&](StringRef OldName,
                                std::unordered_map<std::string, std::string> &TargetMap,
                                StringRef Domain) -> bool {
      if (TargetMap.count(OldName.str()) != 0) {
        return true;
      }
      std::string NewName;
      for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
        NewName = generateSameLengthIdentifier(OldName, Seed, Domain, Attempt);
        if (NewName != OldName && !isPropertyNameBlacklisted(NewName)) {
          break;
        }
        NewName.clear();
      }
      if (NewName.empty()) {
        return false;
      }
      TargetMap.emplace(OldName.str(), NewName);
      return true;
    };

    bool Changed = false;
    for (GlobalVariable &GV : M.globals()) {
      if (!isObjCPropertyAttrGV(GV) || !GV.hasInitializer()) {
        continue;
      }
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString()) {
        continue;
      }

      // Skip properties referenced by protocol definitions.
      if (isUsedInProtocolContext(GV)) {
        ++SkippedProtocol;
        errs() << "[ObjCPropertyObfuscation] skip protocol property: "
               << CDS->getAsCString() << "\n";
        continue;
      }

      auto Tokens = splitCSV(CDS->getAsCString());
      if (Tokens.size() < 2) {
        continue;
      }

      bool LocalChanged = false;
      // Tokens[0] is the type encoding (e.g. "T@\"NSString\"", "TC", "TI",
      // "Ti", "Tq", "TB").  It must NEVER be renamed — doing so corrupts the
      // ObjC runtime property attribute parsing.  For example, renaming "TI"
      // to "GA" creates a spurious custom-getter attribute G=A, which makes
      // WCDB (and any code that calls property_copyAttributeValue(p,"G"))
      // use selector "A" instead of the real property name, crashing at
      // runtime with "unrecognized selector".
      //
      // We only rename the ivar name (V-prefixed token) below.

      for (size_t I = 1; I < Tokens.size(); ++I) {
        StringRef Attr = Tokens[I];
        if (Attr.size() <= 1 || Attr.front() != 'V') {
          continue;
        }
        StringRef Ivar = Attr.drop_front();
        if (!isPropertyIdentifier(Ivar) || isPropertyNameBlacklisted(Ivar)) {
          continue;
        }
        if (!ensureMappedName(Ivar, IvarNameMap, "hikari.objcpropobf.ivar")) {
          errs() << "[ObjCPropertyObfuscation] Failed to obfuscate ivar "
                 << Ivar << "\n";
          continue;
        }
        Tokens[I] = "V" + IvarNameMap[Ivar.str()];
        LocalChanged = true;
      }

      if (!LocalChanged) {
        continue;
      }

      std::string NewValue = joinCSV(Tokens);
      if (NewValue.size() != CDS->getAsCString().size()) {
        continue;
      }
      if (tryRewriteCStringInitializer(GV, M, NewValue)) {
        Changed = true;
      }
    }

    for (GlobalVariable &GV : M.globals()) {
      StringRef OldName = GV.getName();
      if (OldName.contains("OBJC_IVAR_$_") || OldName.contains(".ivar.")) {
        std::string NewName = replaceTrailingField(OldName, IvarNameMap);
        if (NewName != OldName) {
          GV.setName(NewName);
          Changed = true;
        }
      }
      // Note: We no longer rename OBJC_PROP_NAME_ATTR_ GV names based on
      // property names.  Only ivar GV names are renamed.
    }

    errs() << "[ObjCPropertyObfuscation] Summary: "
           << IvarNameMap.size() << " ivars renamed, "
           << SkippedProtocol << " protocol skipped\n";
    for (const auto &KV : IvarNameMap) {
      errs() << "[ObjCPropertyObfuscation] ivar " << KV.first << " -> "
             << KV.second << "\n";
    }

    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createObjCPropertyObfuscationPass(bool flag) {
  return new ObjCPropertyObfuscation(flag);
}

char ObjCPropertyObfuscation::ID = 0;
INITIALIZE_PASS(ObjCPropertyObfuscation, "objcpropobf",
                "Rename Objective-C Property Metadata.", false, false)
