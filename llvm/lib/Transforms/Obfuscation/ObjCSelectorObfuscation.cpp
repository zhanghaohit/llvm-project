// For open-source license, please refer to
// [License](https://github.com/HikariObfuscator/Hikari/wiki/License).
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/ObjCSelectorObfuscation.h"
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
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/raw_ostream.h"
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace llvm;

namespace {

static cl::opt<std::string> ObjCSelectorObfuscationSeed(
    "objcselobf-seed", cl::init(""), cl::NotHidden,
    cl::desc("Seed for deterministic ObjCSelectorObfuscation. "
             "Use the same seed across all translation units in one build."));

// Return the prefix filter from OBJCSELOBF_PREFIX env var.
// Only selectors starting with this prefix will be renamed.
// Set to a non-empty string to enable obfuscation (e.g. "wk_private_").
// If empty, all matching selectors are renamed (unsafe for mixed Swift/ObjC).
static std::string getObjCSelObfPrefix() {
  if (const char *P = std::getenv("OBJCSELOBF_PREFIX")) {
    return std::string(P);
  }
  return "";
}

#if LLVM_VERSION_MAJOR >= 18
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.starts_with(Prefix);
}
inline bool containsStr(StringRef Name, StringRef Substr) {
  return Name.contains(Substr);
}
#else
inline bool startsWith(StringRef Name, StringRef Prefix) {
  return Name.startswith(Prefix);
}
inline bool containsStr(StringRef Name, StringRef Substr) {
  return Name.contains(Substr);
}
#endif

bool isIdentifierChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_' || C == ':';
}

bool isSelectorLike(StringRef S) {
  if (S.empty() || S.size() > 128) {
    return false;
  }
  if (!std::isalpha(static_cast<unsigned char>(S.front())) && S.front() != '_') {
    return false;
  }
  bool HasAlphabet = false;
  for (char C : S) {
    if (!isIdentifierChar(C)) {
      return false;
    }
    HasAlphabet |= std::isalpha(static_cast<unsigned char>(C));
  }
  return HasAlphabet;
}

