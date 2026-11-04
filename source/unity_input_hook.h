/* unity_input_hook.h -- patch the game's il2cpp UnityEngine.Input methods to return our
 * Switch touch state, and force the display language. See unity_input_hook.c. */
#ifndef UNITY_INPUT_HOOK_H
#define UNITY_INPUT_HOOK_H

#include <stdint.h>

/* Install the hooks. Call once after libil2cpp is loaded+finalized (il2cpp load_virtbase). */
void nx_install_input_hooks(uintptr_t il2cpp_base);
void nx_install_language_hook(uintptr_t il2cpp_base);

/* Route UnityEngine.PlayerPrefs (the game's save) through our persistent prefs.kv store, since
 * Unity's native PlayerPrefs never writes to disk on Switch. string_new = il2cpp_string_new. */
void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new);

/* Max simultaneous touches reported to UnityEngine.Input (Switch panel: 16,
 * clamped here to keep the per-frame state small; the game only needs 2). */
#define NX_HOOK_MAX_TOUCHES 10

/* Push the current touches (Unity screen space, bottom-left origin, px) each
 * frame. count is clamped to NX_HOOK_MAX_TOUCHES; ids are Unity fingerIds
 * (real fingers: the HID finger_id; virtual stick fingers: 1000 = cursor,
 * 1001 = ZR second finger). A finger absent from this frame's list after
 * being present is reported once with phase Ended, then dropped, matching
 * Unity semantics and the old single-touch behaviour. At capacity, new
 * fingers are deferred until continuing fingers and release events fit.
 * Mouse emulation follows the first finger by ID; after it lifts, another
 * click can start only after all fingers have lifted. */
void nx_input_hook_update_multi(int count, const int32_t *ids,
                                const float *xs, const float *ys);

/* Single-touch compatibility wrapper: one touch while active, none while not. */
void nx_input_hook_update(int active, float ux, float uy);

#endif /* UNITY_INPUT_HOOK_H */
