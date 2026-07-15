// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// iOS orientation lock (🎯T36). Two swizzles on UIViewController:
//
//   1. `prefersInterfaceOrientationLocked` (Apple TN3192, iPadOS 26+)
//      returns YES once a lock has been requested. This freezes the
//      post-launch orientation against the iPadOS multitasking
//      swivel gesture.
//   2. `supportedInterfaceOrientations` is narrowed to the consumer's
//      requested orientation when a lock is active. iOS rotates the
//      UI at launch to a supported orientation, so the swizzle here
//      effectively forces the specific orientation regardless of how
//      the device was held when launching.
//
// The plist's `UISupportedInterfaceOrientations` becomes the fallback
// (consulted when no runtime lock is set) rather than the gate.
// Consumers can leave the plist permissive (all four orientations)
// and let `SessionConfig.orientation` decide at runtime.
//
// Things that DON'T work *alone* and should not be re-tried as sole knobs:
//   * UIRequiresFullScreen                         — deprecated, ignored on iPad.
//   * SDL_HINT_ORIENTATIONS                        — limits the supported set
//                                                    only; no runtime force.
//   * UIWindowScene requestGeometryUpdate alone    — silently no-ops on iPad.
//   * setNeedsUpdateOfSupportedInterfaceOrientations alone — only flips
//                                                    UIKit's view of the
//                                                    supported set; doesn't
//                                                    create a lock.
//
// Working *stack* (same for direct apps and stream players — 🎯T154.3):
//   1. Packaging plist narrows the launch set (PlayerLand / Port / game app).
//   2. g_lockedOrientation + the two swizzles above (runtime lock).
//   3. setNeedsUpdate* so UIKit re-queries the swizzles.
//   4. requestGeometryUpdate *after* the lock is armed — not alone; this is
//      what rotates an already-presented scene into the locked mask when the
//      device/sim is still in the wrong orientation (matches direct glass).
//
// History:
//   * e0da016 reverted the "plist alone" experiment.
//   * 5c2f2a5 added the prefersInterfaceOrientationLocked swizzle (boolean-mode lock).
//   * v0.31.0 (🎯T36) added the supportedInterfaceOrientations swizzle so
//     the specific constant is honored.

#include "player_orientation.h"

#import <UIKit/UIKit.h>
#import <objc/runtime.h>
#include <SDL3/SDL_video.h>
#include <spdlog/spdlog.h>

// Sentinel matching ge::wire::kOrientationAnyLandscape (Protocol.h).
// Kept in sync by inspection — Protocol.h's constant is exported as
// uint8_t; both files have a comment pointing at the other.
static constexpr uint8_t kLockAnyLandscape = 0xFE;

// 0 = unlocked. Non-zero = the SDL_ORIENTATION_* (or sentinel) value
// the consumer requested. Read by both swizzles.
static uint8_t g_lockedOrientation = 0;

// Map a requested lock value to the iOS interface-orientation mask.
static UIInterfaceOrientationMask geLockMask(uint8_t lock) {
    switch (lock) {
        case SDL_ORIENTATION_PORTRAIT:          return UIInterfaceOrientationMaskPortrait;
        case SDL_ORIENTATION_PORTRAIT_FLIPPED:  return UIInterfaceOrientationMaskPortraitUpsideDown;
        // SDL's "Landscape" is the device tilted left (UIDeviceOrientationLandscapeLeft),
        // which iOS surfaces to UIKit as UIInterfaceOrientationLandscapeRight.
        case SDL_ORIENTATION_LANDSCAPE:         return UIInterfaceOrientationMaskLandscapeRight;
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return UIInterfaceOrientationMaskLandscapeLeft;
        case kLockAnyLandscape:                 return UIInterfaceOrientationMaskLandscape;
        default:                                return UIInterfaceOrientationMaskAll;
    }
}

@interface UIViewController (GeOrientationLock)
@end

@implementation UIViewController (GeOrientationLock)

+ (void)load {
    // Swizzle 1 — prefersInterfaceOrientationLocked (iPadOS 26+).
    {
        SEL sel = @selector(prefersInterfaceOrientationLocked);
        Method orig = class_getInstanceMethod([UIViewController class], sel);
        if (orig) {
            IMP newImp = imp_implementationWithBlock(^BOOL(id self) {
                return g_lockedOrientation != 0;
            });
            method_setImplementation(orig, newImp);
        }
    }

    // Swizzle 2 — supportedInterfaceOrientations. When locked, narrow
    // to the requested mask so iOS rotates the UI to it at launch.
    // When unlocked, fall through to the original (plist-derived) mask.
    {
        SEL sel = @selector(supportedInterfaceOrientations);
        Method orig = class_getInstanceMethod([UIViewController class], sel);
        if (orig) {
            IMP originalImp = method_getImplementation(orig);
            IMP newImp = imp_implementationWithBlock(^UIInterfaceOrientationMask(id self) {
                if (g_lockedOrientation == 0) {
                    using OrigFn = UIInterfaceOrientationMask (*)(id, SEL);
                    return ((OrigFn)originalImp)(self, sel);
                }
                return geLockMask(g_lockedOrientation);
            });
            method_setImplementation(orig, newImp);
        }
    }
}

@end

static bool g_deviceOrientationActive = false;

