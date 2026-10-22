#ifndef JIT_GUEST_H
#define JIT_GUEST_H

#include <stdint.h>

typedef uint32_t (*mem_read_cb)(void *, uint32_t, uint32_t);
typedef void (*mem_write_cb)(void *, uint32_t, uint32_t, uint32_t);

/* returns the cycles taken if the guest ran the code rather than compile it */
typedef int (*jit_compile_cb)(void *, uint32_t);
typedef void (*jit_link_cb)(void *, uint32_t);
typedef void (*jit_interrupt_cb)(void *);

struct memory;

struct jit_guest {
  /* mask used to directly map each guest address to a block of code */
  uint32_t addr_mask;

  /* memory interface used by both the frontend and backend */
  void *ctx;
  void *membase;
  struct memory *mem;
  void (*lookup)(struct memory *, uint32_t, void **, uint8_t **, mem_read_cb *,
                 mem_write_cb *);
  uint8_t (*r8)(struct memory *, uint32_t);
  uint16_t (*r16)(struct memory *, uint32_t);
  uint32_t (*r32)(struct memory *, uint32_t);
  uint64_t (*r64)(struct memory *, uint32_t);
  void (*w8)(struct memory *, uint32_t, uint8_t);
  void (*w16)(struct memory *, uint32_t, uint16_t);
  void (*w32)(struct memory *, uint32_t, uint32_t);
  void (*w64)(struct memory *, uint32_t, uint64_t);

  /* optional window that 32-bit stores to a non-constant address may write
     directly, skipping w32: when (addr >> 26) == store_window_tag, the store
     goes to store_window + (addr & store_window_mask) */
  uint8_t *store_window;
  uint32_t store_window_tag;
  uint32_t store_window_mask;

  /* runtime interface used by the backend and dispatch */
  void *data;
  int offset_pc;
  int offset_cycles;
  int offset_instrs;
  int offset_interrupts;
  jit_compile_cb compile_code;
  jit_link_cb link_code;
  jit_interrupt_cb check_interrupts;
};

#endif
