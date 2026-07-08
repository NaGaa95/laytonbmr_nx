/* main.c -- LAYTON BROTHERS MYSTERY ROOM Switch wrapper entry point.
 *
 * Unity 6000.0.58f2 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives
 * the lifecycle the Java UnityPlayer normally runs (JNI_OnLoad -> initJni ->
 * recreate GFX state -> surface changed -> resume/focus -> render loop), calling
 * the native entry points recovered from libunity.so's RegisterNatives table
 * (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context created
 * from android_native_window(); SDL is audio/HID only.
 *
 * Forked from the vln_nx (Very Little Nightmares) / ZOOKEEPER DX ports. Laytonbmr
 * deltas vs the vln base:
 *   - engine offsets re-derived for Unity 6000.0.58f2 by FINGERPRINTING the
 *     editor's symbolicated arm64-v8a libunity into the shipped (engine-stripped)
 *     binary -- offsets are NOT byte-compatible with vln (see tools/re);
 *   - libunity .text has a 0x4000 VA-fileoffset delta (offsets are link-time VADDRs);
 *   - initJni gains a trailing int arg (Context,int); TimeManager::Update now takes
 *     newTime as its d0 argument; il2cpp JavaVM/GC globals re-derived;
 *   - NO OBB: assets are the classic split-file layout (globalgamemanagers,
 *     level0..N, sharedassets*.assets), pre-staged flat by tools/stage_sd.py;
 *   - region-granularity 256MB->64MB patch table re-derived (region_patch.h).
 *
 * Heap + syscall scaffolding inherited from the vln/cr3_nx base (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "region_patch.h"
#include "unity_input_hook.h"
#include "diag.h"

#define DATA_ROOT  GAME_HOME          /* sdmc:/switch/laytonbmr_nx */
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"

/* ---- Audio: force FMOD's OpenSL ES output ---------------------------------
 * Unity's AudioManager::InitNormal requests an FMOD output type (AAudio/OpenSL/
 * AudioTrack from GetAndroidAudioOutputType), which is silent with no JVM. We
 * rewrite the requested type to OpenSL at the setOutput call site so FMOD self-
 * drives its OpenSL output -> dlopen(libOpenSLES.so) -> opensles.c (SDL2 queue ->
 * libnx audout). Site RE'd via the Unity 6000.0.58f2 arm64-v8a Release symbols:
 * AudioManager::InitNormal loads `ldr w1,[sp,#0x2c]` (the requested FMOD_OUTPUTTYPE)
 * at VADDR 0x7deb28 right before `bl FMOD::System::setOutput`; the muted path
 * stores 2 (NOSOUND) there, which confirms the modern FMOD enum -> OpenSL == 12.
 * Rewrite the load to `movz w1,#12`. Self-verifying: patched only if the original
 * word matches, else left stock (audio silent, never corrupted). AUDIO IS A POLISH
 * ITEM pending on-hardware verification (see LAYTONBMR_PORT_HANDOFF.md). */
#define LBMR_FMOD_OPENSL_PATCH   1
#define LBMR_FMOD_SETOUTPUT_SITE 0x7deb28
#define LBMR_FMOD_SETOUTPUT_FROM 0xb9402fe1u   /* ldr w1, [sp, #0x2c] (requested type) */
/* THIS FMOD renumbered FMOD_OUTPUTTYPE: OpenSL ES = 22 (NOT the standard 12!). RE'd from the
 * live registry: 21="FMOD Audio Track Output", 22="FMOD OpenSL ES Output", 24="FMOD AAudio Output".
 * Unity natively requests 21 (AudioTrack=Java=silent here); force 22 so FMOD uses its OpenSL output,
 * which dlopens libOpenSLES.so -> our opensles.c shim -> libnx audout. */
#define LBMR_FMOD_SETOUTPUT_TO   0x528002c1u   /* movz w1, #22 (this build's FMOD_OUTPUTTYPE_OPENSL) */

/* Apply the 256MB->64MB region-granularity patch (region_patch.h). Needed for a
 * 4GB Switch; harmless on 8GB. Self-verifying, so leaving it on is safe. */
#define LBMR_REGION_PATCH_ENABLE 1

/* Choreographer / vsync free-run. Unity 6 paces frames to Android vsync via a Choreographer we
 * can't feed, so the first frame blocks forever on a callback that never fires. Patch
 * ChoreographerBase::Get() to return NULL; its callers null-check it, so the loop free-runs on our
 * clock hook instead. Self-verifying (both entry words checked); left stock on mismatch. */
