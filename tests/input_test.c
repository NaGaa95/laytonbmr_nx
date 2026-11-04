#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "unity_input_hook.c"

int screen_width = 1920, screen_height = 1080;
int debugPrintf(char *fmt, ...) { (void)fmt; return 0; }
int so_patch_code(void *d, const void *s, unsigned long n) { (void)d; (void)s; (void)n; return 0; }
int nx_pick_elanguage(void) { return 0; }
void nx_prefs_set(char t, const char *k, const char *v) { (void)t; (void)k; (void)v; }
const char *nx_prefs_get(const char *k) { (void)k; return NULL; }
void nx_prefs_del(const char *k) { (void)k; }
void nx_prefs_flush(void) {}

/* Only the libnx boundary is mocked; hid_feed.inc contains the real HID code. */
typedef struct { int unused; } PadState;
typedef struct { int32_t x, y; } HidAnalogStickState;
typedef struct { uint32_t finger_id, x, y; } TestHidTouch;
typedef struct { int32_t count; TestHidTouch touches[16]; } HidTouchScreenState;
enum { HidNpadStyleSet_NpadStandard = 1, HidNpadButton_A = 1,
       HidNpadButton_ZR = 2, AppletOperationMode_Console = 1 };
static uint64_t buttons;
static HidAnalogStickState stick;
static HidTouchScreenState panel;
static int operation_mode = AppletOperationMode_Console;
void padConfigureInput(int n, int s) { (void)n; (void)s; }
void padInitializeDefault(PadState *p) { (void)p; }
void hidInitializeTouchScreen(void) {}
void padUpdate(PadState *p) { (void)p; }
HidAnalogStickState padGetStickPos(PadState *p, int index) { (void)p; (void)index; return stick; }
uint64_t padGetButtons(PadState *p) { (void)p; return buttons; }
int hidGetTouchScreenStates(HidTouchScreenState *p, int n) { (void)n; *p = panel; return 1; }
int appletGetOperationMode(void) { return operation_mode; }
#include "hid_feed.inc"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; \
} } while (0)

static void reset(void) {
  nx_input_hook_update_multi(0, NULL, NULL, NULL);
  nx_input_hook_update_multi(0, NULL, NULL, NULL);
  screen_width = 1920; screen_height = 1080;
  buttons = 0;
  memset(&stick, 0, sizeof stick);
  memset(&panel, 0, sizeof panel);
  g_cursor_x = 960; g_cursor_y = 540;
  android_native_feed_hid();
}
static int touch(int32_t id, NxTouch *out) {
  for (int i = 0; i < hk_touchCount(NULL); i++) {
    NxTouch t = hk_getTouch(i, NULL);
    if (t.m_FingerId == id) { *out = t; return 1; }
  }
  return 0;
}
static float absolute(float x) { return x < 0 ? -x : x; }

static void test_single_touch(void) {
  reset();
  nx_input_hook_update(1, 120, 240);
  CHECK(hk_touchCount(NULL) == 1 && hk_getTouch(0,NULL).m_Phase == 0);
  CHECK(hk_getMouseButtonDown(0,NULL) && hk_getMouseButton(0,NULL));
  nx_input_hook_update(1, 120, 240);
  CHECK(hk_getTouch(0,NULL).m_Phase == 2 && !hk_getMouseButtonDown(0,NULL));
  nx_input_hook_update(1, 125, 250);
  NxTouch t = hk_getTouch(0,NULL);
  CHECK(t.m_Phase == 1 && t.m_PositionDelta.x == 5 && t.m_PositionDelta.y == 10);
  nx_input_hook_update(0, 0, 0);
  CHECK(hk_getTouch(0,NULL).m_Phase == 3 && hk_getMouseButtonUp(0,NULL));
  CHECK(hk_mousePosition(NULL).x == 125 && hk_mousePosition(NULL).y == 250);
  nx_input_hook_update(0, 0, 0);
  CHECK(hk_touchCount(NULL) == 0 && !hk_getMouseButtonUp(0,NULL));
}

