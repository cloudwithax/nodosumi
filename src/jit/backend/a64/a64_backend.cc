#include "jit/backend/a64/a64_local.h"

#include <functional>
#include <list>
#include <map>

extern "C" {
#include "core/core.h"
#include "core/exception_handler.h"
#include "core/memory.h"
#include "jit/backend/a64/a64_backend.h"
#include "jit/ir/ir.h"
#include "jit/jit.h"
#include "jit/jit_backend.h"
#include "jit/jit_guest.h"
}

/* fixed regs, see a64_local.h */
const Register a64_arg0(0, 64);
const Register a64_arg1(1, 64);
const Register a64_arg2(2, 64);
const Register a64_arg3(3, 64);
const Register a64_guestctx(28, 64);
const Register a64_guestmem(27, 64);
const Register a64_cycles(26, 64);

/* clang-format off */
static const struct jit_register a64_registers[] = {
    {"x0",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x0},
    {"x1",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x1},
    {"x2",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x2},
    {"x3",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x3},
    {"x4",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x4},
    {"x5",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x5},
    {"x6",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x6},
    {"x7",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x7},
    {"x8",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x8},
    {"x9",  JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x9},
    {"x10", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x10},
    {"x11", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x11},
    {"x12", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x12},
    {"x13", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x13},
    {"x14", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x14},
    {"x15", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_I64, &x15},
    {"x16", JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x16},
    {"x17", JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x17},
    {"x18", JIT_RESERVED | JIT_CALLEE_SAVE | JIT_REG_I64, &x18},
    {"x19", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x19},
    {"x20", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x20},
    {"x21", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x21},
    {"x22", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x22},
    {"x23", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x23},
    {"x24", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x24},
    {"x25", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_I64, &x25},
    {"x26", JIT_RESERVED | JIT_CALLEE_SAVE | JIT_REG_I64, &x26},
    {"x27", JIT_RESERVED | JIT_CALLEE_SAVE | JIT_REG_I64, &x27},
    {"x28", JIT_RESERVED | JIT_CALLEE_SAVE | JIT_REG_I64, &x28},
    {"x29", JIT_RESERVED | JIT_CALLEE_SAVE | JIT_REG_I64, &x29},
    {"x30", JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_I64, &x30},
    {"v0",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v0},
    {"v1",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v1},
    {"v2",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v2},
    {"v3",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v3},
    {"v4",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v4},
    {"v5",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v5},
    {"v6",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v6},
    {"v7",  JIT_RESERVED | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v7},
    {"v8",  JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v8},
    {"v9",  JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v9},
    {"v10", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v10},
    {"v11", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v11},
    {"v12", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v12},
    {"v13", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v13},
    {"v14", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v14},
    {"v15", JIT_ALLOCATE | JIT_CALLEE_SAVE | JIT_REG_F64, &v15},
    {"v16", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v16},
    {"v17", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v17},
    {"v18", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v18},
    {"v19", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v19},
    {"v20", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v20},
    {"v21", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v21},
    {"v22", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v22},
    {"v23", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v23},
    {"v24", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v24},
    {"v25", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v25},
    {"v26", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v26},
    {"v27", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v27},
    {"v28", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v28},
    {"v29", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v29},
    {"v30", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v30},
    {"v31", JIT_ALLOCATE | JIT_CALLER_SAVE | JIT_REG_F64 | JIT_REG_V128, &v31},
};

const int a64_num_registers = ARRAY_SIZE(a64_registers);
/* clang-format on */

static const uint8_t *a64_masm_pc(MacroAssembler &e) {
  return e.GetBuffer()->GetStartAddress<uint8_t *>() +
         e.GetSizeOfCodeGenerated();
}

static void a64_emit_b(MacroAssembler &e, const void *target) {
  int64_t imm = ((const uint8_t *)target - a64_masm_pc(e)) >> 2;
  e.b(imm);
}

static void a64_emit_b_cond(MacroAssembler &e, Condition cond,
                            const void *target) {
  int64_t imm = ((const uint8_t *)target - a64_masm_pc(e)) >> 2;
  e.b(imm, cond);
}