bool isBlacklistedSelector(StringRef S) {
  static const std::unordered_set<std::string> Blacklist = {
      // NSObject fundamentals
      "initialize", "load", "dealloc", "init", "new", "copy", "mutableCopy",
      "retain", "release", "autorelease", "retainCount",
      "class", "superclass", "hash", "description", "debugDescription",
      "isEqual:", "isKindOfClass:", "isMemberOfClass:",
      "respondsToSelector:", "conformsToProtocol:",
      "performSelector:", "performSelector:withObject:",
      "performSelector:withObject:withObject:",
      "copyWithZone:", "mutableCopyWithZone:",
      "encodeWithCoder:", "initWithCoder:",
      "main", "self", "isProxy", "zone",
      "forwardInvocation:", "methodSignatureForSelector:",
      "doesNotRecognizeSelector:", "forwardingTargetForSelector:",
      // KVC/KVO
      "valueForKey:", "setValue:forKey:",
      "valueForKeyPath:", "setValue:forKeyPath:",
      "valueForUndefinedKey:", "setValue:forUndefinedKey:",
      "setValuesForKeysWithDictionary:", "dictionaryWithValuesForKeys:",
      "validateValue:forKey:error:", "validateValue:forKeyPath:error:",
      "observeValueForKeyPath:ofObject:change:context:",
      "addObserver:forKeyPath:options:context:",
      "removeObserver:forKeyPath:",
      "removeObserver:forKeyPath:context:",
      "willChangeValueForKey:", "didChangeValueForKey:",
      "willChange:valuesAtIndexes:forKey:",
      "didChange:valuesAtIndexes:forKey:",
      // UIViewController lifecycle
      "viewDidLoad", "loadView", "viewWillAppear:", "viewDidAppear:",
      "viewWillDisappear:", "viewDidDisappear:",
      "viewWillLayoutSubviews", "viewDidLayoutSubviews",
      "viewDidUnload", "didReceiveMemoryWarning",
      "prepareForSegue:sender:",
      "shouldPerformSegueWithIdentifier:sender:",
      "viewWillTransitionToSize:withTransitionCoordinator:",
      "willMoveToParentViewController:",
      "didMoveToParentViewController:",
      "preferredStatusBarStyle", "prefersStatusBarHidden",
      "supportedInterfaceOrientations", "shouldAutorotate",
      "preferredInterfaceOrientationForPresentation",
      "childViewControllerForStatusBarStyle",
      "childViewControllerForStatusBarHidden",
      "setNeedsStatusBarAppearanceUpdate",
      "traitCollectionDidChange:",
      "viewSafeAreaInsetsDidChange",
      "additionalSafeAreaInsets",
      "edgesForExtendedLayout",
      "extendedLayoutIncludesOpaqueBars",
      "modalPresentationStyle",
      "modalTransitionStyle",
      // UIView
      "layoutSubviews", "drawRect:", "sizeThatFits:",
      "intrinsicContentSize", "invalidateIntrinsicContentSize",
      "setNeedsLayout", "layoutIfNeeded",
      "setNeedsDisplay", "setNeedsDisplayInRect:",
      "hitTest:withEvent:", "pointInside:withEvent:",
      "awakeFromNib", "prepareForInterfaceBuilder",
      "updateConstraints", "requiresConstraintBasedLayout",
      "didAddSubview:", "willRemoveSubview:",
      "willMoveToSuperview:", "didMoveToSuperview",
      "willMoveToWindow:", "didMoveToWindow",
      "tintColorDidChange",
      "safeAreaInsetsDidChange",
      // UIResponder
      "touchesBegan:withEvent:", "touchesMoved:withEvent:",
      "touchesEnded:withEvent:", "touchesCancelled:withEvent:",
      "touchesEstimatedPropertiesUpdated:",
      "canBecomeFirstResponder", "becomeFirstResponder",
      "canResignFirstResponder", "resignFirstResponder",
      "isFirstResponder",
      "canPerformAction:withSender:",
      "targetForAction:withSender:",
      "motionBegan:withEvent:", "motionEnded:withEvent:",
      "motionCancelled:withEvent:",
      "nextResponder", "inputView", "inputAccessoryView",
      "undoManager",
      // UIApplication / AppDelegate
      "application:didFinishLaunchingWithOptions:",
      "applicationDidBecomeActive:",
      "applicationWillResignActive:",
      "applicationDidEnterBackground:",
      "applicationWillEnterForeground:",
      "applicationWillTerminate:",
      "application:openURL:options:",
      "application:openURL:sourceApplication:annotation:",
      "application:configurationForConnectingSceneSession:options:",
      "application:didDiscardSceneSessions:",
      "application:didRegisterForRemoteNotificationsWithDeviceToken:",
      "application:didFailToRegisterForRemoteNotificationsWithError:",
      "application:didReceiveRemoteNotification:fetchCompletionHandler:",
      "application:handleEventsForBackgroundURLSession:completionHandler:",
      "application:willFinishLaunchingWithOptions:",
      "application:supportedInterfaceOrientationsForWindow:",
      "application:continueUserActivity:restorationHandler:",
      "application:performActionForShortcutItem:completionHandler:",
      // UIScene
      "scene:willConnectToSession:options:",
      "sceneDidDisconnect:", "sceneDidBecomeActive:",
      "sceneWillResignActive:", "sceneWillEnterForeground:",
      "sceneDidEnterBackground:",
      "scene:openURLContexts:",
      "scene:continueUserActivity:",
      // UITableView delegate/datasource
      "numberOfSectionsInTableView:",
      "tableView:numberOfRowsInSection:",
      "tableView:cellForRowAtIndexPath:",
      "tableView:titleForHeaderInSection:",
      "tableView:titleForFooterInSection:",
      "tableView:didSelectRowAtIndexPath:",
      "tableView:didDeselectRowAtIndexPath:",
      "tableView:heightForRowAtIndexPath:",
      "tableView:heightForHeaderInSection:",
      "tableView:heightForFooterInSection:",
      "tableView:viewForHeaderInSection:",
      "tableView:viewForFooterInSection:",
      "tableView:estimatedHeightForRowAtIndexPath:",
      "tableView:editingStyleForRowAtIndexPath:",
      "tableView:commitEditingStyle:forRowAtIndexPath:",
      "tableView:canEditRowAtIndexPath:",
      "tableView:canMoveRowAtIndexPath:",
      "tableView:moveRowAtIndexPath:toIndexPath:",
      "tableView:willDisplayCell:forRowAtIndexPath:",
      "tableView:didEndDisplayingCell:forRowAtIndexPath:",
      "tableView:accessoryButtonTappedForRowWithIndexPath:",
      "tableView:shouldHighlightRowAtIndexPath:",
      "tableView:willBeginEditingRowAtIndexPath:",
      "tableView:didEndEditingRowAtIndexPath:",
      "tableView:indentationLevelForRowAtIndexPath:",
      "tableView:trailingSwipeActionsConfigurationForRowAtIndexPath:",
      "tableView:leadingSwipeActionsConfigurationForRowAtIndexPath:",
      "tableView:contextMenuConfigurationForRowAtIndexPath:point:",
      // UICollectionView delegate/datasource
      "numberOfSectionsInCollectionView:",
      "collectionView:numberOfItemsInSection:",
      "collectionView:cellForItemAtIndexPath:",
      "collectionView:viewForSupplementaryElementOfKind:atIndexPath:",
      "collectionView:didSelectItemAtIndexPath:",
      "collectionView:didDeselectItemAtIndexPath:",
      "collectionView:layout:sizeForItemAtIndexPath:",
      "collectionView:layout:insetForSectionAtIndex:",
      "collectionView:layout:minimumLineSpacingForSectionAtIndex:",
      "collectionView:layout:minimumInteritemSpacingForSectionAtIndex:",
      "collectionView:layout:referenceSizeForHeaderInSection:",
      "collectionView:layout:referenceSizeForFooterInSection:",
      "collectionView:willDisplayCell:forItemAtIndexPath:",
      "collectionView:didEndDisplayingCell:forItemAtIndexPath:",
      "collectionView:shouldSelectItemAtIndexPath:",
      "collectionView:shouldHighlightItemAtIndexPath:",
      "collectionView:canMoveItemAtIndexPath:",
      "collectionView:moveItemAtIndexPath:toIndexPath:",
      "collectionView:contextMenuConfigurationForItemAtIndexPath:point:",
      // UIScrollView
      "scrollViewDidScroll:", "scrollViewWillBeginDragging:",
      "scrollViewDidEndDragging:willDecelerate:",
      "scrollViewWillBeginDecelerating:",
      "scrollViewDidEndDecelerating:",
      "scrollViewDidEndScrollingAnimation:",
      "scrollViewDidScrollToTop:",
      "scrollViewShouldScrollToTop:",
      "scrollViewDidZoom:",
      "viewForZoomingInScrollView:",
      "scrollViewWillEndDragging:withVelocity:targetContentOffset:",
      // UITextField / UITextView
      "textFieldShouldBeginEditing:", "textFieldDidBeginEditing:",
      "textFieldShouldEndEditing:", "textFieldDidEndEditing:",
      "textField:shouldChangeCharactersInRange:replacementString:",
      "textFieldShouldClear:", "textFieldShouldReturn:",
      "textViewShouldBeginEditing:", "textViewDidBeginEditing:",
      "textViewShouldEndEditing:", "textViewDidEndEditing:",
      "textView:shouldChangeTextInRange:replacementText:",
      "textViewDidChange:", "textViewDidChangeSelection:",
      // UISearchBar
      "searchBarSearchButtonClicked:", "searchBarCancelButtonClicked:",
      "searchBar:textDidChange:", "searchBarTextDidBeginEditing:",
      "searchBarTextDidEndEditing:",
      "searchBar:shouldChangeTextInRange:replacementText:",
      // UINavigationController
      "navigationController:willShowViewController:animated:",
      "navigationController:didShowViewController:animated:",
      "navigationController:animationControllerForOperation:fromViewController:toViewController:",
      "navigationController:interactionControllerForAnimationController:",
      // UIGestureRecognizer
      "gestureRecognizer:shouldReceiveTouch:",
      "gestureRecognizerShouldBegin:",
      "gestureRecognizer:shouldRecognizeSimultaneouslyWithGestureRecognizer:",
      "gestureRecognizer:shouldRequireFailureOfGestureRecognizer:",
      "gestureRecognizer:shouldBeRequiredToFailByGestureRecognizer:",
      // UIAlertController / UIActionSheet (legacy)
      "alertView:clickedButtonAtIndex:",
      "actionSheet:clickedButtonAtIndex:",
      // UIImagePicker
      "imagePickerController:didFinishPickingMediaWithInfo:",
      "imagePickerControllerDidCancel:",
      // UIPickerView
      "numberOfComponentsInPickerView:",
      "pickerView:numberOfRowsInComponent:",
      "pickerView:titleForRow:forComponent:",
      "pickerView:didSelectRow:inComponent:",
      "pickerView:viewForRow:forComponent:reusingView:",
      "pickerView:rowHeightForComponent:",
      "pickerView:widthForComponent:",
      // UITabBarController
      "tabBarController:shouldSelectViewController:",
      "tabBarController:didSelectViewController:",
      // Accessibility
      "isAccessibilityElement", "accessibilityLabel",
      "accessibilityHint", "accessibilityValue",
      "accessibilityTraits", "accessibilityFrame",
      "accessibilityIdentifier",
      "accessibilityElementCount", "accessibilityElementAtIndex:",
      "indexOfAccessibilityElement:",
      "accessibilityPerformEscape", "accessibilityActivate",
      // NSCoding / NSSecureCoding
      "supportsSecureCoding", "initWithCoder:", "encodeWithCoder:",
      "awakeAfterUsingCoder:",
      // NSObject protocol extra
      "isProxy", "version", "setVersion:",
      // UIKit cell registration/reuse
      "prepareForReuse", "setSelected:animated:", "setHighlighted:animated:",
      "setEditing:animated:",
      // UIStoryboard / Nib
      "initWithNibName:bundle:",
      "initWithStyle:",
      "awakeFromNib",
      // Notification handling
      "addTarget:action:forControlEvents:",
      // CLLocationManager
      "locationManager:didUpdateLocations:",
      "locationManager:didFailWithError:",
      "locationManager:didChangeAuthorizationStatus:",
      "locationManagerDidChangeAuthorization:",
      // MKMapView
      "mapView:viewForAnnotation:",
      "mapView:didSelectAnnotationView:",
      "mapView:regionDidChangeAnimated:",
      // WKWebView / WKNavigationDelegate
      "webView:didFinishNavigation:",
      "webView:didFailNavigation:withError:",
      "webView:decidePolicyForNavigationAction:decisionHandler:",
      "webView:decidePolicyForNavigationResponse:decisionHandler:",
      "webView:didStartProvisionalNavigation:",
      "webView:didFailProvisionalNavigation:withError:",
      "webView:didReceiveServerRedirectForProvisionalNavigation:",
      "webView:didCommitNavigation:",
      "webView:runJavaScriptAlertPanelWithMessage:initiatedByFrame:completionHandler:",
      "userContentController:didReceiveScriptMessage:",
      // URLSession
      "URLSession:dataTask:didReceiveData:",
      "URLSession:task:didCompleteWithError:",
      "URLSession:downloadTask:didFinishDownloadingToURL:",
      "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:",
      "URLSession:dataTask:didReceiveResponse:completionHandler:",
      "URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:",
      "URLSession:didBecomeInvalidWithError:",
      "URLSession:task:didReceiveChallenge:completionHandler:",
      "URLSession:downloadTask:didWriteData:totalBytesWritten:totalBytesExpectedToWrite:",
      "URLSession:downloadTask:didResumeAtOffset:expectedTotalBytes:",
      "URLSessionDidFinishEventsForBackgroundURLSession:",
      // CoreBluetooth CBCentralManagerDelegate
      "centralManagerDidUpdateState:",
      "centralManager:willRestoreState:",
      "centralManager:didDiscoverPeripheral:advertisementData:RSSI:",
      "centralManager:didConnectPeripheral:",
      "centralManager:didFailToConnectPeripheral:error:",
      "centralManager:didDisconnectPeripheral:error:",
      "centralManager:didDisconnectPeripheral:timestamp:isReconnecting:error:",
      "centralManager:connectionEventDidOccur:forPeripheral:",
      "centralManager:didUpdateANCSAuthorizationForPeripheral:",
      // CoreBluetooth CBPeripheralDelegate
      "peripheral:didDiscoverServices:",
      "peripheral:didDiscoverIncludedServicesForService:error:",
      "peripheral:didDiscoverCharacteristicsForService:error:",
      "peripheral:didUpdateValueForCharacteristic:error:",
      "peripheral:didWriteValueForCharacteristic:error:",
      "peripheral:didUpdateNotificationStateForCharacteristic:error:",
      "peripheral:didDiscoverDescriptorsForCharacteristic:error:",
      "peripheral:didUpdateValueForDescriptor:error:",
      "peripheral:didWriteValueForDescriptor:error:",
      "peripheral:didReadRSSI:error:",
      "peripheralDidUpdateRSSI:error:",
      "peripheralDidUpdateName:",
      "peripheral:didModifyServices:",
      "peripheral:didOpenL2CAPChannel:error:",
      "peripheralIsReadyToSendWriteWithoutResponse:",
      // CoreBluetooth CBPeripheralManagerDelegate
      "peripheralManagerDidUpdateState:",
      "peripheralManager:willRestoreState:",
      "peripheralManagerDidStartAdvertising:error:",
      "peripheralManager:didAddService:error:",
      "peripheralManager:central:didSubscribeToCharacteristic:",
      "peripheralManager:central:didUnsubscribeFromCharacteristic:",
      "peripheralManager:didReceiveReadRequest:",
      "peripheralManager:didReceiveWriteRequests:",
      "peripheralManager:didPublishL2CAPChannel:error:",
      "peripheralManager:didUnpublishL2CAPChannel:error:",
      "peripheralManager:didOpenL2CAPChannel:error:",
      // WatchConnectivity WCSessionDelegate
      "sessionDidBecomeInactive:", "sessionDidDeactivate:",
      "session:activationDidCompleteWithState:error:",
      "sessionWatchStateDidChange:", "sessionReachabilityDidChange:",
      "session:didReceiveMessage:", "session:didReceiveMessage:replyHandler:",
      "session:didReceiveMessageData:", "session:didReceiveMessageData:replyHandler:",
      "session:didReceiveApplicationContext:",
      "session:didFinishUserInfoTransfer:error:",
      "session:didReceiveUserInfo:",
      "session:didFinishFileTransfer:error:",
      "session:didReceiveFile:",
      // UserNotifications UNUserNotificationCenterDelegate
      "userNotificationCenter:willPresentNotification:withCompletionHandler:",
      "userNotificationCenter:didReceiveNotificationResponse:withCompletionHandler:",
      "userNotificationCenter:openSettingsForNotification:",
      // HealthKit HKWorkoutSessionDelegate
      "workoutSession:didChangeTo:from:date:",
      "workoutSession:didFailWithError:",
      "workoutSession:didGenerateEvent:",
      // HealthKit HKWorkoutBuilderDelegate
      "workoutBuilder:didCollectDataOfTypes:",
      "workoutBuilderDidCollectEvent:",
      // AVFoundation AVAudioPlayerDelegate
      "audioPlayerDidFinishPlaying:successfully:",
      "audioPlayerDecodeErrorDidOccur:error:",
      "audioPlayerBeginInterruption:", "audioPlayerEndInterruption:",
      "audioPlayerEndInterruption:withOptions:",
      "audioPlayerEndInterruption:withFlags:",
      // AVFoundation AVAudioRecorderDelegate
      "audioRecorderDidFinishRecording:successfully:",
      "audioRecorderEncodeErrorDidOccur:error:",
      // AVFoundation AVSpeechSynthesizerDelegate
      "speechSynthesizer:didStartSpeechUtterance:",
      "speechSynthesizer:didFinishSpeechUtterance:",
      "speechSynthesizer:didPauseSpeechUtterance:",
      "speechSynthesizer:didContinueSpeechUtterance:",
      "speechSynthesizer:didCancelSpeechUtterance:",
      "speechSynthesizer:willSpeakRangeOfSpeechString:utterance:",
      // UIAdaptivePresentationControllerDelegate
      "adaptivePresentationStyleForPresentationController:",
      "adaptivePresentationStyleForPresentationController:traitCollection:",
      "viewControllerForAdaptivePresentationStyle:forPresentationController:",
      "presentationControllerWillDismiss:",
      "presentationControllerDidDismiss:",
      "presentationControllerShouldDismiss:",
      "presentationControllerDidAttemptToDismiss:",
      // UIViewControllerTransitioningDelegate
      "animationControllerForPresentedController:presentingController:sourceController:",
      "animationControllerForDismissedController:",
      "interactionControllerForPresentation:",
      "interactionControllerForDismissal:",
      "presentationControllerForPresentedViewController:presentingViewController:sourceViewController:",
      // UIViewControllerAnimatedTransitioning
      "transitionDuration:", "animateTransition:", "animationEnded:",
      // UISplitViewController
      "splitViewController:willChangeToDisplayMode:",
      "splitViewController:collapseSecondaryViewController:ontoPrimaryViewController:",
      "splitViewController:separateSecondaryViewControllerFromPrimaryViewController:",
      "splitViewController:showViewController:sender:",
      "splitViewController:showDetailViewController:sender:",
      "primaryViewControllerForCollapsingSplitViewController:",
      "primaryViewControllerForExpandingSplitViewController:",
      // UIPageViewController
      "pageViewController:viewControllerAfterViewController:",
      "pageViewController:viewControllerBeforeViewController:",
      "pageViewController:spineLocationForInterfaceOrientation:",
      "presentationCountForPageViewController:",
      "presentationIndexForPageViewController:",
      "pageViewController:didFinishAnimating:previousViewControllers:transitionCompleted:",
      "pageViewController:willTransitionToViewControllers:",
      // NSFetchedResultsController
      "controllerWillChangeContent:", "controllerDidChangeContent:",
      "controller:didChangeSection:atIndex:forChangeType:",
      "controller:didChangeObject:atIndexPath:forChangeType:newIndexPath:",
      "controller:sectionIndexTitleForSectionName:",
      // UITextInput / UIKeyInput / UITextInputDelegate
      "insertText:", "deleteBackward", "hasText",
      "selectionWillChange:", "selectionDidChange:",
      "textWillChange:", "textDidChange:",
      // UIFocus
      "didUpdateFocusInContext:withAnimationCoordinator:",
      "shouldUpdateFocusInContext:",
      "indexPathForPreferredFocusedViewInCollectionView:",
      // UIDynamicAnimatorDelegate
      "dynamicAnimatorDidPause:", "dynamicAnimatorWillResume:",
      // UITableView prefetch
      "tableView:prefetchRowsAtIndexPaths:",
      "tableView:cancelPrefetchingForRowsAtIndexPaths:",
      // UICollectionView prefetch
      "collectionView:prefetchItemsAtIndexPaths:",
      "collectionView:cancelPrefetchingForItemsAtIndexPaths:",
      // UIActivityItemSource
      "activityViewControllerPlaceholderItem:",
      "activityViewController:itemForActivityType:",
      "activityViewController:subjectForActivityType:",
      "activityViewController:dataTypeIdentifierForActivityType:",
      "activityViewController:thumbnailImageForActivityType:suggestedSize:",
      // UIPopoverPresentationController
      "popoverPresentationControllerDidDismissPopover:",
      "popoverPresentationControllerShouldDismissPopover:",
      "prepareForPopoverPresentation:",
      // NSProgressReporting
      "progress",
      // UIPreviewAction / UIPreviewActionItem
      "previewActions",
  };
  return Blacklist.count(S.str()) != 0;
}

