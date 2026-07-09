/* unity_input_hook.c -- feed Switch touch straight into UnityEngine.Input.
 *
 * Unity 6's nativeInjectEvent path is a dead end here: it ACCEPTS our fake MotionEvent
 * (inject_ret=1) but never reads its coordinates (never calls getX/getY on the JNI event),
 * so Input.touchCount / GetTouch / mousePosition stay empty and the game's UI (EZGUI
 * UIManager, ColliderEvent, EventWaitTouch, ...) never sees a tap.
 *
 * Instead we bypass the whole JNI event system: patch the game's own il2cpp Input methods
 * to return OUR touch state directly. The game reads these every frame, so it now sees the
 * Switch touchscreen. RVAs from Il2CppDumper (script.json), runtime addr = il2cpp
 * load_virtbase + RVA (same mapping as the payment/camera patches).
 */
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "unity_input_hook.h"

int   debugPrintf(char *fmt, ...);
extern int screen_width, screen_height;
int so_patch_code(void *dst, const void *src, unsigned long len);   /* so_util.c */

/* ---- touch state, written by android_native_feed_hid every frame ---------- */
int   g_hook_count = 0;                 /* Input.touchCount (0/1)                        */
int   g_hook_btn   = 0;                 /* Input.GetMouseButton(0)                       */
int   g_hook_btn_down = 0, g_hook_btn_up = 0;  /* GetMouseButtonDown/Up(0), 1-frame edges */
int   g_hook_phase = 3;                 /* TouchPhase: 0 Began 1 Moved 2 Stat 3 Ended    */
float g_hook_x = 0.0f, g_hook_y = 0.0f; /* Unity screen space (bottom-left origin, px)   */

/* ---- Unity value types (AArch64 return conventions matter) ---------------- */
typedef struct { float x, y; }    NxV2;
typedef struct { float x, y, z; } NxV3;                 /* HFA -> s0,s1,s2      */
typedef struct {                                        /* UnityEngine.Touch, 0x44 bytes */
  int32_t m_FingerId;       NxV2 m_Position;   NxV2 m_RawPosition; NxV2 m_PositionDelta;
  float   m_TimeDelta;      int32_t m_TapCount; int32_t m_Phase;   int32_t m_Type;
  float   m_Pressure;       float m_maxPressure; float m_Radius;   float m_RadiusVariance;
  float   m_AltitudeAngle;  float m_AzimuthAngle;
} NxTouch;                                               /* > 16 bytes -> sret (x8) */

/* ---- the hooks: il2cpp calling convention = (real args..., MethodInfo*) ---- */
static int32_t hk_touchCount(void *mi){ (void)mi; return g_hook_count; }
static NxV3    hk_mousePosition(void *mi){ (void)mi; NxV3 v = { g_hook_x, g_hook_y, 0.0f }; return v; }
static uint8_t hk_getMouseButton(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn : 0; }
static uint8_t hk_getMouseButtonDown(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_down : 0; }
static uint8_t hk_getMouseButtonUp(int32_t b, void *mi){ (void)mi; return (b==0) ? (uint8_t)g_hook_btn_up : 0; }
static NxTouch hk_getTouch(int32_t index, void *mi){
  (void)mi; NxTouch t; memset(&t, 0, sizeof t);
  if (index == 0) {
    t.m_Position.x = g_hook_x; t.m_Position.y = g_hook_y;
    t.m_RawPosition = t.m_Position;
    t.m_Phase = g_hook_phase; t.m_TapCount = 1;
    t.m_Pressure = 1.0f; t.m_maxPressure = 1.0f;
  }
  return t;
}

/* Overwrite a method entry with an absolute long jump to `target`:
 *   ldr x16, #8 ; br x16 ; .quad target      (16 bytes) */
static void patch_jump(uintptr_t site, void *target) {
  uint32_t code[4];
  code[0] = 0x58000050u;                 /* ldr x16, #8  (load target from site+8) */
  code[1] = 0xd61f0200u;                 /* br  x16                                */
  memcpy(&code[2], &target, sizeof target);
  so_patch_code((void *)site, code, sizeof code);
}