#define LBMR_CHOREO_FREERUN   1
#define LBMR_CHOREO_GET_SITE  0x6b66fc
#define LBMR_CHOREO_GET_W0    0xd10303ffu   /* sub sp, sp, #0xc0            */
#define LBMR_CHOREO_GET_W1    0xa90a57feu   /* stp x30, x21, [sp, #0xa0]    */

/* Payment/DRM boot-gate. The boot coroutine GameInitializeScene.<Start>d__0::MoveNext calls
 * initPayment(pubkey) once, fire-and-forget; on our shim the billing nativeInstance is null, so it
 * throws an NRE that kills the coroutine and the title never loads. Stub initPayment (void, caller
 * ignores the result) to a bare `ret`. Self-verifying; left stock on mismatch. Other
 * CallAndroidPlugin.* methods aren't on the boot path (would need movz w0,#v;ret as they're non-void). */
#define LBMR_PAYMENT_STUB     1
#define LBMR_INITPAYMENT_RVA  0xE229CCu
#define LBMR_INITPAYMENT_W0   0xa9bd5ffeu   /* stp x30, x23, [sp, #-0x30]! */

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena (consumed by mmap_fake/munmap_fake in libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;
u64    g_alias_base = 0, g_alias_size = 0;
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;

so_module main_mod, unity_mod, il2cpp_mod;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Native engine clock. Our loop drives nativeRender directly and never delivers Choreographer frame
 * callbacks, so TimeManager's newTime freezes, deltaTime collapses, and async scene loads never
 * finish (black screen). Detour TimeManager::Update (0x51d4d8): replay its prologue (frameCount @+0x160,
 * aux @+0x168, pause @+0x1a8), then re-enter the body (0x51d4fc) with a live monotonic newTime. */
static void (*g_unity_update_body)(void *, double) = NULL; /* 0x51d4fc Update body */
static uint64_t g_clk_base_ns = 0;
static void   *g_tm = NULL;          /* captured TimeManager instance (for the clock thread) */
static Mutex   g_clock_lock;         /* serialize main-hook vs background clock-thread ticks */

/* Android presentation/vsync counter (libunity @0x1243158). AndroidVSync::WaitForLastPresentation
 * blocks in `while(*counter < target) cond_wait` for a display present we don't have; the clock
 * thread bumps it ~120Hz and the shim's cond_wait re-polls every 16ms, so the waiter proceeds. */
#define OFF_ANDROID_VSYNC_COUNTER 0x1243158
static volatile uint64_t *g_vsync_counter = NULL;
/* Wall-clock ns of the last main-thread Update hook. The background clock thread
 * drives the engine clock ONLY when this goes stale (main thread parked in a
 * synchronous scene-load job); during normal play the per-frame hook owns the
 * clock, so the thread stays out or it fragments Time.deltaTime. */
static volatile uint64_t g_last_main_tick_ns = 0;
#define CLOCK_STALL_NS 100000000ULL   /* 100ms of main-thread silence => treat as stalled */
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
/* advance the engine clock by re-running Update's body with a live newTime.
 * newTime = seconds since our first tick (the caller's normal epoch is seconds-
 * since-startup, also ~0 at boot), so deltaTime = newTime - prevNewTime is wall-rate. */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double newTime = (double)(now - g_clk_base_ns) / 1e9;      /* seconds since first tick */
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
}
static void nx_time_update_hook(void *tm, double newTime_ignored) {
  (void)newTime_ignored;                                     /* frozen; we supply our own */
  g_tm = tm;                                                 /* capture for the clock thread */
  g_last_main_tick_ns = nx_now_ns();                         /* main thread is driving the clock */
  *(volatile uint64_t *)((char *)tm + TM_FIELD_FRAMECOUNT) += 1;  /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + TM_FIELD_AUX)        += 1;  /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + TM_FIELD_PAUSE) != 0) return; /* paused -> return    */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
/* Keep the engine clock advancing even while nativeRender blocks in a synchronous
 * scene-load job (the frame-2 wall) -- see the vln handoff. trylock so we never
 * block the main hook or invert lock order; the thread gets its own bionic TLS. */