// Check if a selector string is used by any protocol method list global.
// Walk the use chain upward to find the owning GlobalVariable.
// If any owning GV has "PROTOCOL" in its name, the selector is a protocol
// method and should not be renamed.
bool isUsedInProtocolMethodList(GlobalVariable &SelectorGV) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> Worklist;
  Worklist.push_back(&SelectorGV);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *GV = dyn_cast<GlobalVariable>(U)) {
        StringRef GVName = GV->getName();
        if (containsStr(GVName, "PROTOCOL"))
          return true;
      }
      // Walk through ConstantExpr, ConstantAggregate, etc.
      if (isa<Constant>(U) && !isa<GlobalVariable>(U)) {
        Worklist.push_back(U);
      }
    }
  }
  return false;
}

// Check if a selector string is ONLY used in local class/instance method lists
// (not in selrefs or protocol method lists in this TU).
// Selectors with instruction uses (selrefs) call system or cross-TU methods —
// renaming them would break system framework dispatch. Only selectors in
// method list globals are implementations we define and can safely rename
// (subject to prefix filter and protocol checks below).
bool isOnlyUsedInLocalMethodLists(GlobalVariable &SelectorGV) {
  SmallPtrSet<Value *, 16> Visited;
  SmallVector<Value *, 16> Worklist;
  Worklist.push_back(&SelectorGV);

  while (!Worklist.empty()) {
    Value *V = Worklist.pop_back_val();
    if (!Visited.insert(V).second)
      continue;

    for (User *U : V->users()) {
      if (auto *GV = dyn_cast<GlobalVariable>(U)) {
        StringRef GVName = GV->getName();
        if (containsStr(GVName, "PROTOCOL"))
          return false;
        if (containsStr(GVName, "INSTANCE_METHODS") ||
            containsStr(GVName, "CLASS_METHODS") ||
            containsStr(GVName, "PROP_LIST") ||
            containsStr(GVName, "CATEGORY"))
          continue;
        return false;
      }
      if (isa<Constant>(U) && !isa<GlobalVariable>(U)) {
        Worklist.push_back(U);
        continue;
      }
      if (isa<Instruction>(U))
        return false;
    }
  }
  return true;
}

