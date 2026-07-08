/* unity_entrypoints.h -- UnityPlayer native methods recovered from
 * libunity.so's RegisterNatives table (LAYTON BROTHERS MYSTERY ROOM,
 * Unity 6000.0.58f2, arm64-v8a, changeset 92dee566b325).
 *
 * Recovered by tools/re/jnitables.py (a Unity-version-agnostic scan of the
 * R_AARCH64_RELATIVE relocations that build the {name,sig,fn} method tables).
 * The UnityPlayer table sits at .data.rel.ro 0x11b5608 (32 entries).
 *
 * These offsets are LINK-TIME addresses for THIS exact libunity.so (the shipped,
 * engine-code-stripped build). Runtime address = unity_mod.load_virtbase + off
 * (the .so links at base 0; for the exec segment file-offset == vaddr, RVA == VA).
 * If the game updates, re-run tools/re/jnitables.py against the new binary.
 *
 * Unity-6 deltas vs the vln_nx (2021.3) base:
 *   - initJni is (Context,int) -- 2 ARGS (was 1); the int is an API/flags value.
 *   - new lifecycle natives: nativeUnityPlayerSetRunning(Z), nativeGetNoWindowMode,
 *     nativeMemoryUsageChanged(I), nativeConfigurationChanged, nativeOnApplyWindowInsets.
 *   - every address differs (engine version + code stripping).
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- UnityPlayer native method offsets (link-time vaddr) ----------------
 * Laytonbmr table @0x11b5608 (32 entries). */
/* drive-critical */
#define OFF_JNI_OnLoad                    0x6c9ed4 /* (JavaVM*,reserved)->jint  export; caches VM, registers natives */
#define OFF_initJni                       0x6c8d24 /* (env,thiz,Context,int)  *** 4-arg: Context + int *** */
#define OFF_nativeRecreateGfxState        0x6c8ff0 /* (env,thiz,int,Surface)  set surface*/
#define OFF_nativeSendSurfaceChangedEvent 0x6c9058 /* (env,thiz)                         */
#define OFF_nativeRender                  0x6c92c4 /* (env,thiz)->Z   per-frame; false=stop */
#define OFF_nativeInjectEvent             0x6c9324 /* (env,thiz,InputEvent)->Z  3-arg, NO trailing int */
#define OFF_nativePause                   0x6c8e78 /* (env,thiz)->Z                      */
#define OFF_nativeResume                  0x6c8edc /* (env,thiz)->V                      */
#define OFF_nativeFocusChanged            0x6c8f8c /* (env,thiz,Z)                       */
#define OFF_nativeDone                    0x6c8d98 /* (env,thiz)->Z   shutdown           */
#define OFF_nativeApplicationUnload       0x6c8f3c /* (env,thiz)                         */
#define OFF_nativeOrientationChanged      0x6c9dc8 /* (env,thiz,int,int)                 */
/* Unity-6 lifecycle (new) */
#define OFF_nativeUnityPlayerSetRunning   0x6c9e28 /* (env,thiz,Z)  Unity6: gate the loop  */
#define OFF_nativeGetNoWindowMode         0x6c9e7c /* (env,thiz)->Z                      */
#define OFF_nativeMemoryUsageChanged      0x6c8e24 /* (env,thiz,int)                     */
#define OFF_nativeConfigurationChanged    0x6c9234 /* (env,thiz,Configuration)           */
#define OFF_nativeOnApplyWindowInsets     0x6c90ac /* (env,thiz,WindowInsets)            */
/* secondary / usually unused for a port */
#define OFF_nativeUnitySendMessage        0x6c992c /* (env,thiz,String,String,byte[])    */
#define OFF_nativeMuteMasterAudio         0x6c9b58 /* (env,thiz,Z)                       */
#define OFF_nativeIsAutorotationOn        0x6c9af8 /* (env,thiz)->Z                      */
#define OFF_onAudioVolumeChanged          0x6c58e8 /* (env,thiz,int)                     */
#define OFF_permissionResponseToNative    0x6c9cf4 /* (env,thiz,long,Z)                  */
#define OFF_nativeSetLaunchURL            0x6c9bbc /* (env,thiz,String)                  */
#define OFF_nativeHidePreservedContent    0x6c9d78 /* (env,thiz)                         */
/* soft keyboard (route via SoftInputProvider stub; not needed for first boot) */
#define OFF_nativeSetInputArea            0x6c95dc
#define OFF_nativeSetKeyboardIsVisible    0x6c9664
#define OFF_nativeSetInputString          0x6c96c4
#define OFF_nativeSetInputSelection       0x6c976c
#define OFF_nativeSoftInputClosed         0x6c98d4
#define OFF_nativeSoftInputCanceled       0x6c97dc
#define OFF_nativeSoftInputLostFocus      0x6c9834
#define OFF_nativeReportKeyboardConfigChanged 0x6c988c

/* ---- FMOD natives (libunity RegisterNatives table @0x11c1a60) -----------
 * Captured for the audio path (opensles.c). */
#define OFF_fmodGetInfo                   0xe4209c /* (I)I                               */
#define OFF_fmodProcess                   0xe42164 /* (Ljava/nio/ByteBuffer;)I           */
#define OFF_fmodProcessMicData            0xe421f0 /* (Ljava/nio/ByteBuffer;I)I          */

/* ---- Native engine clock fix (THE black-screen fix; see main.c) ----------
 * TimeManager::Update(double newTime) -- Unity 6 passes newTime as the d0 arg
 * (2021.3 took only `this`). Located in the shipped stripped binary by its
 * distinctive prologue (frameCount++ @+0x160, aux++ @+0x168, pause byte @+0x1a8;
 * ref sym _ZN11TimeManager6UpdateEd @0x86cf54, unique prologue match @shipped
 * file 0x5194d8 -> VADDR 0x51d4d8; +0x4000 delta). Entry runs the prologue then
 * falls through (past cbz+ret) to the body at entry+0x24, which reads newTime from
 * d0 (fmov d8,d0) and integrates. Our hook (main.c nx_time_update_hook) replays
 * the prologue then calls the body with newTime = live monotonic seconds (starts
 * ~0, matching GetTimeSinceStartup's epoch), so deltaTime advances at wall rate
 * regardless of the frozen vsync feed.
 * NOTE: these are LINK-TIME VADDRs. libunity .text VA = file_off + 0x4000. */
#define OFF_TimeManager_Update_entry      0x51d4d8 /* prologue: [+0x160]++;[+0x168]++;if([+0x1a8])ret */
#define OFF_TimeManager_Update_body       0x51d4fc /* frameless; reads newTime from d0 */
#define TM_FIELD_FRAMECOUNT               0x160    /* u64 */
#define TM_FIELD_AUX                      0x168    /* u32 */
#define TM_FIELD_PAUSE                    0x1a8    /* byte */

/* ---- JNI native signatures: ret (*)(JNIEnv*, jobject thiz, args...) -----
 * initJni gains a trailing int in Unity 6 (Context,int). nativeInjectEvent is
 * (env,thiz,InputEvent) -- 3 args; we keep fn_inject 4-arg so feed_hid can pass a
 * deviceId uniformly (the engine's 3-arg fn ignores w3, harmless on AArch64). */
typedef void     (*fn_initJni)(void*,void*,void*,int32_t);   /* env,thiz,Context,int */
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

#endif /* UNITY_ENTRYPOINTS_H */