static Thread g_clock_thr;
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  uint64_t vsync_last_ns = 0;
  while (!jni_quit_requested) {
    svcSleepThread(4000000ULL);                  /* ~4ms poll (finer than 16.6ms frame) */
    /* Simulate a display vsync at a real 60Hz cadence so AndroidVSync::WaitForLast-
     * Presentation (while(*counter < target) cond_wait) advances at the panel rate,
     * NOT free-running -- an UNTHROTTLED present flood (counter racing arbitrarily
     * ahead) overwhelms the compositor (vi) and takes the whole system down. Bump the
     * counter once per elapsed 16.6ms of wall time (catch-up loop covers slow ticks);
     * the shim's 16ms cond re-poll then lets the waiter proceed at ~60fps. */
    if (g_vsync_counter) {
      uint64_t now = nx_now_ns();
      if (!vsync_last_ns) vsync_last_ns = now;
      while ((int64_t)(now - vsync_last_ns) >= 16666667LL) {
        __atomic_add_fetch(g_vsync_counter, 1, __ATOMIC_RELAXED);
        vsync_last_ns += 16666667ULL;
      }
    }
    void *tm = g_tm;
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {
      nx_clock_tick(tm);
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_start_clock_thread(void) {
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_vsync_counter = (volatile uint64_t *)(ub + OFF_ANDROID_VSYNC_COUNTER);
  g_unity_update_body = (void (*)(void *, double))(ub + OFF_TimeManager_Update_body);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  so_patch_code((void *)(ub + OFF_TimeManager_Update_entry), stub, sizeof stub);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+0x%x "
              "(newTime <- monotonic wallclock)\n", OFF_TimeManager_Update_entry);
}

/* audio warmup gate for opensles.c (frames since boot) */
static volatile uint32_t g_frame_count = 0;
uint32_t port_frame_count(void) { return g_frame_count; }

/* libunity ~19M + libil2cpp ~34M + headroom for relocated segments */
#define SO_REGION_BYTES (224u * 1024 * 1024)

static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { best_l = he - hs; best_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from the vln/cr3_nx base.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 384 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);
    /* On a small heap (4GB Switch ~3.1GB usable) the fixed arena starves the newlib
     * heap that il2cpp/mono/GC malloc from. Cap it at 30% of the usable heap so
     * newlib gets the majority. On 8GB this is a no-op; on 4GB the arena shrinks. */
    size_t usable    = size - so_zone - big_align;
    size_t arena_cap = ((usable * 30) / 100) & ~(big_align - 1);
    if (arena_sz > arena_cap) arena_sz = arena_cap;
    fake_heap_size = size - so_zone - arena_sz - big_align;
  } else {
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

/* Verify the essential data files exist. Laytonbmr uses the CLASSIC split-file
 * layout (no OBB, no data.unity3d): globalgamemanagers + level0 + the il2cpp
 * metadata, all pre-staged flat under assets/bin/Data/ by tools/stage_sd.py. */
static void check_data(void) {
  const char *files[] = {
    LIB_MAIN, LIB_UNITY, LIB_IL2CPP,
    "assets/bin/Data/Managed/Metadata/global-metadata.dat",
    "assets/bin/Data/globalgamemanagers",
    "assets/bin/Data/level0",
  };
  char path[768];
  struct stat st;
  for (unsigned i = 0; i < sizeof(files)/sizeof(*files); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, files[i]);
    if (stat(path, &st) < 0)
      fatal_error("Missing data file:\n%s\nCheck your SD card layout (see BUILD.md).\n"
                  "Run tools/stage_sd.py to assemble it from the APKs.", files[i]);
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  crx_resolve_imports(mod);
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;
static fn_vz       Unity_nativeUnityPlayerSetRunning;

/* Shrink Unity's memory-region granularity 256MB -> 64MB, applied IN-MEMORY to
 * libunity after it is loaded+relocated but before any allocator runs. Lets us
 * ship STOCK game data (no patched libunity.so on the SD) -- the .nro carries the
 * patch. Table auto-derived by tools/re/region_table.py (region_patch.h); 18 sites
 * across MemoryManager::VirtualAllocator's 2-level block table + the DynamicHeap/
 * Bucket/TLS/LowLevel allocator ctors, changing the region size/shift/mask so ~4x
 * more regions fit (a 4GB Switch fits). All sites are immediate-operand
 * instructions no relocation touches, so the in-memory word equals the file word.
 * We verify EVERY original word first and patch NOTHING on any mismatch -- a
 * different/updated libunity is left completely stock, never corrupted. */
static int nx_patch_unity_regions(uintptr_t ub) {
  for (int i = 0; i < LBMR_REGION_PATCH_N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + LBMR_REGION_PATCH[i].off);
    if (cur != LBMR_REGION_PATCH[i].from) {
      debugPrintf("[region] libunity mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all "
                  "(stock/updated libunity, 256MB regions)\n",
                  (unsigned)LBMR_REGION_PATCH[i].off, cur, LBMR_REGION_PATCH[i].from);
      return 0;
    }
  }
  for (int i = 0; i < LBMR_REGION_PATCH_N; i++)
    so_patch_code((void *)(ub + LBMR_REGION_PATCH[i].off),
                  &LBMR_REGION_PATCH[i].to, sizeof LBMR_REGION_PATCH[i].to);
  debugPrintf("[region] libunity memory granularity 256MB->64MB patched in-memory (%d sites)\n",
              LBMR_REGION_PATCH_N);
  return 1;
}

int main(int argc, char *argv[]) {
  (void)argc; (void)argv;
  socketInitializeDefault();
  debugPrintf("[boot] === laytonbmr_nx start (Unity 6000.0.58f2, build v1) ===\n");

  /* CWD fix: title-override / hbloader leaves cwd at the .nro folder or SD root.
   * Unity & il2cpp read many files through *relative* paths ("assets/bin/..."),
   * so chdir into DATA_ROOT. (Absolute "sdmc:/..." reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    int reach_meta = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_ggm  = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(rel): metadata=%d globalgamemanagers=%d\n", reach_meta, reach_ggm);
  }

  /* Load config.txt (portrait rotation, language) from the game folder; create it
   * with defaults on first run so the options are discoverable/editable on the SD. */
  {
    const char *cfg = DATA_ROOT "/" CONFIG_NAME;
    int rc = read_config(cfg);
    if (rc != 0) write_config(cfg);   /* -1 missing or 1 stale keys -> (re)write */
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[boot] phys: total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20), (unsigned)((tot - used) >> 20));
  }

  /* Arm the stack-region overcommit arena (svcMapMemory aliases heap pages into
   * the stack region; Unity reserves multi-GB of PROT_NONE but commits little).
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      pool = memalign(0x1000, OC_POOL_BYTES);
      if (pool && oc_arena_init(win, winsz, pool, OC_POOL_BYTES))
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(OC_POOL_BYTES >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20));
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* LANDSCAPE. Confirmed on hardware: Layton Brothers renders landscape and its single
   * orthographic camera is fine at the game's default orthographicSize (320) once we STOP
   * rotating -- the earlier "zoom" was entirely the portrait-rotation apparatus fighting a
   * landscape game. So render 16:9 landscape with NO rotation; the buffer maps 1:1 to the
   * panel. 1920x1080 (supersampled on a 720p handheld -> crisp). Tunable via config.txt
   * screen_width/screen_height (<=0 = auto). */
  if (config.screen_width > 0 && config.screen_height > 0) {
    screen_width = config.screen_width; screen_height = config.screen_height;
  } else {
    screen_width = 1920; screen_height = 1080;   /* 16:9 landscape, fills the panel */
  }
  debugPrintf("[gfx] boot mode=%s render=%dx%d landscape aspect=%.3f\n",
              appletGetOperationMode() == AppletOperationMode_Console ? "DOCKED" : "HANDHELD",
              screen_width, screen_height, (float)screen_width / (float)screen_height);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

  check_data();

  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  debugPrintf("[boot] modules finalized + flushed\n");