static void a64_emit_bl(MacroAssembler &e, const void *target) {
  int64_t imm = ((const uint8_t *)target - a64_masm_pc(e)) >> 2;
  e.bl(imm);
}

Register a64_backend_reg(struct a64_backend *backend,
                         const struct ir_value *v) {
  CHECK(v->reg >= 0 && v->reg < a64_num_registers);
  const Register *reg = (const Register *)a64_registers[v->reg].data;
  CHECK(reg->IsRegister());
  int size = (v->type == VALUE_I64) ? 64 : 32;
  return Register(reg->GetCode(), size);
}

VRegister a64_backend_vreg(struct a64_backend *backend,
                           const struct ir_value *v) {
  CHECK(v->reg >= 0 && v->reg < a64_num_registers);
  const VRegister *reg = (const VRegister *)a64_registers[v->reg].data;
  int size = (v->type == VALUE_F32) ? 32 : (v->type == VALUE_F64 ? 64 : 128);
  return VRegister(reg->GetCode(), size);
}

int a64_backend_save_regs(struct a64_backend *backend, MacroAssembler &e,
                          int mask) {
  /* fixed 160-byte frame header: lr, fp, x19-x26, d8-d15 */
  e.Stp(x29, x30, MemOperand(sp, -A64_STACK_LOCALS, PreIndex));
  e.Stp(x19, x20, MemOperand(sp, 16));
  e.Stp(x21, x22, MemOperand(sp, 32));
  e.Stp(x23, x24, MemOperand(sp, 48));
  e.Stp(x25, x26, MemOperand(sp, 64));
  e.Str(d8, MemOperand(sp, 80));
  e.Str(d9, MemOperand(sp, 88));
  e.Str(d10, MemOperand(sp, 96));
  e.Str(d11, MemOperand(sp, 104));
  e.Str(d12, MemOperand(sp, 112));
  e.Str(d13, MemOperand(sp, 120));
  e.Str(d14, MemOperand(sp, 128));
  e.Str(d15, MemOperand(sp, 136));
  return A64_STACK_LOCALS;
}

void a64_backend_restore_regs(struct a64_backend *backend, MacroAssembler &e,
                              int mask) {
  e.Ldr(d8, MemOperand(sp, 80));
  e.Ldr(d9, MemOperand(sp, 88));
  e.Ldr(d10, MemOperand(sp, 96));
  e.Ldr(d11, MemOperand(sp, 104));
  e.Ldr(d12, MemOperand(sp, 112));
  e.Ldr(d13, MemOperand(sp, 120));
  e.Ldr(d14, MemOperand(sp, 128));
  e.Ldr(d15, MemOperand(sp, 136));
  e.Ldp(x25, x26, MemOperand(sp, 64));
  e.Ldp(x23, x24, MemOperand(sp, 48));
  e.Ldp(x21, x22, MemOperand(sp, 32));
  e.Ldp(x19, x20, MemOperand(sp, 16));
  /* restores sp and lr/fp (post-index) */
  e.Ldp(x29, x30, MemOperand(sp, A64_STACK_LOCALS, PostIndex));
}

void a64_backend_load_mem(struct a64_backend *backend, MacroAssembler &e,
                          const struct ir_value *dst, const MemOperand &src) {
  switch (dst->type) {
    case VALUE_I8:
      e.Ldrb(a64_backend_reg(backend, dst), src);
      break;
    case VALUE_I16:
      e.Ldrh(a64_backend_reg(backend, dst), src);
      break;
    case VALUE_I32:
    case VALUE_I64:
      e.Ldr(a64_backend_reg(backend, dst), src);
      break;
    case VALUE_F32:
      e.Ldr(VRegister(a64_backend_vreg(backend, dst).GetCode(), 32), src);
      break;
    case VALUE_F64:
      e.Ldr(VRegister(a64_backend_vreg(backend, dst).GetCode(), 64), src);
      break;
    case VALUE_V128:
      e.Ldr(VRegister(a64_backend_vreg(backend, dst).GetCode(), 128), src);
      break;
    default:
      LOG_FATAL("unexpected load result type");
      break;
  }
}