/* Il2CppDumper RVAs (== link-time VADDR); runtime = il2cpp load_virtbase + RVA. */
#define RVA_get_touchCount        0x1bf48f4u
#define RVA_get_mousePosition     0x1bf441cu
#define RVA_GetMouseButton        0x1bf404cu
#define RVA_GetMouseButtonDown    0x1bf4088u
#define RVA_GetMouseButtonUp      0x1bf40c4u
#define RVA_GetTouch              0x1bf4100u

void nx_install_input_hooks(uintptr_t il2cpp_base) {
  patch_jump(il2cpp_base + RVA_get_touchCount,     (void *)hk_touchCount);
  patch_jump(il2cpp_base + RVA_get_mousePosition,  (void *)hk_mousePosition);
  patch_jump(il2cpp_base + RVA_GetMouseButton,     (void *)hk_getMouseButton);
  patch_jump(il2cpp_base + RVA_GetMouseButtonDown, (void *)hk_getMouseButtonDown);
  patch_jump(il2cpp_base + RVA_GetMouseButtonUp,   (void *)hk_getMouseButtonUp);
  patch_jump(il2cpp_base + RVA_GetTouch,           (void *)hk_getTouch);
  debugPrintf("[input] il2cpp Input hooks installed (touchCount/GetTouch/mousePosition/GetMouseButton*)\n");
}

/* Replace LanguageParam.getCurrentLanguage() so the game uses nx_pick_elanguage()'s
 * choice. There is no saved language field, so the game calls this live per scene load. */
int nx_pick_elanguage(void);                       /* jni_fake.c */
#define RVA_getCurrentLanguage    0xf815b4u
static int32_t hk_getCurrentLanguage(void *mi){ (void)mi; return (int32_t)nx_pick_elanguage(); }

void nx_install_language_hook(uintptr_t il2cpp_base) {
  patch_jump(il2cpp_base + RVA_getCurrentLanguage, (void *)hk_getCurrentLanguage);
}

/* Called by android_native_feed_hid with the current touch, already mapped to Unity screen
 * space (bottom-left origin, game pixels). active=1 while a finger is down; the release frame
 * is reported once as phase Ended (count still 1) so EZGUI sees the tap complete. */
void nx_input_hook_update(int active, float ux, float uy) {
  static int prev = 0;
  g_hook_btn_down = (active && !prev);
  g_hook_btn_up   = (!active && prev);
  if (active) {
    g_hook_count = 1; g_hook_btn = 1;
    g_hook_x = ux; g_hook_y = uy;
    g_hook_phase = prev ? 1 /*Moved*/ : 0 /*Began*/;
  } else if (prev) {                     /* release: one Ended frame at the last position */
    g_hook_count = 1; g_hook_btn = 0; g_hook_phase = 3 /*Ended*/;
  } else {
    g_hook_count = 0; g_hook_btn = 0; g_hook_phase = 3;
  }
  prev = active;
}

/* ---- PlayerPrefs persistence ---------------------------------------------
 * The game saves via UnityEngine.PlayerPrefs, but Unity's native PlayerPrefs never reaches disk
 * on Switch (it doesn't use our JNI SharedPreferences and its native flush is a no-op), so
 * progress lived only in RAM and vanished on reboot. We replace the Set/Get/Delete/Save methods
 * so the game's PlayerPrefs go through our own persistent store (unity_jni.c prefs.kv), loaded on
 * boot and flushed to the SD. RVAs are the UnityEngine.PlayerPrefs methods. */
extern void        nx_prefs_set(char type, const char *key, const char *val);   /* unity_jni.c */
extern const char *nx_prefs_get(const char *key);
extern void        nx_prefs_del(const char *key);
extern void        nx_prefs_flush(void);

static void *(*g_il2cpp_string_new)(const char *);

/* il2cpp System.String (arm64): length @0x10 (int32), UTF-16 chars @0x14. Malloc'd UTF-8 copy
 * (caller frees); values can be large (serialized save blobs). */