bool isObjCSelectorStringGV(const GlobalVariable &GV) {
  StringRef Name = GV.getName();
  StringRef Section = GV.getSection();
  return Section.contains("__objc_methname") ||
         Name.contains("OBJC_METH_VAR_NAME_");
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

std::string getObjCSelectorSeed() {
  if (!ObjCSelectorObfuscationSeed.empty()) {
    return ObjCSelectorObfuscationSeed;
  }
  if (const char *EnvSeed = std::getenv("OBJCSELOBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  if (const char *EnvSeed = std::getenv("HIKARI_OBF_SEED")) {
    if (*EnvSeed != '\0') {
      return std::string(EnvSeed);
    }
  }
  return "hikari-objcselobf-default-seed";
}

std::string generateSameLengthSelector(StringRef OldSelector, StringRef Seed,
                                       unsigned Attempt) {
  static constexpr char FirstCharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_";
  static constexpr char CharSet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_";

  std::string Entropy = buildEntropyHex("hikari.objcselobf.selector", Seed,
                                        OldSelector, Attempt,
                                        OldSelector.size() * 2 + 2);
  std::string NewSelector = OldSelector.str();
  size_t EntropyPos = 0;
  for (size_t I = 0; I < NewSelector.size(); ++I) {
    if (OldSelector[I] == ':') {
      NewSelector[I] = ':';
      continue;
    }
    const char *PickSet = (I == 0 || OldSelector[I - 1] == ':') ? FirstCharSet
                                                                 : CharSet;
    const size_t PickSize = (I == 0 || OldSelector[I - 1] == ':')
                                ? sizeof(FirstCharSet) - 1
                                : sizeof(CharSet) - 1;
    const unsigned Byte =
        (hexToNibble(Entropy[EntropyPos]) << 4) |
        hexToNibble(Entropy[EntropyPos + 1]);
    EntropyPos += 2;
    NewSelector[I] = PickSet[Byte % PickSize];
  }
  return NewSelector;
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

} // namespace

namespace llvm {
struct ObjCSelectorObfuscation : public ModulePass {
  static char ID;
  bool flag;
  ObjCSelectorObfuscation() : ModulePass(ID) { this->flag = true; }
  ObjCSelectorObfuscation(bool flag) : ModulePass(ID) { this->flag = flag; }
  StringRef getPassName() const override { return "ObjCSelectorObfuscation"; }

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

    // Skip Swift modules — Swift @objc bridged selectors are used by other
    // frameworks via ObjC runtime dispatch. The isOnlyUsedInLocalMethodLists
    // heuristic cannot detect cross-framework usage of Swift-bridged selectors.
    if (isSwiftModule(M)) {
      errs() << "[ObjCSelectorObfuscation] Skipping Swift module: "
             << M.getSourceFileName() << "\n";
      return false;
    }

    errs() << "Running ObjCSelectorObfuscation pass on "
           << M.getSourceFileName() << "\n";
    const std::string Seed = getObjCSelectorSeed();
    errs() << "[ObjCSelectorObfuscation] seed: " << Seed << "\n";

    Triple TripleInfo(M.getTargetTriple());
    if (TripleInfo.getVendor() != Triple::VendorType::Apple) {
      return false;
    }

    // Prefix filter: only rename selectors starting with OBJCSELOBF_PREFIX.
    // If OBJCSELOBF_PREFIX is empty (default), rename ALL selectors that pass
    // the blacklist + protocol + isOnlyUsedInLocalMethodLists checks.
    // isOnlyUsedInLocalMethodLists() rejects any selector with instruction
    // uses (selrefs) in the current TU, protecting same-TU Swift/ObjC calls.
    // Cross-TU Swift callers are not visible here; set OBJCSELOBF_PREFIX to a
    // private-only prefix (e.g. "wp_") if cross-TU safety is a concern.
    const std::string SelPrefix = getObjCSelObfPrefix();
    const bool PrefixFilterEnabled = !SelPrefix.empty();
    if (PrefixFilterEnabled) {
      errs() << "[ObjCSelectorObfuscation] prefix filter: " << SelPrefix << "\n";
    } else {
      errs() << "[ObjCSelectorObfuscation] no prefix filter — renaming all safe selectors\n";
    }

    std::unordered_map<std::string, std::string> SelectorMap;
    unsigned SkippedBlacklist = 0;
    unsigned SkippedPrefix = 0;
    unsigned SkippedProtocol = 0;
    unsigned SkippedExternal = 0;

    bool Changed = false;
    for (GlobalVariable &GV : M.globals()) {
      if (!isObjCSelectorStringGV(GV) || !GV.hasInitializer()) {
        continue;
      }
      ConstantDataSequential *CDS =
          dyn_cast<ConstantDataSequential>(GV.getInitializer());
      if (!CDS || !CDS->isCString()) {
        continue;
      }

      std::string OldSelector = CDS->getAsCString().str();
      if (!isSelectorLike(OldSelector)) {
        continue;
      }
      if (isBlacklistedSelector(OldSelector)) {
        ++SkippedBlacklist;
        continue;
      }

      // Only rename selectors matching the user-specified prefix (if set).
      if (PrefixFilterEnabled && !startsWith(OldSelector, SelPrefix)) {
        ++SkippedPrefix;
        continue;
      }

      // Skip selectors used in protocol method lists (system/framework protocols).
      if (isUsedInProtocolMethodList(GV)) {
        ++SkippedProtocol;
        continue;
      }

      // Only rename selectors that are exclusively in method list globals.
      // Selectors with instruction uses (selrefs) call system/external methods
      // and must not be renamed (Swift TUs and Pods callers stay unchanged).
      if (!isOnlyUsedInLocalMethodLists(GV)) {
        ++SkippedExternal;
        continue;
      }

      if (SelectorMap.count(OldSelector) == 0) {
        std::string NewSelector;
        for (unsigned Attempt = 0; Attempt < 64; ++Attempt) {
          NewSelector = generateSameLengthSelector(OldSelector, Seed, Attempt);
          if (NewSelector != OldSelector && !isBlacklistedSelector(NewSelector)) {
            break;
          }
          NewSelector.clear();
        }
        if (NewSelector.empty()) {
          errs() << "[ObjCSelectorObfuscation] Failed to obfuscate selector "
                 << OldSelector << "\n";
          continue;
        }
        SelectorMap.emplace(OldSelector, NewSelector);
      }

      const std::string &NewSelector = SelectorMap[OldSelector];
      if (tryRewriteCStringInitializer(GV, M, NewSelector)) {
        Changed = true;
      }
    }

    errs() << "[ObjCSelectorObfuscation] Summary: "
           << SelectorMap.size() << " renamed, "
           << SkippedBlacklist << " blacklisted, "
           << SkippedPrefix << " prefix-mismatch, "
           << SkippedProtocol << " protocol, "
           << SkippedExternal << " external-selref\n";

    for (const auto &KV : SelectorMap) {
      errs() << "[ObjCSelectorObfuscation] " << KV.first << " -> " << KV.second
             << "\n";
    }

    return Changed;
  }
};
} // namespace llvm

ModulePass *llvm::createObjCSelectorObfuscationPass(bool flag) {
  return new ObjCSelectorObfuscation(flag);
}

char ObjCSelectorObfuscation::ID = 0;
INITIALIZE_PASS(ObjCSelectorObfuscation, "objcselobf",
                "Rename Objective-C Selector Strings.", false, false)
