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

/* Push the current touch (Unity screen space, bottom-left origin, px) each frame. */
void nx_input_hook_update(int active, float ux, float uy);

#endif /* UNITY_INPUT_HOOK_H */
