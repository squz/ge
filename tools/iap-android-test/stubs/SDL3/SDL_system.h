#pragma once
// The test installs the JVM/activity through the production JNI entry point.
inline void* SDL_GetAndroidJNIEnv() { return nullptr; }
inline void* SDL_GetAndroidActivity() { return nullptr; }