void a64_backend_store_mem(struct a64_backend *backend, MacroAssembler &e,
                           const MemOperand &dst, const struct ir_value *src) {
  if (ir_is_constant(src)) {
    /* constants go through x17, except 0 which stores from xzr */
    Register tmp = xzr;
    if (ir_is_int(src->type) && ir_zext_constant(src)) {
      tmp = x17;
      e.Mov(x17, (uint64_t)ir_zext_constant(src));
    }

    switch (src->type) {
      case VALUE_I8:
        e.Strb(tmp.W(), dst);
        break;
      case VALUE_I16:
        e.Strh(tmp.W(), dst);
        break;
      case VALUE_I32:
        e.Str(tmp.W(), dst);
        break;
      case VALUE_I64:
        e.Str(tmp, dst);
        break;
      case VALUE_F32: {
        uint32_t bits;
        memcpy(&bits, &src->f32, 4);
        e.Mov(w17, bits);
        e.Fmov(VRegister(0, 32), w17);
        e.Str(VRegister(0, 32), dst);
      } break;
      case VALUE_F64: {
        uint64_t bits;
        memcpy(&bits, &src->f64, 8);
        e.Mov(x17, bits);
        e.Fmov(VRegister(0, 64), x17);
        e.Str(VRegister(0, 64), dst);
      } break;
      default:
        LOG_FATAL("unexpected store value type");
        break;
    }
    return;
  }

  switch (src->type) {
    case VALUE_I8:
      e.Strb(a64_backend_reg(backend, src), dst);
      break;
    case VALUE_I16:
      e.Strh(a64_backend_reg(backend, src), dst);
      break;
    case VALUE_I32:
    case VALUE_I64:
      e.Str(a64_backend_reg(backend, src), dst);
      break;
    case VALUE_F32:
      e.Str(VRegister(a64_backend_vreg(backend, src).GetCode(), 32), dst);
      break;
    case VALUE_F64:
      e.Str(VRegister(a64_backend_vreg(backend, src).GetCode(), 64), dst);
      break;
    case VALUE_V128:
      e.Str(VRegister(a64_backend_vreg(backend, src).GetCode(), 128), dst);
      break;
    default:
      LOG_FATAL("unexpected store value type");
      break;
  }
}

void a64_backend_mov_value(struct a64_backend *backend, MacroAssembler &e,
                           const Register &dst, const struct ir_value *v) {
  if (ir_is_constant(v)) {
    e.Mov(dst, (uint64_t)ir_zext_constant(v));
    return;
  }

  Register rs = a64_backend_reg(backend, v);
  if (dst.GetSizeInBits() == 64) {
    e.Mov(XRegister(dst.GetCode()), XRegister(rs.GetCode()));
  } else {
    e.Mov(WRegister(dst.GetCode()), WRegister(rs.GetCode()));
  }
}

void a64_backend_block_label(char *name, size_t size, struct ir_block *block) {
  snprintf(name, size, ".%p", block);
}

/* slow path after the unit, so the fast path falls through its branch. writes
   pc to the context if asked, then jumps to thunk (or traps). if it has code,
   runs that and returns to ret */
struct a64_stub {
  Label label;
  int write_pc;
  uint32_t pc;
  const void *thunk;
  std::function<void(MacroAssembler &)> code;
  Label ret;
};

Label *a64_backend_stub(struct a64_backend *backend, int write_pc, uint32_t pc,
                        const void *thunk) {
  std::list<struct a64_stub> *stubs =
      (std::list<struct a64_stub> *)backend->stubs;
  stubs->emplace_back();
  struct a64_stub &stub = stubs->back();
  stub.write_pc = write_pc;
  stub.pc = pc;
  stub.thunk = thunk;
  return &stub.label;
}

Label *a64_backend_slow_path(struct a64_backend *backend, Label **ret,
                             std::function<void(MacroAssembler &)> code) {
  std::list<struct a64_stub> *stubs =
      (std::list<struct a64_stub> *)backend->stubs;
  Label *label = a64_backend_stub(backend, 0, 0, NULL);
  stubs->back().code = code;
  *ret = &stubs->back().ret;
  return label;
}