static void test_mouse_ownership(void) {
  reset();
  int32_t ids[] = { 7, 9 };
  float xs[] = { 100, 600 }, ys[] = { 200, 700 };
  nx_input_hook_update_multi(2, ids, xs, ys);
  ids[0] = 9; ids[1] = 7;
  xs[0] = 610; xs[1] = 100; ys[0] = 705; ys[1] = 200;
  nx_input_hook_update_multi(2, ids, xs, ys);
  CHECK(hk_getTouch(0,NULL).m_Phase == 1 && hk_getTouch(0,NULL).m_PositionDelta.x == 10);
  CHECK(hk_getTouch(1,NULL).m_Phase == 2 && hk_mousePosition(NULL).x == 100);
  nx_input_hook_update_multi(1, ids, xs, ys);
  NxTouch t;
  CHECK(touch(7,&t) && t.m_Phase == 3);
  CHECK(hk_getMouseButtonUp(0,NULL) && !hk_getMouseButton(0,NULL));
  CHECK(hk_mousePosition(NULL).x == 100);
  nx_input_hook_update_multi(1, ids, xs, ys);
  CHECK(!touch(7,&t) && !hk_getMouseButtonDown(0,NULL) && !hk_getMouseButtonUp(0,NULL));
  /* Reusing the old primary's ID while another finger remains must not click. */
  nx_input_hook_update_multi(2, ids, xs, ys);
  CHECK(!hk_getMouseButton(0,NULL) && !hk_getMouseButtonDown(0,NULL));
  nx_input_hook_update_multi(0, NULL, NULL, NULL);
  nx_input_hook_update_multi(1, ids, xs, ys);
  CHECK(hk_getMouseButtonDown(0,NULL));
}

static void test_hybrid(void) {
  for (operation_mode = 0; operation_mode <= AppletOperationMode_Console; operation_mode++) {
    reset();
    buttons = HidNpadButton_A;
    android_native_feed_hid();
    CHECK(hk_touchCount(NULL) == 1 && hk_getMouseButtonDown(0,NULL));
    panel.count = 1;
    panel.touches[0] = (TestHidTouch){ 2, 100, 100 };
    android_native_feed_hid();
    CHECK(hk_touchCount(NULL) == 2);
    CHECK(!hk_getMouseButtonDown(0,NULL) && !hk_getMouseButtonUp(0,NULL));
    CHECK(hk_mousePosition(NULL).x == 960 && hk_mousePosition(NULL).y == 540);
    CHECK(hk_getTouch(0,NULL).m_Position.x == 150 && hk_getTouch(0,NULL).m_Position.y == 930);
    buttons = 0;
    android_native_feed_hid();
    CHECK(hk_getMouseButtonUp(0,NULL) && !hk_getMouseButton(0,NULL));
    buttons = HidNpadButton_A;
    android_native_feed_hid();
    CHECK(!hk_getMouseButtonDown(0,NULL) && !hk_getMouseButton(0,NULL));
  }
}

static void test_virtual_pair(void) {
  for (int width = 1280; width <= 1920; width += 640) {
    for (int right_side = 0; right_side <= 1; right_side++) {
      reset();
      screen_width = width; screen_height = width == 1280 ? 720 : 1080;
      g_cursor_x = right_side ? width - 80.0f : width - 161.0f;
      buttons = HidNpadButton_ZR;
      android_native_feed_hid();
      CHECK(hk_touchCount(NULL) == 2 && nx_cursor2_show);
      NxTouch a = hk_getTouch(0,NULL), b = hk_getTouch(1,NULL);
      float offset = b.m_Position.x - a.m_Position.x;
      CHECK(a.m_FingerId == 1000 && b.m_FingerId == 1001 && absolute(offset) == 160);
      /* Cross the former side-switch boundary and clamp at both edges. */
      for (int frame = 0; frame < 180; frame++) {
        stick.x = frame < 60 ? 32767 : -32767;
        android_native_feed_hid();
        a = hk_getTouch(0,NULL); b = hk_getTouch(1,NULL);
        CHECK(a.m_Phase != 0 && b.m_Phase != 0);
        CHECK(absolute(a.m_PositionDelta.x - b.m_PositionDelta.x) < 0.001f);
        CHECK(absolute(b.m_Position.x - a.m_Position.x - offset) < 0.001f);
        CHECK(a.m_Position.x >= 0 && a.m_Position.x <= width);
        CHECK(b.m_Position.x >= 0 && b.m_Position.x <= width);
      }
      buttons = HidNpadButton_A;
      android_native_feed_hid();
      CHECK(hk_getTouch(1,NULL).m_Phase == 3 && !hk_getMouseButtonDown(0,NULL));
      android_native_feed_hid();
      CHECK(hk_touchCount(NULL) == 1);
      buttons = HidNpadButton_ZR;
      android_native_feed_hid();
      CHECK(hk_touchCount(NULL) == 2 && hk_getTouch(1,NULL).m_Phase == 0);
    }
  }
}

