#ifndef A64_LOCAL_H
#define A64_LOCAL_H

#include <inttypes.h>
#include <functional>
#include "aarch64/macro-assembler-aarch64.h"
#include "aarch64/operands-aarch64.h"

extern "C" {
#include "jit/jit_backend.h"
#include "jit/jit_guest.h"
}

using namespace vixl;
using namespace vixl::aarch64;

/*
 * a64 backend
 */
struct a64_backend {
  struct jit_backend base;

  /* code cache, indexed by guest block address */
  uint32_t cache_mask;
  int cache_shift;
  int cache_size;
  void **cache;

  /* codegen state. one MacroAssembler is constructed per assemble_code call
     over the remaining space in the code buffer */
  uint8_t *code;
  int code_size;
  int code_offset;

  /* dispatch entry points */
  void *dispatch_dynamic;
  void *dispatch_static;
  void *dispatch_compile;
  void *dispatch_interrupt;
  void (*dispatch_enter)(int32_t);
  void *dispatch_exit;

  /* transient state used while emitting one ir unit: map of block labels,
     the slow paths placed after the unit's code, and whether the block being
     emitted makes calls (the only way its code can raise an interrupt) */
  void *labels;
  void *stubs;
  int check_interrupts;

  /* fastmem slow-path thunks */
  void *load_thunk[31];
  void *store_thunk;
};

#define A64_THUNK_SIZE 16384

/* stack frame: saved lr/fp + callee-saved regs, then scratch for ir locals */
#define A64_STACK_LOCALS 160
#define A64_STACK_SIZE 2048
#define A64_STACK_FRAME (A64_STACK_LOCALS + A64_STACK_SIZE)

/*
 * fixed registers
 *
 *   x0-x7   argument registers
 *   x16/x17 intra-procedure scratch
 *   x27     guest memory base
 *   x28     guest context
 *   x29/x30 frame pointer / link register
 */
Register a64_backend_reg(struct a64_backend *backend, const struct ir_value *v);
VRegister a64_backend_vreg(struct a64_backend *backend,
                           const struct ir_value *v);

/* fixed registers shared with the emitters */
extern const Register a64_arg0, a64_arg1, a64_arg2, a64_arg3, a64_guestctx,
    a64_guestmem, a64_cycles;

int a64_backend_save_regs(struct a64_backend *backend, MacroAssembler &e,
                          int mask);
void a64_backend_restore_regs(struct a64_backend *backend, MacroAssembler &e,
                              int mask);
void a64_backend_load_mem(struct a64_backend *backend, MacroAssembler &e,
                          const struct ir_value *dst, const MemOperand &src);
void a64_backend_store_mem(struct a64_backend *backend, MacroAssembler &e,
                           const MemOperand &dst, const struct ir_value *src);
void a64_backend_mov_value(struct a64_backend *backend, MacroAssembler &e,
                           const Register &dst, const struct ir_value *v);
void a64_backend_block_label(char *name, size_t size, struct ir_block *block);
void a64_backend_emit_branch(struct a64_backend *backend, MacroAssembler &e,
                             struct ir *ir, const struct ir_value *target);
Label *a64_backend_stub(struct a64_backend *backend, int write_pc, uint32_t pc,
                        const void *thunk);
Label *a64_backend_slow_path(struct a64_backend *backend, Label **ret,
                             std::function<void(MacroAssembler &)> code);

/* fastmem thunks emitted into the reserved thunk area */
void a64_backend_emit_thunks(struct a64_backend *backend, MacroAssembler &e);

/*
 * dispatch
 */
void a64_dispatch_init(struct a64_backend *backend);
void a64_dispatch_shutdown(struct a64_backend *backend);
void a64_dispatch_emit_thunks(struct a64_backend *backend);
void a64_dispatch_run_code(struct jit_backend *base, int cycles);
void *a64_dispatch_lookup_code(struct jit_backend *base, uint32_t addr);
void a64_dispatch_cache_code(struct jit_backend *base, uint32_t addr,
                             void *code);
void a64_dispatch_invalidate_code(struct jit_backend *base, uint32_t addr);
void a64_dispatch_patch_edge(struct jit_backend *base, void *code, void *dst);
void a64_dispatch_restore_edge(struct jit_backend *base, void *code,
                               uint32_t dst);

/*
 * emitters
 */
typedef void (*a64_emit_cb)(struct a64_backend *, MacroAssembler &, struct ir *,
                            struct ir_instr *);
extern struct jit_emitter a64_emitters[IR_NUM_OPS];

#endif