static void a64_backend_emit_stubs(struct a64_backend *backend,
                                   MacroAssembler &e) {
  struct jit_guest *guest = backend->base.guest;
  std::list<struct a64_stub> *stubs =
      (std::list<struct a64_stub> *)backend->stubs;

  for (struct a64_stub &stub : *stubs) {
    e.Bind(&stub.label);
    if (stub.code) {
      stub.code(e);
      e.B(&stub.ret);
      continue;
    }
    if (stub.write_pc) {
      e.Mov(w16, stub.pc);
      e.Str(w16, MemOperand(a64_guestctx, guest->offset_pc));
    }
    if (stub.thunk) {
      a64_emit_b(e, stub.thunk);
    } else {
      e.Brk(0);
    }
  }
}

/* only calls raise irqs, so instead of checking on entry every block, one
   that makes calls checks on the way out and writes the pc of the block it was
   going to if it yields. pc is a direct branch target, otherwise only written
   on slow paths */
static void a64_backend_check_interrupts(struct a64_backend *backend,
                                         MacroAssembler &e, int write_pc,
                                         uint32_t pc) {
  struct jit_guest *guest = backend->base.guest;

  if (!backend->check_interrupts) {
    return;
  }

  Label *stub =
      a64_backend_stub(backend, write_pc, pc, backend->dispatch_interrupt);
  e.Ldr(x16, MemOperand(a64_guestctx, guest->offset_interrupts));
  e.Cbnz(x16, stub);
}

void a64_backend_emit_branch(struct a64_backend *backend, MacroAssembler &e,
                             struct ir *ir, const struct ir_value *target) {
  struct jit_guest *guest = backend->base.guest;
  std::map<struct ir_block *, Label *> *labels =
      (std::map<struct ir_block *, Label *> *)backend->labels;

  int dispatch_type = 2;
  Label *block_label = NULL;
  Register pc = w17;

  if (target) {
    if (ir_is_constant(target)) {
      if (target->type == VALUE_BLOCK) {
        struct ir_value *addr = ir_get_meta(ir, target->blk, IR_META_ADDR);
        e.Mov(w16, (uint32_t)addr->i32);
        e.Str(w16, MemOperand(a64_guestctx, guest->offset_pc));
        block_label = (*labels)[target->blk];
        dispatch_type = 0;
      } else {
        dispatch_type = 1;
      }
    } else {
      Register ra = a64_backend_reg(backend, target);
      pc = Register(ra.GetCode(), 32);
      e.Str(pc, MemOperand(a64_guestctx, guest->offset_pc));
      dispatch_type = 2;
    }
  }

  a64_backend_check_interrupts(backend, e, dispatch_type == 1,
                               dispatch_type == 1 ? target->i32 : 0);

  switch (dispatch_type) {
    case 0:
      e.B(block_label);
      break;
    case 1: {
      /* pass the target in the word after the call instead of writing it to
         the context. once linked the call is a direct branch that never reads
         it, so pc is only written on slow paths */
      ExactAssemblyScope scope(&e, 8);
      a64_emit_bl(e, backend->dispatch_static);
      e.dc32((uint32_t)target->i32);
    } break;
    case 2: {
      /* same lookup as the dynamic thunk, inline, so each indirect branch
         gets its own predictor entry rather than sharing the thunk's */
      if (!target) {
        e.Ldr(pc, MemOperand(a64_guestctx, guest->offset_pc));
      }
      int width = 32 - backend->cache_shift - clz32(backend->cache_mask);
      e.Ldr(x16, (uint64_t)backend->cache);
      e.Ubfx(w17, pc, backend->cache_shift, width);
      e.Ldr(x16, MemOperand(x16, x17, LSL, 3));
      e.Br(x16);
    } break;
  }
}

static void a64_backend_emit_epilog(struct a64_backend *backend,
                                    MacroAssembler &e, struct ir *ir,
                                    struct ir_block *block) {
  struct ir_instr *last_instr =
      list_last_entry(&block->instrs, struct ir_instr, it);

  if (last_instr->op != OP_BRANCH && last_instr->op != OP_BRANCH_COND) {
    a64_backend_emit_branch(backend, e, ir, NULL);
  }
}