#if LBMR_FMOD_OPENSL_PATCH
  /* Force FMOD's OpenSL ES output (type 22 here) onto the opensles.c shim; Unity natively
   * picks 21 (Java AudioTrack, silent on Switch). Self-verifying. */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *site = (volatile uint32_t *)(ub + LBMR_FMOD_SETOUTPUT_SITE);
    if (*site == LBMR_FMOD_SETOUTPUT_FROM) {
      uint32_t to = LBMR_FMOD_SETOUTPUT_TO;
      so_patch_code((void *)site, &to, sizeof to);
    }
  }
#endif

#if LBMR_CHOREO_FREERUN
  /* Make ChoreographerBase::Get() return NULL so Unity never constructs a vsync
   * choreographer and the player loop free-runs (see the block comment above). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *g = (volatile uint32_t *)(ub + LBMR_CHOREO_GET_SITE);
    if (g[0] == LBMR_CHOREO_GET_W0 && g[1] == LBMR_CHOREO_GET_W1) {
      uint32_t movz0 = 0xd2800000u;   /* movz x0, #0 */
      uint32_t ret   = 0xd65f03c0u;   /* ret         */
      so_patch_code((void *)&g[0], &movz0, sizeof movz0);
      so_patch_code((void *)&g[1], &ret,   sizeof ret);
      debugPrintf("[choreo] ChoreographerBase::Get() -> return NULL @libunity+0x%x "
                  "(vsync free-run)\n", (unsigned)LBMR_CHOREO_GET_SITE);
    } else {
      debugPrintf("[choreo] Get() site mismatch @+0x%x: have 0x%08x,0x%08x -> NOT patched "
                  "(may hang on vsync)\n", (unsigned)LBMR_CHOREO_GET_SITE, g[0], g[1]);
    }
  }