static void test_capacity(void) {
  reset();
  int32_t ids[NX_HOOK_MAX_TOUCHES];
  float xs[NX_HOOK_MAX_TOUCHES], ys[NX_HOOK_MAX_TOUCHES];
  for (int i=0; i<NX_HOOK_MAX_TOUCHES; i++) { ids[i]=i; xs[i]=(float)(100+i); ys[i]=200; }
  nx_input_hook_update_multi(NX_HOOK_MAX_TOUCHES, ids, xs, ys);
  for (int i=0; i<NX_HOOK_MAX_TOUCHES; i++) ids[i]=i+1;
  nx_input_hook_update_multi(NX_HOOK_MAX_TOUCHES, ids, xs, ys);
  NxTouch t;
  CHECK(hk_touchCount(NULL) == NX_HOOK_MAX_TOUCHES && touch(0,&t) && t.m_Phase == 3);
  CHECK(!touch(NX_HOOK_MAX_TOUCHES,&t));
  nx_input_hook_update_multi(NX_HOOK_MAX_TOUCHES, ids, xs, ys);
  CHECK(!touch(0,&t) && touch(NX_HOOK_MAX_TOUCHES,&t) && t.m_Phase == 0);
  nx_input_hook_update_multi(0, NULL, NULL, NULL);
  for (int i=0; i<hk_touchCount(NULL); i++) CHECK(hk_getTouch(i,NULL).m_Phase == 3);
  nx_input_hook_update_multi(0, NULL, NULL, NULL);
  CHECK(hk_touchCount(NULL) == 0);
}

static uint32_t random_state = 1;
static uint32_t next_random(void) {
  random_state = random_state * 1664525u + 1013904223u;
  return random_state;
}
static void test_lifecycles(void) {
  reset();
  int previous[16] = {0};
  for (int frame=0; frame<2000; frame++) {
    int32_t ids[16];
    float xs[16], ys[16];
    int requested[16] = {0}, current[16] = {0}, seen[16] = {0};
    for (int i=0; i<16; i++) ids[i]=i;
    for (int i=15; i>0; i--) {
      int j=(int)(next_random() % (uint32_t)(i+1));
      int32_t temporary=ids[i]; ids[i]=ids[j]; ids[j]=temporary;
    }
    int count=(int)(next_random() % (NX_HOOK_MAX_TOUCHES+1));
    for (int i=0; i<count; i++) { requested[ids[i]]=1; xs[i]=(float)(frame+i); ys[i]=300; }
    int was_down=hk_getMouseButton(0,NULL);
    nx_input_hook_update_multi(count, ids, xs, ys);
    CHECK(hk_touchCount(NULL) <= NX_HOOK_MAX_TOUCHES);
    CHECK(hk_getMouseButtonDown(0,NULL) == (hk_getMouseButton(0,NULL) && !was_down));
    CHECK(hk_getMouseButtonUp(0,NULL) == (!hk_getMouseButton(0,NULL) && was_down));
    for (int i=0; i<hk_touchCount(NULL); i++) {
      NxTouch t=hk_getTouch(i,NULL);
      int id=t.m_FingerId;
      CHECK(id>=0 && id<16);
      if (id<0 || id>=16) continue;
      CHECK(!seen[id]); seen[id]=1;
      if (t.m_Phase==3) CHECK(previous[id] && !requested[id]);
      else {
        CHECK(requested[id]);
        CHECK((t.m_Phase==0) == !previous[id]);
        current[id]=1;
      }
    }
    for (int id=0; id<16; id++) CHECK(!previous[id] || seen[id]);
    memcpy(previous,current,sizeof previous);
  }
}

int main(void) {
  test_single_touch();
  test_mouse_ownership();
  test_hybrid();
  test_virtual_pair();
  test_capacity();
  test_lifecycles();
  if (failures) { fprintf(stderr, "%d input checks failed\n", failures); return 1; }
  puts("PASS: tap/drag, mouse ownership, hybrid input, virtual edges, capacity and 2000 lifecycle frames");
  return 0;
}