static void a64_backend_emit_prolog(struct a64_backend *backend,
                                    MacroAssembler &e, struct ir *ir,
                                    struct ir_block *block) {
  struct jit_guest *guest = backend->base.guest;

  int num_instrs = 0;
  int num_cycles = 0;
  uint32_t block_addr = 0;

  backend->check_interrupts = 0;

  list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
    if (instr->op == OP_SOURCE_INFO) {
      if (!num_instrs) {
        block_addr = instr->arg[0]->i32;
      }
      num_instrs += 1;
      num_cycles += instr->arg[1]->i32;
    }
    if (ir_opdefs[instr->op].flags & IR_FLAG_CALL) {
      backend->check_interrupts = 1;
    }
  }

  /* yield once the cycles are used. the budget lives in a64_cycles across
     blocks and is written back on exit. direct branches only write pc on slow
     paths, so the context has the block address when needed. irqs are checked
     on the way out of blocks that can raise them */
  e.Tbnz(w26, 31,
         a64_backend_stub(backend, 1, block_addr, backend->dispatch_exit));
  e.Sub(w26, w26, num_cycles);

  /* instruction counting is for profiling only, and costs a load/store per
     block */
  static int count_instrs = -1;
  if (count_instrs < 0) {
    count_instrs = getenv("NODOSUMI_COUNT_INSTRS") != NULL;
  }
  if (count_instrs) {
    e.Ldr(w16, MemOperand(a64_guestctx, guest->offset_instrs));
    e.Add(w16, w16, num_instrs);
    e.Str(w16, MemOperand(a64_guestctx, guest->offset_instrs));
  }
}

static int a64_backend_emit(struct a64_backend *backend, struct ir *ir,
                            jit_emit_cb emit_cb, void *emit_data) {
  std::map<struct ir_block *, Label *> labels;
  list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
    labels[block] = new Label();
  }
  backend->labels = &labels;

  std::list<struct a64_stub> stubs;
  backend->stubs = &stubs;

  CHECK_LT(ir->locals_size, A64_STACK_SIZE);

  int ok = 1;
  try {
    MacroAssembler masm(backend->code + backend->code_offset,
                        backend->code_size - backend->code_offset);

    list_for_each_entry(block, &ir->blocks, struct ir_block, it) {
      int first = 1;
      uint8_t *block_addr = (uint8_t *)a64_masm_pc(masm);

      masm.Bind(labels[block]);

      a64_backend_emit_prolog(backend, masm, ir, block);

      list_for_each_entry(instr, &block->instrs, struct ir_instr, it) {
        if (emit_cb && instr->op == OP_SOURCE_INFO) {
          uint32_t guest_addr = instr->arg[0]->i32;

          if (first) {
            emit_cb(emit_data, JIT_EMIT_BLOCK, guest_addr, block_addr);
            first = 0;
          }

          uint8_t *instr_addr = (uint8_t *)a64_masm_pc(masm);
          emit_cb(emit_data, JIT_EMIT_INSTR, guest_addr, instr_addr);
        }

        struct jit_emitter *emitter = &a64_emitters[instr->op];
        a64_emit_cb emit = (a64_emit_cb)emitter->func;
        CHECK_NOTNULL(emit);
        emit(backend, masm, ir, instr);
      }

      a64_backend_emit_epilog(backend, masm, ir, block);
    }

    a64_backend_emit_stubs(backend, masm);

    masm.FinalizeCode();
    backend->code_offset += (int)masm.GetSizeOfCodeGenerated();
  } catch (...) {
    /* out of code buffer, have the jit flush */
    ok = 0;
  }

  backend->labels = NULL;
  backend->stubs = NULL;
  for (auto &it : labels) {
    delete it.second;
  }

  return ok;
}

static int a64_backend_assemble_code(struct jit_backend *base, struct ir *ir,
                                     uint8_t **addr, int *size,
                                     jit_emit_cb emit_cb, void *emit_data) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);

  int start = backend->code_offset;
  uint8_t *code = backend->code + start;


  if (!a64_backend_emit(backend, ir, emit_cb, emit_data)) {
    backend->code_offset = start;
    return 0;
  }


  *addr = code;
  *size = backend->code_offset - start;

  __builtin___clear_cache((char *)code, (char *)code + *size);

  return 1;
}