#endif

#if LBMR_REGION_PATCH_ENABLE
  /* Region-granularity 256MB->64MB, in-memory, BEFORE any allocator/init runs, so a
   * 4GB Switch fits and the SD ships stock libunity.so (self-verifying). */
  nx_patch_unity_regions((uintptr_t)unity_mod.load_virtbase);
#endif

#if LBMR_PAYMENT_STUB
  /* Crack the payment boot-gate: stub CallAndroidPlugin.initPayment -> `ret` so the
   * boot coroutine doesn't NRE on the null billing nativeInstance (see block comment
   * above). Runtime addr = il2cpp load_virtbase + link-time RVA (so_util maps by
   * p_vaddr). Self-verifying against the original entry word. */
  {
    uintptr_t ib = (uintptr_t)il2cpp_mod.load_virtbase;
    volatile uint32_t *p = (volatile uint32_t *)(ib + LBMR_INITPAYMENT_RVA);
    if (*p == LBMR_INITPAYMENT_W0) {
      uint32_t ret = 0xd65f03c0u;   /* ret */
      so_patch_code((void *)p, &ret, sizeof ret);
      debugPrintf("[payment] initPayment stubbed -> ret @libil2cpp+0x%x (boot-gate cracked)\n",
                  (unsigned)LBMR_INITPAYMENT_RVA);
    } else {
      debugPrintf("[payment] initPayment site mismatch @+0x%x: have 0x%08x want 0x%08x -> "
                  "NOT patched (may black-screen on payment NRE)\n",
                  (unsigned)LBMR_INITPAYMENT_RVA, *p, (unsigned)LBMR_INITPAYMENT_W0);
    }
  }