int playerGetPhysicalOrientation() {
    if (!g_deviceOrientationActive) {
        [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
        g_deviceOrientationActive = true;
    }
    // Real devices report physical orientation. Simulator returns Unknown
    // (no accelerometer), which falls through to portrait default.
    UIDeviceOrientation dev = [UIDevice currentDevice].orientation;
    switch (dev) {
    case UIDeviceOrientationPortrait:           return SDL_ORIENTATION_PORTRAIT;
    case UIDeviceOrientationPortraitUpsideDown: return SDL_ORIENTATION_PORTRAIT_FLIPPED;
    case UIDeviceOrientationLandscapeLeft:      return SDL_ORIENTATION_LANDSCAPE;
    case UIDeviceOrientationLandscapeRight:     return SDL_ORIENTATION_LANDSCAPE_FLIPPED;
    default:                                    return SDL_ORIENTATION_PORTRAIT;
    }
}

// Best-effort name for the diagnostic log.
static const char* geLockName(uint8_t lock) {
    switch (lock) {
        case SDL_ORIENTATION_PORTRAIT:          return "Portrait";
        case SDL_ORIENTATION_PORTRAIT_FLIPPED:  return "PortraitFlipped";
        case SDL_ORIENTATION_LANDSCAPE:         return "Landscape (LandscapeRight)";
        case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return "LandscapeFlipped (LandscapeLeft)";
        case kLockAnyLandscape:                 return "AnyLandscape";
        default:                                return "Unknown";
    }
}

// Apply lock to every live window scene. Safe to call repeatedly (direct
// send() and player post-glass both use this — same API surface).
static void geApplyOrientationToScenes(UIInterfaceOrientationMask requested) {
    NSInteger scenes = 0;
    for (UIScene *s in UIApplication.sharedApplication.connectedScenes) {
        if (![s isKindOfClass:[UIWindowScene class]]) continue;
        UIWindowScene *scene = (UIWindowScene *)s;
        ++scenes;
        for (UIWindow *w in scene.windows) {
            UIViewController *vc = w.rootViewController;
            if (!vc) continue;
            // Refresh both the lock state and the supported-orientations
            // set so iOS re-evaluates and rotates the UI if needed.
            [vc setNeedsUpdateOfSupportedInterfaceOrientations];
            SEL updateSel = @selector(setNeedsUpdateOfPrefersInterfaceOrientationLocked);
            if ([vc respondsToSelector:updateSel]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
                [vc performSelector:updateSel];
#pragma clang diagnostic pop
            }
        }
        // After the lock is armed, ask the scene to adopt the locked mask.
        // Alone this no-ops on iPad; with the swizzles it rotates a portrait
        // presentation into landscape (direct glass behaviour).
        if (@available(iOS 16.0, *)) {
            UIWindowSceneGeometryPreferencesIOS *prefs =
                [[UIWindowSceneGeometryPreferencesIOS alloc]
                    initWithInterfaceOrientations:requested];
            [scene requestGeometryUpdateWithPreferences:prefs
                errorHandler:^(NSError *error) {
                    if (error) {
                        SPDLOG_WARN("playerForceOrientation: geometry update: {}",
                                    error.localizedDescription.UTF8String);
                    }
                }];
        }
    }
    if (scenes == 0) {
        SPDLOG_INFO("playerForceOrientation: no UIWindowScene yet — lock armed; "
                    "will re-apply when scenes exist");
    }
}

void playerForceOrientation(uint8_t orientation) {
    if (orientation == 0) return;

    if (!g_deviceOrientationActive) {
        [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
        g_deviceOrientationActive = true;
    }

    g_lockedOrientation = orientation;

    UIInterfaceOrientationMask requested = geLockMask(orientation);
    SPDLOG_INFO("playerForceOrientation: lock {} (mask=0x{:x})", geLockName(orientation), (unsigned)requested);

    // Cross-check against the plist's UISupportedInterfaceOrientations. If
    // none of the requested orientations are in the plist's allowed set,
    // iOS won't be able to honor the lock during launch — a loud log
    // saves the multimaze2-style debugging session.
    NSArray<NSString*>* plistOrientations =
        [[NSBundle mainBundle] objectForInfoDictionaryKey:@"UISupportedInterfaceOrientations"];
    if (plistOrientations) {
        UIInterfaceOrientationMask plistMask = 0;
        for (NSString* o in plistOrientations) {
            if ([o isEqualToString:@"UIInterfaceOrientationPortrait"])           plistMask |= UIInterfaceOrientationMaskPortrait;
            else if ([o isEqualToString:@"UIInterfaceOrientationPortraitUpsideDown"]) plistMask |= UIInterfaceOrientationMaskPortraitUpsideDown;
            else if ([o isEqualToString:@"UIInterfaceOrientationLandscapeLeft"]) plistMask |= UIInterfaceOrientationMaskLandscapeLeft;
            else if ([o isEqualToString:@"UIInterfaceOrientationLandscapeRight"]) plistMask |= UIInterfaceOrientationMaskLandscapeRight;
        }
        if ((plistMask & requested) == 0) {
            SPDLOG_WARN(
                "Info.plist UISupportedInterfaceOrientations (0x{:x}) does not include "
                "requested orientation {} (0x{:x}). The swizzle overrides this at runtime, "
                "but narrowing the plist matches engine intent and avoids brief launch flicker.",
                (unsigned)plistMask, geLockName(orientation), (unsigned)requested);
        }
    }

    void (^apply)(void) = ^{
        geApplyOrientationToScenes(requested);
    };

    if ([NSThread isMainThread]) {
        apply();
        // Second kick next runloop: CreateWindow / immersive may attach the
        // root VC after the first call (stream player) the same way a late
        // DirectRenderHost::send still needs a live scene.
        dispatch_async(dispatch_get_main_queue(), apply);
    } else {
        dispatch_async(dispatch_get_main_queue(), apply);
    }
}