static void a64_backend_reset(struct jit_backend *base) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  backend->code_offset = A64_THUNK_SIZE;
}

static void a64_backend_dump_code(struct jit_backend *base, const uint8_t *addr,
                                  int size, FILE *output) {
  fprintf(output, "#==--------------------------------------------------==#\n");
  fprintf(output, "# a64 (no disassembler, raw words)\n");
  fprintf(output, "#==--------------------------------------------------==#\n");
  for (int i = 0; i + 4 <= size; i += 4) {
    fprintf(output, "%08x\n", *(const uint32_t *)(addr + i));
  }
}

/* fastmem slow path.
 *
 * a faulting integer load/store with base x27 is treated as an mmio access.
 * the signal handler redirects pc into one of these thunks, with x0/x1/x2 set
 * up as arguments and x16 holding the callback. the thunk preserves the
 * caller-saved registers the jit may have live values in, calls the callback,
 * and returns to pc+4 (x30). */
void a64_backend_emit_thunks(struct a64_backend *backend, MacroAssembler &e) {
  uint8_t *base = e.GetBuffer()->GetStartAddress<uint8_t *>();

  /* x31 is sp, so 31 load thunks */
  for (int rt = 0; rt < 31; rt++) {
    backend->load_thunk[rt] = (void *)(base + e.GetSizeOfCodeGenerated());

    e.Sub(sp, sp, 336);
    e.Stp(x9, x10, MemOperand(sp, 0));
    e.Stp(x11, x12, MemOperand(sp, 16));
    e.Stp(x13, x14, MemOperand(sp, 32));
    e.Str(x15, MemOperand(sp, 48));
    e.Str(x16, MemOperand(sp, 56));
    e.Str(x30, MemOperand(sp, 64));
    for (int i = 0; i < 16; i++) {
      e.Str(VRegister(16 + i, 128), MemOperand(sp, 80 + i * 16));
    }
    e.Blr(x16);
    for (int i = 0; i < 16; i++) {
      e.Ldr(VRegister(16 + i, 128), MemOperand(sp, 80 + i * 16));
    }
    e.Ldr(x30, MemOperand(sp, 64));
    e.Ldr(x16, MemOperand(sp, 56));
    e.Ldr(x15, MemOperand(sp, 48));
    e.Ldp(x13, x14, MemOperand(sp, 32));
    e.Ldp(x11, x12, MemOperand(sp, 16));
    e.Ldp(x9, x10, MemOperand(sp, 0));
    /* move the result after restoring caller-saved regs: rt may be one of
       x9-x15, which would otherwise clobber the loaded value */
    e.Mov(XRegister(rt), x0);
    e.Add(sp, sp, 336);
    e.Ret();
  }

  backend->store_thunk = (void *)(base + e.GetSizeOfCodeGenerated());
  e.Sub(sp, sp, 336);
  e.Stp(x9, x10, MemOperand(sp, 0));
  e.Stp(x11, x12, MemOperand(sp, 16));
  e.Stp(x13, x14, MemOperand(sp, 32));
  e.Str(x15, MemOperand(sp, 48));
  e.Str(x16, MemOperand(sp, 56));
  e.Str(x30, MemOperand(sp, 64));
  for (int i = 0; i < 16; i++) {
    e.Str(VRegister(16 + i, 128), MemOperand(sp, 80 + i * 16));
  }
  e.Blr(x16);
  for (int i = 0; i < 16; i++) {
    e.Ldr(VRegister(16 + i, 128), MemOperand(sp, 80 + i * 16));
  }
  e.Ldr(x30, MemOperand(sp, 64));
  e.Ldr(x16, MemOperand(sp, 56));
  e.Ldr(x15, MemOperand(sp, 48));
  e.Ldp(x13, x14, MemOperand(sp, 32));
  e.Ldp(x11, x12, MemOperand(sp, 16));
  e.Ldp(x9, x10, MemOperand(sp, 0));
  e.Add(sp, sp, 336);
  e.Ret();
}