#endif

  /* Touch input: patch the game's il2cpp UnityEngine.Input.* methods to return our Switch
   * touch state (Unity 6's JNI nativeInjectEvent accepts events but never reads touch coords,
   * so Input stays empty). android_native_feed_hid feeds the live touch each frame. */
  nx_install_input_hooks((uintptr_t)il2cpp_mod.load_virtbase);

  /* Language: replace LanguageParam.getCurrentLanguage() with the config-selected
   * eLanguage (AUTO follows the Switch system language). The game has no saved
   * language field, so this single hook re-languages every scene/resource load. */
  nx_install_language_hook((uintptr_t)il2cpp_mod.load_virtbase);

  /* Main thread runs init_array + the engine lifecycle; give it its own stable
   * bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered VADDRs) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  Unity_nativeUnityPlayerSetRunning = (fn_vz)   UNITY_RESOLVE(unity_mod, OFF_nativeUnityPlayerSetRunning);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p inject=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender, (void *)Unity_nativeInjectEvent);

  install_bionic_tls(main_tls);

  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST: runs jni::Initialize(), caching
   * the JavaVM into libunity's internal JNI manager. Without it the ScopedJNI inside
   * initJni gets a NULL JNIEnv and crashes. */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling libunity JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime so managed AndroidJNI / AndroidJava*
   * calls resolve. We do NOT call libil2cpp's JNI_OnLoad (its first act is a PLT log
   * call that may mis-bind); its only essential effects, RE'd from THIS libil2cpp's
   * JNI_OnLoad @0xd64750, are two global stores: cache the VM at il2cpp+0x20a7f80,
   * and store the JNI-handler fn-ptr (il2cpp+0xd64794) at il2cpp+0x20a7f60 (set by
   * the helper @0xd63b18: `str x0,[x8,#0xf60]`). Replicate those directly. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
    *(void **)(b + 0x20a7f80) = fake_vm;                 /* JavaVM cache            */
    *(void **)(b + 0x20a7f60) = (void *)(b + 0xd64794);  /* JNI handler fn pointer  */
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p, handler=%p)\n",
                fake_vm, (void *)(b + 0xd64794));
  }

  /* Unity 6 initJni gains a trailing int (Context,int); it's a mode flag (cmp #1
   * gates only a log string, then tail-calls the real init). Pass 0 (default). */
  debugPrintf("[boot] calling initJni(ctx, 0)...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj, 0);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* The Unity player loop only advances Update/coroutines/animation when RESUMED
   * and FOCUSED (the Java UnityPlayer drives this from onResume()/onWindowFocus). */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  /* Unity 6 lifecycle: mark the player running so the engine treats itself as live. */
  if (Unity_nativeUnityPlayerSetRunning)
    Unity_nativeUnityPlayerSetRunning(fake_env, fake_unityplayer_thiz, 1);
  debugPrintf("[boot] resumed + focus=true + setRunning(1)\n");

  /* CRITICAL ORDER: install GC-disable + clock fix BEFORE the first nativeRender.
   * The game allocates managed objects on its first frames, which can trigger a
   * Boehm GC; the GC stops the world with POSIX signals Switch never delivers, so
   * nativeRender would never return. il2cpp is initialized by now (initJni ran
   * il2cpp_init), so the exported GC API is live. */
  {
    /* Disable the Boehm GC entirely. il2cpp_gc_set_mode(DISABLED=1) calls GC_disable
     * and turns off the incremental collector (its signal-based dirty barrier is also
     * broken on Switch); il2cpp_gc_disable() belt-and-suspenders it. */
    typedef void (*fn_set_mode)(int);
    typedef void (*fn_void)(void);
    fn_set_mode il2cpp_gc_set_mode = (fn_set_mode)so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    fn_void     il2cpp_gc_disable  = (fn_void)    so_try_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
    if (il2cpp_gc_set_mode) { il2cpp_gc_set_mode(1); debugPrintf("[boot] il2cpp_gc_set_mode(DISABLED)\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_set_mode not found\n");
    if (il2cpp_gc_disable)  { il2cpp_gc_disable();   debugPrintf("[boot] il2cpp_gc_disable() -> GC OFF\n"); }
    else debugPrintf("[boot] WARNING: il2cpp_gc_disable not found\n");

    /* Native engine-clock fix: drive TimeManager::Update with a live newTime so
     * deltaTime / realtimeSinceStartup advance for native readers (PreloadManager),
     * unblocking async scene loads. Explicit GC.Collect() calls are handled by the
     * stop-the-world bridge in libc_shim.c (pthread_kill_gc). */
    nx_install_time_fix();
  }
  /* Keep the engine clock advancing even while nativeRender blocks on a synchronous
   * scene-load job (the frame-2 wall). Frame pacing: with no Java/NDK Choreographer
   * delivered, jni_fake returns null for Choreographer.getInstance() so Unity renders
   * on our loop cadence (see LAYTONBMR_PORT_HANDOFF -- a hardware-bringup unknown for
   * Unity 6's Swappy frame pacing). */
  nx_start_clock_thread();
  debugPrintf("[boot] GC off + clock fix installed + clock thread started; entering render loop\n");

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");
  diag_watchdog_start();

  int frame = 0;
  uint64_t next_frame_ns = nx_now_ns();
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);
    g_frame_count++;
    android_native_update_mode();
    android_native_feed_hid();
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame < 5 || (frame % 120) == 0) debugPrintf("[boot] frame %d rendered\n", frame);
    frame++;
    /* FRAME LIMITER (~60fps). With no real display vsync our loop would spin as fast
     * as it can, flooding the GPU/compositor with buffer submissions -> a full-system
     * crash on the first real content present. Pace to the panel's 60Hz: sleep until
     * the next 16.6ms boundary. Belt-and-suspenders with the 60Hz vsync counter. */
    next_frame_ns += 16666667ULL;
    uint64_t now = nx_now_ns();
    if ((int64_t)(next_frame_ns - now) > 0) svcSleepThread(next_frame_ns - now);
    else next_frame_ns = now;   /* fell behind: resync, don't accumulate debt */
  }

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
