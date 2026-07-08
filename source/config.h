/* config.h -- LAYTON BROTHERS MYSTERY ROOM Switch wrapper configuration
 * (forked from the vln_nx / ZOOKEEPER DX ports, themselves from cr3_nx / max_nx).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// Newlib heap for the engine/libc++/il2cpp managed heaps; the rest goes to the .so loader.
#define MEMORY_MB 768

// mmap arena. Unity reserves aligned pools by over-mmapping then trimming the head/tail; a plain
// malloc-per-mmap would free the whole block on head-trim and corrupt the kept middle (TLSF
// "next_free == NULL" crash). We back anonymous mmaps from an aligned arena with a per-page bitmap
// so sub-range munmap frees only the trimmed pages. ALIGN must match Unity's region granularity.
#define MMAP_ARENA_ALIGN    ((size_t)64 * 1024 * 1024)

// Unity's VirtualAllocator defaults to 256MB region granularity; we patch it to 64MB in-memory at
// boot (main.c nx_patch_unity_regions) so more regions fit and a 4GB Switch boots. Ships stock libunity.
#define MMAP_ARENA_RESERVE  ((size_t)1792 * 1024 * 1024)  // heap-backed cap (28x64MB)

// Stack-region overcommit arena (libc_shim.c): svcMapMemory can only alias into the ~2GB stack
// region, so big PROT_NONE reservations live there, backed by a small heap commit-pool.
#define OC_WINDOW_BYTES     ((size_t)1536 * 1024 * 1024)
#define OC_POOL_BYTES       ((size_t) 384 * 1024 * 1024)

// Overcommit (alias-region) mode window -- unused on title-override, kept for base parity.
#define MMAP_VIRT_RESERVE   ((size_t)6144 * 1024 * 1024)
#define OVERCOMMIT_HEAP_MB  608u

// --- CR3 leftovers (inherited, unused by Unity) ---
#define SO_NAME      "libcrx.so"
#define SO_CPP_NAME  "libc++_shared.so"
#define MAIN_MVGL    "main.10007.android.mvgl"

// Game identity for the JNI Context shim. No OBB: classic split-file data (globalgamemanagers,
// level0..N, sharedassets*) merged flat under assets/bin/Data/ by stage_sd.py.
#define LBMR_PACKAGE       "com.Level_5.MysteryRoomENG"
#define LBMR_VERSION_CODE  22           // APK versionCode
#define LBMR_VERSION_NAME  "1.1.3"

#define CONFIG_NAME "config.txt"
#define LOG_NAME    "sdmc:/switch/laytonbmr_nx/debug.log"

// Game data root == the .nro's own folder, matching the SD convention used by
// the sibling SoLoader ports: nro + libs + assets all live in
// sdmc:/switch/laytonbmr_nx/. Returned for getenv("HOME")/getpwuid() and the
// managed data path.
#define GAME_HOME   "sdmc:/switch/laytonbmr_nx"

// flip to 1 (and rebuild) for on-hardware file logging (debug.log)
#define DEBUG_LOG 0

extern int screen_width;
extern int screen_height;

// Language selection (config.txt "language" value). The shipped Worldwide package
// (com.Level_5.MysteryRoomENG) bundles English/French/Spanish/German/Italian; there
// are no Japanese assets. 0 = AUTO: derive from the Switch system language. These
// values are mapped to the game's eLanguage id inside nx_pick_elanguage(); the old
// EN=2 numbering is kept so existing config.txt files stay English (no silent switch).
#define LANG_AUTO 0
#define LANG_JA   1   /* Japanese -- not in the Worldwide package; treated as English */
#define LANG_EN   2
#define LANG_FR   3
#define LANG_ES   4
#define LANG_DE   5
#define LANG_IT   6

typedef struct {
  int screen_width;
  int screen_height;
  int language;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
