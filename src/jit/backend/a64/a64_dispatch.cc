#include "jit/backend/a64/a64_local.h"

extern "C" {
#include "core/core.h"
#include "jit/jit.h"
#include "jit/jit_guest.h"
}

static inline void **cache_slot(struct a64_backend *backend, uint32_t addr) {
  return &backend->cache[(addr & backend->cache_mask) >> backend->cache_shift];
}

/* address the assembler will put the next instruction at */
static void *masm_pc(MacroAssembler &e) {
  return e.GetBuffer()->GetStartAddress<uint8_t *>() +
         e.GetSizeOfCodeGenerated();
}

/* branch back to target with the fixed offset the assembler doesn't know */
static void branch_back(MacroAssembler &e, const void *target) {
  int64_t imm = ((const uint8_t *)target - (uint8_t *)masm_pc(e)) >> 2;
  e.b(imm);
}

void a64_dispatch_restore_edge(struct jit_backend *base, void *code,
                               uint32_t dst) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);

  int64_t imm = ((const uint8_t *)backend->dispatch_static - (uint8_t *)code) >> 2;
  *(uint32_t *)code = 0x94000000 | (uint32_t)(imm & 0x3ffffff);
  __builtin___clear_cache((char *)code, (char *)code + 4);
}

void a64_dispatch_patch_edge(struct jit_backend *base, void *code, void *dst) {
  int64_t imm = ((const uint8_t *)dst - (uint8_t *)code) >> 2;
  *(uint32_t *)code = 0x14000000 | (uint32_t)(imm & 0x3ffffff);
  __builtin___clear_cache((char *)code, (char *)code + 4);
}

void a64_dispatch_invalidate_code(struct jit_backend *base, uint32_t addr) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  *cache_slot(backend, addr) = backend->dispatch_compile;
}

void a64_dispatch_cache_code(struct jit_backend *base, uint32_t addr,
                             void *code) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  void **entry = cache_slot(backend, addr);
  CHECK_EQ(*entry, backend->dispatch_compile);
  *entry = code;
}

void *a64_dispatch_lookup_code(struct jit_backend *base, uint32_t addr) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  return *cache_slot(backend, addr);
}

void a64_dispatch_run_code(struct jit_backend *base, int cycles) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  backend->dispatch_enter(cycles);
}

void a64_dispatch_emit_thunks(struct a64_backend *backend) {
  struct jit_guest *guest = backend->base.guest;

  MacroAssembler masm(backend->code, backend->code_size);

  /* dynamic dispatch: pc -> cache[pc & mask] -> br */
  backend->dispatch_dynamic = masm_pc(masm);
  masm.Ldr(x16, (uint64_t)backend->cache);
  masm.Ldr(w17, MemOperand(x28, guest->offset_pc));
  masm.And(w17, w17, backend->cache_mask);
  masm.Lsr(w17, w17, backend->cache_shift);
  masm.Ldr(x16, MemOperand(x16, x17, LSL, 3));
  masm.Br(x16);

  /* static dispatch: link the edge from the caller, then fall through */
  backend->dispatch_static = masm_pc(masm);
  masm.Ldr(x0, (uint64_t)guest->data);
  masm.Sub(x1, x30, 4);
  /* the branch site passes its target in the word after it. save it first,
     the dynamic dispatch after a compile reads it from the context */
  masm.Ldr(w2, MemOperand(x30));
  masm.Str(w2, MemOperand(x28, guest->offset_pc));
  masm.Ldr(x16, (uint64_t)guest->link_code);
  masm.Blr(x16);
  branch_back(masm, backend->dispatch_dynamic);

  /* compile thunk: default cache entry for uncompiled blocks. the guest may
     run the code instead of compiling, returning the cycles it took, and then
     the budget/irqs are checked as after a block */
  Label irq;
  backend->dispatch_compile = masm_pc(masm);
  masm.Ldr(x0, (uint64_t)guest->data);
  masm.Ldr(w1, MemOperand(x28, guest->offset_pc));
  masm.Ldr(x16, (uint64_t)guest->compile_code);
  masm.Blr(x16);
  {
    int64_t imm = ((uint8_t *)backend->dispatch_dynamic - (uint8_t *)masm_pc(masm)) >> 2;
    masm.cbz(w0, imm);
  }
  masm.Sub(w26, w26, w0);
  masm.B(&irq);

  /* interrupt thunk. blocks check the budget before they run, so check it
     here first too, since a block can yield on its way out */
  Label out;
  backend->dispatch_interrupt = masm_pc(masm);
  masm.Bind(&irq);
  masm.Tbnz(w26, 31, &out);
  masm.Ldr(x0, (uint64_t)guest->data);
  masm.Ldr(x16, (uint64_t)guest->check_interrupts);
  masm.Blr(x16);
  branch_back(masm, backend->dispatch_dynamic);

  /* entry point: build the frame, set fixed regs, jump to pc, taking any irq
     raised since the last run first */
  backend->dispatch_enter = (void (*)(int32_t))masm_pc(masm);
  a64_backend_save_regs(backend, masm, 0);
  masm.Sub(sp, sp, A64_STACK_SIZE);
  masm.Ldr(x28, (uint64_t)guest->ctx);
  masm.Ldr(x27, (uint64_t)guest->membase);
  masm.Mov(w26, w0);
  masm.Str(wzr, MemOperand(x28, guest->offset_instrs));
  branch_back(masm, backend->dispatch_interrupt);

  /* exit point: tear the frame down and return */
  backend->dispatch_exit = masm_pc(masm);
  masm.Bind(&out);
  masm.Str(w26, MemOperand(x28, guest->offset_cycles));
  masm.Add(sp, sp, A64_STACK_SIZE);
  a64_backend_restore_regs(backend, masm, 0);
  masm.Ret();

  a64_backend_emit_thunks(backend, masm);

  masm.FinalizeCode();

  CHECK_LT((int)masm.GetSizeOfCodeGenerated(), A64_THUNK_SIZE);
  backend->code_offset = A64_THUNK_SIZE;

  for (int i = 0; i < backend->cache_size; i++) {
    backend->cache[i] = backend->dispatch_compile;
  }
}

void a64_dispatch_shutdown(struct a64_backend *backend) {
  free(backend->cache);
}

void a64_dispatch_init(struct a64_backend *backend) {
  struct jit_guest *guest = backend->base.guest;

  backend->cache_mask = guest->addr_mask;
  backend->cache_shift = ctz32(guest->addr_mask);
  backend->cache_size = (backend->cache_mask >> backend->cache_shift) + 1;
  backend->cache = (void **)malloc(backend->cache_size * sizeof(void *));
}
