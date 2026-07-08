/* region_patch.h -- Laytonbmr libunity.so 256MB->64MB memory-region granularity
 * patch table (Unity 6000.0.58f2, arm64-v8a, engine-stripped shipped build).
 *
 * AUTO-DERIVED by tools/re/region_table.py from the editor's symbolicated
 * arm64-v8a Release libunity (fingerprinted into the shipped binary). Do not edit
 * by hand; re-run region_table.py if the game updates.
 *
 * Unity's MemoryManager::VirtualAllocator indexes a 2-level block table at a 256MB
 * region granularity (RE-confirmed byte-identical math to 2021.3/vln). On a 4GB
 * Switch the boot reservation set does not fit; rewriting the granularity-encoding
 * immediates to 64MB lets ~4x more regions fit. 18 sites across the allocator
 * ctors + block-table index/reserve/commit paths. Every `from` word is an
 * immediate-operand instruction that no relocation touches, so the in-memory word
 * equals this file word; nx_patch_unity_regions() verifies ALL 18 before touching
 * any, and patches nothing on a single mismatch -- a stock/mismatched/updated
 * libunity is left completely untouched (runs 256MB regions), never corrupted.
 *
 * Transforms (value-based, reproducing vln's proven table exactly):
 *   BITF (UBFM)  immr 28->26 ; imms -2 unless 0x3f (lsr keeps its msb)
 *   LOGIMM       high-mask clearing low 28/36 bits -> clear 2 fewer
 *   MOV (hw=1)   imm16 0x1000->0x400 (256MB const) ; 0xf000->0xfc00 (256MB-1 / -mask)
 *   SHREG        add/sub shift amount 28->26
 */
#ifndef REGION_PATCH_H
#define REGION_PATCH_H
#include <stdint.h>

/* {link-time VADDR in shipped libunity, original word (256MB), patched word (64MB)}
 * VADDR = load_virtbase + off at runtime. libunity .text VA = file_off + 0x4000
 * (nonzero VA-fileoffset delta, unlike vln where they coincided). */
static const struct { uint32_t off, from, to; } LBMR_REGION_PATCH[] = {
  {0x44f8a8, 0x12be0009, 0x12bf8009},  /* MOV    LocalLowLevelAllocator::ReserveMemoryBlock+0x88 (256MB-1 mask hi) */
  {0x44f8b8, 0x92648d35, 0x92669535},  /* LOGIMM LocalLowLevelAllocator::ReserveMemoryBlock+0x98 (~low28 mask)     */
  {0x450198, 0x52a20008, 0x52a08008},  /* MOV    TLSAllocator::ThreadInitialize+0x10          (256MB const)        */
  {0x451ba0, 0x52a20009, 0x52a08009},  /* MOV    BucketAllocator::ctor+0xe8                   (256MB const)        */
  {0x454624, 0x52a20009, 0x52a08009},  /* MOV    DynamicHeapAllocator::ctor+0x4c              (256MB region size)  */
  {0x454ae0, 0x12be000d, 0x12bf800d},  /* MOV    DynamicHeapAllocator::RequestLargeAllocMemory+0x50 (256MB-1 mask hi) */
  {0x454b0c, 0x92648d36, 0x92669536},  /* LOGIMM DynamicHeapAllocator::RequestLargeAllocMemory+0x7c (~low28 mask)  */
  {0x456ae0, 0xd35cdc33, 0xd35ad433},  /* BITF   VirtualAllocator::MarkMemoryBlocks+0x10      (ubfx #28,#28)       */
  {0x456ae8, 0xd35cfd15, 0xd35afd15},  /* BITF   VirtualAllocator::MarkMemoryBlocks+0x18      (lsr #28)            */
  {0x456b8c, 0x52a20008, 0x52a08008},  /* MOV    VirtualAllocator::ReserveMemoryBlock+0x50     (256MB const)        */
  {0x456ec0, 0xd35cfc28, 0xd35afc28},  /* BITF   VirtualAllocator::GetMemoryBlockFromPointer+0x0  (lsr #28)        */
  {0x456ed0, 0x92646c28, 0x92667428},  /* LOGIMM VirtualAllocator::GetMemoryBlockFromPointer+0x10 (~low28 mask)    */
  {0x456ed8, 0xd35c9c2a, 0xd35a942a},  /* BITF   VirtualAllocator::GetMemoryBlockFromPointer+0x18 (ubfx #28,#12)   */
  {0x456eec, 0xd35cdc29, 0xd35ad429},  /* BITF   VirtualAllocator::GetMemoryBlockFromPointer+0x2c (ubfx #28,#28)   */
  {0x456ef0, 0xb25c6feb, 0xb25e77eb},  /* LOGIMM VirtualAllocator::GetMemoryBlockFromPointer+0x30 (-255*gran hi)   */
  {0x456ef4, 0xf2a2000b, 0xf2a0800b},  /* MOV    VirtualAllocator::GetMemoryBlockFromPointer+0x34 (movk 256MB)     */
  {0x456f38, 0xcb0a7108, 0xcb0a6908},  /* SHREG  VirtualAllocator::GetMemoryBlockFromPointer+0x78 (sub lsl #28)    */
  {0x456f5c, 0xd35c9c29, 0xd35a9429},  /* BITF   VirtualAllocator::GetBlockInfoFromPointer+0x10 (ubfx #28,#12)     */
};
#define LBMR_REGION_PATCH_N ((int)(sizeof(LBMR_REGION_PATCH)/sizeof(LBMR_REGION_PATCH[0])))

#endif /* REGION_PATCH_H */