static int a64_backend_handle_exception(struct jit_backend *base,
                                        struct exception_state *ex) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  struct jit_guest *guest = backend->base.guest;

  uint32_t insn = *(uint32_t *)ex->pc;

  /* integer load/store (bits 29:24 == 111000) */
  if (((insn >> 24) & 0x3f) != 0x38) {
    return 0;
  }

  int size = (insn >> 30) & 3;
  int is_vector = (insn >> 26) & 1;
  int opc = (insn >> 22) & 3;
  int rn = (insn >> 5) & 0x1f;
  int rt = insn & 0x1f;

  /* only plain load/store through the guest memory base (x27) */
  if (is_vector || rn != 27 || (opc != 0 && opc != 1)) {
    return 0;
  }

  int size_bytes = 1 << size;
  uint64_t guest_addr = ex->fault_addr - ex->thread_state.r27;

  /* if the address maps to real backing memory the fault was not an mmio
     access, let it propagate */
  uint8_t *ptr;
  guest->lookup(guest->mem, (uint32_t)guest_addr, NULL, &ptr, NULL, NULL);
  if (ptr) {
    return 0;
  }

  ex->thread_state.r0 = (uint64_t)guest->mem;
  ex->thread_state.r1 = (uint32_t)guest_addr;
  ex->thread_state.r30 = ex->pc + 4;

  if (opc == 1) {
    /* load: callback returns the value in w0/x0, the thunk moves it to rt */
    switch (size_bytes) {
      case 1:
        ex->thread_state.r16 = (uint64_t)guest->r8;
        break;
      case 2:
        ex->thread_state.r16 = (uint64_t)guest->r16;
        break;
      case 4:
        ex->thread_state.r16 = (uint64_t)guest->r32;
        break;
      case 8:
        ex->thread_state.r16 = (uint64_t)guest->r64;
        break;
    }
    ex->thread_state.pc = (uint64_t)backend->load_thunk[rt];
  } else {
    /* store: pass the source register value in x2. register 31 is the zero
       register constant zeros are stored from */
    ex->thread_state.r2 = rt == 31 ? 0 : ex->thread_state.r[rt];
    switch (size_bytes) {
      case 1:
        ex->thread_state.r16 = (uint64_t)guest->w8;
        break;
      case 2:
        ex->thread_state.r16 = (uint64_t)guest->w16;
        break;
      case 4:
        ex->thread_state.r16 = (uint64_t)guest->w32;
        break;
      case 8:
        ex->thread_state.r16 = (uint64_t)guest->w64;
        break;
    }
    ex->thread_state.pc = (uint64_t)backend->store_thunk;
  }

  return 1;
}

static void a64_backend_destroy(struct jit_backend *base) {
  struct a64_backend *backend = container_of(base, struct a64_backend, base);
  a64_dispatch_shutdown(backend);
  free(backend);
}

struct jit_backend *a64_backend_create(struct jit_guest *guest, void *code,
                                       int code_size) {
  struct a64_backend *backend =
      (struct a64_backend *)calloc(1, sizeof(struct a64_backend));

  backend->base.guest = guest;
  backend->base.destroy = &a64_backend_destroy;

  backend->base.registers = a64_registers;
  backend->base.num_registers = ARRAY_SIZE(a64_registers);
  backend->base.emitters = a64_emitters;
  backend->base.num_emitters = ARRAY_SIZE(a64_emitters);

  backend->base.reset = &a64_backend_reset;
  backend->base.assemble_code = &a64_backend_assemble_code;
  backend->base.dump_code = &a64_backend_dump_code;
  backend->base.handle_exception = &a64_backend_handle_exception;

  backend->base.run_code = &a64_dispatch_run_code;
  backend->base.lookup_code = &a64_dispatch_lookup_code;
  backend->base.cache_code = &a64_dispatch_cache_code;
  backend->base.invalidate_code = &a64_dispatch_invalidate_code;
  backend->base.patch_edge = &a64_dispatch_patch_edge;
  backend->base.restore_edge = &a64_dispatch_restore_edge;

  backend->code = (uint8_t *)code;
  backend->code_size = code_size;
  backend->code_offset = A64_THUNK_SIZE;

  int r = protect_pages(code, code_size, ACC_READWRITEEXEC);
  CHECK(r);

  a64_dispatch_init(backend);
  a64_dispatch_emit_thunks(backend);

  return &backend->base;
}