static char *il2str_dup(void *s) {
  if (!s) return NULL;
  int32_t len = *(int32_t *)((char *)s + 0x10);
  if (len < 0) len = 0;
  char *out = (char *)malloc((size_t)len * 3 + 1);
  if (!out) return NULL;
  const uint16_t *ch = (const uint16_t *)((char *)s + 0x14);
  int o = 0;
  for (int i = 0; i < len; i++) {
    uint32_t c = ch[i];
    if (c < 0x80)        out[o++] = (char)c;
    else if (c < 0x800){ out[o++] = (char)(0xC0|(c>>6));  out[o++] = (char)(0x80|(c&0x3F)); }
    else               { out[o++] = (char)(0xE0|(c>>12)); out[o++] = (char)(0x80|((c>>6)&0x3F)); out[o++] = (char)(0x80|(c&0x3F)); }
  }
  out[o] = 0;
  return out;
}
static void *mkstr(const char *s) { return g_il2cpp_string_new ? g_il2cpp_string_new(s ? s : "") : (void *)0; }

/* il2cpp static-method ABI: (real args..., MethodInfo*). */
static void hk_pp_SetString(void *key, void *val, void *mi) {
  (void)mi; char *k = il2str_dup(key), *v = il2str_dup(val);
  if (k) nx_prefs_set('S', k, v ? v : "");
  free(k); free(v);
}
static void hk_pp_SetInt(void *key, int32_t val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[16];
  if (k) { snprintf(b, sizeof b, "%d", (int)val); nx_prefs_set('I', k, b); }
  free(k);
}
static void hk_pp_SetFloat(void *key, float val, void *mi) {
  (void)mi; char *k = il2str_dup(key), b[32];
  if (k) { snprintf(b, sizeof b, "%.9g", (double)val); nx_prefs_set('F', k, b); }
  free(k);
}
static void *hk_pp_GetString2(void *key, void *def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? mkstr(v) : def;
}
static void *hk_pp_GetString1(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return mkstr(v ? v : "");
}
static int32_t hk_pp_GetInt2(void *key, int32_t def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (int32_t)atoi(v) : def;
}
static float hk_pp_GetFloat2(void *key, float def, void *mi) {
  (void)mi; char *k = il2str_dup(key); const char *v = k ? nx_prefs_get(k) : NULL; free(k);
  return v ? (float)atof(v) : def;
}
static uint8_t hk_pp_HasKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); int h = (k && nx_prefs_get(k)); free(k); return h ? 1 : 0;
}
static void hk_pp_DeleteKey(void *key, void *mi) {
  (void)mi; char *k = il2str_dup(key); if (k) nx_prefs_del(k); free(k);
}
static void hk_pp_Save(void *mi) { (void)mi; nx_prefs_flush(); }

void nx_install_playerprefs_hooks(uintptr_t il2cpp_base, void *string_new) {
  g_il2cpp_string_new = (void *(*)(const char *))string_new;
  patch_jump(il2cpp_base + 0x1B9A1A8u, (void *)hk_pp_SetString);
  patch_jump(il2cpp_base + 0x1B99D64u, (void *)hk_pp_SetInt);
  patch_jump(il2cpp_base + 0x1B99F80u, (void *)hk_pp_SetFloat);
  if (g_il2cpp_string_new) {   /* GetString needs to build a String; skip if unresolved (avoid NRE) */
    patch_jump(il2cpp_base + 0x1B9A200u, (void *)hk_pp_GetString2); /* GetString(key,def)  */
    patch_jump(il2cpp_base + 0x1B9A504u, (void *)hk_pp_GetString1); /* GetString(key)      */
  }
  patch_jump(il2cpp_base + 0x1B99DBCu, (void *)hk_pp_GetInt2);      /* covers GetInt(key)  */
  patch_jump(il2cpp_base + 0x1B99FD8u, (void *)hk_pp_GetFloat2);    /* covers GetFloat(key)*/
  patch_jump(il2cpp_base + 0x1B9A54Cu, (void *)hk_pp_HasKey);
  patch_jump(il2cpp_base + 0x1B9A6FCu, (void *)hk_pp_DeleteKey);
  patch_jump(il2cpp_base + 0x1B9A8C8u, (void *)hk_pp_Save);
}
