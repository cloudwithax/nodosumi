#include "guest/memory.h"
#include "guest/pvr/ta.h"
#include "guest/sh4/sh4.h"
#include "jit/jit.h"

#if 0
#define LOG_CCN LOG_INFO
#else
#define LOG_CCN(...)
#endif

/* with OIX, bit 25, rather than bit 13, determines which 4kb bank to use */
#define CACHE_OFFSET(addr, OIX) \
  ((OIX ? ((addr & 0x2000000) >> 13) : ((addr & 0x2000) >> 1)) | (addr & 0xfff))

static void sh4_ccn_reset(struct sh4 *sh4) {
  /* FIXME this isn't right. when the IC is reset a pending flag is set and the
     cache is actually reset at the end of the current block. however, the docs
     for the SH4 IC state "After CCR is updated, an instruction that performs
     data access to the P0, P1, P3, or U0 area should be located at least four
     instructions after the CCR update instruction. Also, a branch instruction
     to the P0, P1, P3, or U0 area should be located at least eight instructions
     after the CCR update instruction."

     i'm not sure if this will ever actually cause problems, but there may need
     to be some const prop that tries to detect writes to CCR and prematurely
     end the block */
  LOG_INFO("sh4_ccn_reset");

  jit_invalidate_code(sh4->jit);
}

/* with the mmu off, a store queue's flushes go to the area in its QACR. when
   that's the ta's, they're sent straight to it */
void sh4_ccn_sq_updated(struct sh4 *sh4) {
  union mmucr mmucr = {sh4->reg[MMUCR]};
  int area0 = (sh4->reg[QACR0] & 0x1c) >> 2;
  int area1 = (sh4->reg[QACR1] & 0x1c) >> 2;

  sh4->sq_ta[0] = !mmucr.AT && area0 == 4 ? sh4->dc->ta : NULL;
  sh4->sq_ta[1] = !mmucr.AT && area1 == 4 ? sh4->dc->ta : NULL;
}

void sh4_ccn_pref(struct sh4 *sh4, uint32_t addr) {
  uint32_t sqi = (addr & 0x20) >> 5;

  /* nearly every store queue goes to the ta's polygon fifo, below its yuv and
     texture paths */
  struct ta *ta = sh4->sq_ta[sqi];
  if (ta && !(addr & 0x01800000)) {
    ta_poly_write32(ta, (const uint8_t *)sh4->sq[sqi]);
    return;
  }

  struct memory *mem = sh4->dc->mem;

  /* make sure this is a sq related prefetch */
  DCHECK(addr >= 0xe0000000 && addr <= 0xe3ffffff);

  uint32_t dst = 0x0;

  /* the registers are read straight from reg rather than through their
     pointers, this runs for every store queue flush */
  union mmucr mmucr = {sh4->reg[MMUCR]};

  if (mmucr.AT) {
    /* get upper 12 bits from UTLB */
    uint32_t vpn = addr >> 20;
    dst = sh4->utlb_sq_map[vpn & 0x3f];

    /* get lower 20 bits from original address */
    dst |= addr & 0xfffe0;
  } else {
    /* get upper 6 bits from QACR* registers */
    if (sqi) {
      dst = (sh4->reg[QACR1] & 0x1c) << 24;
    } else {
      dst = (sh4->reg[QACR0] & 0x1c) << 24;
    }

    /* get lower 26 bits from original address */
    dst |= addr & 0x3ffffe0;
  }

  /* the mmu's translations can reach the polygon fifo too */
  if ((dst >> 26) == 4 && !(dst & 0x01800000)) {
    ta_poly_write32(sh4->dc->ta, (const uint8_t *)sh4->sq[sqi]);
    return;
  }

  sh4_memcpy_to_guest(mem, dst, sh4->sq[sqi], 32);
}

uint32_t sh4_ccn_cache_read(struct sh4 *sh4, uint32_t addr, uint32_t mask) {
  if (!sh4->CCR->ORA) {
    LOG_WARNING("sh4_ccn_cache_read while on-chip RAM is disabled");
    /* need to write a test for this, but I'm guessing garbage is returned in
       this case */
    return 0x0;
  }

  addr = CACHE_OFFSET(addr, sh4->CCR->OIX);
  return READ_DATA(&sh4->ctx.cache[addr]);
}

void sh4_ccn_cache_write(struct sh4 *sh4, uint32_t addr, uint32_t data,
                         uint32_t mask) {
  if (!sh4->CCR->ORA) {
    LOG_WARNING("sh4_ccn_cache_write while on-chip RAM is disabled");
    return;
  }

  CHECK_EQ(sh4->CCR->ORA, 1u);
  addr = CACHE_OFFSET(addr, sh4->CCR->OIX);
  WRITE_DATA(&sh4->ctx.cache[addr]);
}

uint32_t sh4_ccn_sq_read(struct sh4 *sh4, uint32_t addr, uint32_t mask) {
  uint32_t sqi = (addr & 0x20) >> 5;
  uint32_t idx = (addr & 0x1c) >> 2;
  CHECK_EQ(mask, 0xffffffff);
  return sh4->sq[sqi][idx];
}

void sh4_ccn_sq_write(struct sh4 *sh4, uint32_t addr, uint32_t data,
                      uint32_t mask) {
  uint32_t sqi = (addr & 0x20) >> 5;
  uint32_t idx = (addr & 0x1c) >> 2;
  CHECK_EQ(mask, 0xffffffff);
  sh4->sq[sqi][idx] = data;
}

uint32_t sh4_ccn_icache_read(struct sh4 *sh4, uint32_t addr, uint32_t mask) {
  LOG_CCN("sh4_ccn_icache_read 0x%08x", addr);

  /* return an invalid entry */
  return 0;
}

void sh4_ccn_icache_write(struct sh4 *sh4, uint32_t addr, uint32_t data,
                          uint32_t mask) {
  LOG_CCN("sh4_ccn_icache_write 0x%08x", addr);

  /* ignore */
}

uint32_t sh4_ccn_ocache_read(struct sh4 *sh4, uint32_t addr, uint32_t mask) {
  LOG_CCN("sh4_ccn_ocache_read 0x%08x", addr);

  /* return an invalid entry */
  return 0;
}

void sh4_ccn_ocache_write(struct sh4 *sh4, uint32_t addr, uint32_t data,
                          uint32_t mask) {
  LOG_CCN("sh4_ccn_ocache_write 0x%08x", addr);

  /* ignore */
}

REG_W32(sh4_cb, MMUCR) {
  struct sh4 *sh4 = dc->sh4;

  sh4->MMUCR->full = value;
  sh4_ccn_sq_updated(sh4);

  if (sh4->MMUCR->AT) {
    LOG_WARNING("MMU not fully supported");
  }
}

REG_W32(sh4_cb, QACR0) {
  struct sh4 *sh4 = dc->sh4;

  *sh4->QACR0 = value;
  sh4_ccn_sq_updated(sh4);
}

REG_W32(sh4_cb, QACR1) {
  struct sh4 *sh4 = dc->sh4;

  *sh4->QACR1 = value;
  sh4_ccn_sq_updated(sh4);
}

REG_W32(sh4_cb, CCR) {
  struct sh4 *sh4 = dc->sh4;

  /* TODO check for cache toggle
  union ccr CCR_OLD = *sh4->CCR;*/
  sh4->CCR->full = value;

  if (sh4->CCR->ICI) {
    sh4_ccn_reset(sh4);
  }

  /* ICI / OCI is read-only */
  sh4->CCR->ICI = 0;
  sh4->CCR->OCI = 0;
}
