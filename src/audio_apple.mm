// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::audio — iOS AVAudioSession integration (T43).
//
// Subscribes to two NSNotificationCenter notifications:
//
//   AVAudioSessionInterruptionNotification
//     Began  → onAudioFocusLost()
//     Ended  → onAudioFocusGained() (only when shouldResume is set by OS)
//
//   AVAudioSessionRouteChangeNotification
//     OldDeviceUnavailable → headphones unplugged → onAudioFocusLost()
//       then immediately onAudioFocusGained() so audio continues through
//       the built-in speaker; the lost/gained pair effectively re-syncs
//       the route without permanently silencing audio. (Conventional iOS
//       behavior is to pause on unplug and let the user decide; we follow
//       that convention by pausing and immediately re-evaluating — the
//       game can always resume manually if it wants a different policy.)
//
// This file is compiled only on iOS (TargetConditionals.h guard in Module.mk
// via the .mm extension selecting ObjC++ — the guard is belt-and-suspenders).
//
// macOS desktop: audio focus is not a concept; this file is compiled but
// the notification registrations are gated on TARGET_OS_IOS so it compiles
// cleanly for both iOS and macOS targets.

#include <ge/audio.h>

#include <TargetConditionals.h>

#if TARGET_OS_IOS

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

#include <spdlog/spdlog.h>

namespace ge::audio {

// Singleton observer bundle. Registered once by installAppleAudioObservers()
// which is called from DirectRenderHost's constructor on iOS.
// The ids are retained by NSNotificationCenter; we hold them to remove on
// demand (if ge ever tears down and reinitialises, which doesn't happen in
// practice but is good hygiene).
static id g_interruptionObserver = nil;
static id g_routeChangeObserver  = nil;

void installAppleAudioObservers() {
    if (g_interruptionObserver != nil) return;  // idempotent

    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];

    // Interruption (incoming call, alarm, Siri, etc.).
    g_interruptionObserver = [nc
        addObserverForName:AVAudioSessionInterruptionNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
            NSDictionary* info = note.userInfo;
            NSInteger type = [[info objectForKey:AVAudioSessionInterruptionTypeKey]
                              integerValue];
            if (type == AVAudioSessionInterruptionTypeBegan) {
                SPDLOG_INFO("ge::audio: AVAudioSession interruption began");
                ge::audio::onAudioFocusLost();
            } else if (type == AVAudioSessionInterruptionTypeEnded) {
                // Only resume if iOS says we should. Some interruptions
                // (e.g. phone call accepted) don't set this flag — respect
                // that by staying paused until the game re-routes audio.
                NSInteger optBits = [[info objectForKey:
                    AVAudioSessionInterruptionOptionKey] integerValue];
                bool shouldResume = (optBits &
                    AVAudioSessionInterruptionOptionShouldResume) != 0;
                SPDLOG_INFO("ge::audio: AVAudioSession interruption ended "
                            "(shouldResume={})", shouldResume);
                if (shouldResume) {
                    ge::audio::onAudioFocusGained();
                }
            }
        }];

    // Route change (headphones plugged/unplugged, bluetooth connect/disconnect).
    g_routeChangeObserver = [nc
        addObserverForName:AVAudioSessionRouteChangeNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification* note) {
            NSDictionary* info = note.userInfo;
            NSInteger reason = [[info objectForKey:AVAudioSessionRouteChangeReasonKey]
                                integerValue];
            // OldDeviceUnavailable = headphones/BT unplugged: conventional iOS
            // behavior is to pause. We do a paired lost+gained so the audio
            // state machine reaches "playing through built-in speaker" without
            // needing explicit game-side knowledge of route changes.
            if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable) {
                SPDLOG_INFO("ge::audio: audio route changed (old device unavailable) — "
                            "brief pause");
                ge::audio::onAudioFocusLost();
                ge::audio::onAudioFocusGained();
            }
        }];

    SPDLOG_INFO("ge::audio: installed AVAudioSession observers");
}

void removeAppleAudioObservers() {
    NSNotificationCenter* nc = [NSNotificationCenter defaultCenter];
    if (g_interruptionObserver) {
        [nc removeObserver:g_interruptionObserver];
        g_interruptionObserver = nil;
    }
    if (g_routeChangeObserver) {
        [nc removeObserver:g_routeChangeObserver];
        g_routeChangeObserver = nil;
    }
}

} // namespace ge::audio

#endif // TARGET_OS_IOS
