#include "jit/backend/a64/a64_local.h"

extern "C" {
#include "jit/ir/ir.h"
#include "jit/jit.h"
#include "jit/jit_guest.h"
}

#define EMITTER(op, constraints)                                           \
  void a64_emit_##op(struct a64_backend *, MacroAssembler &, struct ir *,  \
                     struct ir_instr *);                                   \
  static struct _a64_##op##_init {                                         \
    _a64_##op##_init() {                                                   \
      a64_emitters[OP_##op] = {(void *)&a64_emit_##op, constraints};       \
    }                                                                      \
  } a64_##op##_init;                                                       \
  void a64_emit_##op(struct a64_backend *backend, MacroAssembler &e,       \
                     struct ir *ir, struct ir_instr *instr)

#define CONSTRAINTS(result_flags, ...) \
  result_flags, {                      \
    __VA_ARGS__                        \
  }

#define RES instr->result
#define ARG0 instr->arg[0]
#define ARG1 instr->arg[1]
#define ARG2 instr->arg[2]
#define ARG3 instr->arg[3]

#define RES_REG a64_backend_reg(backend, RES)
#define ARG0_REG a64_backend_reg(backend, ARG0)
#define ARG1_REG a64_backend_reg(backend, ARG1)
#define ARG2_REG a64_backend_reg(backend, ARG2)
#define ARG3_REG a64_backend_reg(backend, ARG3)

#define RES_VREG a64_backend_vreg(backend, RES)
#define ARG0_VREG a64_backend_vreg(backend, ARG0)
#define ARG1_VREG a64_backend_vreg(backend, ARG1)
#define ARG2_VREG a64_backend_vreg(backend, ARG2)

enum {
  NONE = 0,
  REG_I64 = JIT_REG_I64,
  REG_F64 = JIT_REG_F64,
  REG_V128 = JIT_REG_V128,
  REG_ALL = REG_I64 | REG_F64 | REG_V128,
  IMM_I32 = JIT_IMM_I32,
  IMM_I64 = JIT_IMM_I64,
  IMM_F32 = JIT_IMM_F32,
  IMM_F64 = JIT_IMM_F64,
  IMM_BLK = JIT_IMM_BLK,
  IMM_ALL = IMM_I32 | IMM_I64 | IMM_F32 | IMM_F64 | IMM_BLK,
  VAL_I64 = REG_I64 | IMM_I64,
  VAL_ALL = REG_ALL | IMM_ALL,
  OPT = JIT_OPTIONAL,
  OPT_I64 = OPT | VAL_I64,
};

struct jit_emitter a64_emitters[IR_NUM_OPS];

static Condition a64_cmp_cond(enum ir_cmp cmp) {
  switch (cmp) {
    case CMP_EQ:
      return eq;
    case CMP_NE:
      return ne;
    case CMP_SGE:
      return ge;
    case CMP_SGT:
      return gt;
    case CMP_UGE:
      return hs;
    case CMP_UGT:
      return hi;
    case CMP_SLE:
      return le;
    case CMP_SLT:
      return lt;
    case CMP_ULE:
      return ls;
    case CMP_ULT:
      return lo;
    default:
      LOG_FATAL("unexpected comparison type");
      return eq;
  }
}

EMITTER(SOURCE_INFO, CONSTRAINTS(NONE, IMM_I32, IMM_I32)) {}

EMITTER(FALLBACK, CONSTRAINTS(NONE, IMM_I64, IMM_I32, IMM_I32)) {
  struct jit_guest *guest = backend->base.guest;
  e.Ldr(x0, (uint64_t)guest);
  e.Mov(w1, (uint32_t)ARG1->i32);
  e.Mov(w2, (uint32_t)ARG2->i32);
  e.Ldr(x16, (uint64_t)ARG0->i64);
  e.Blr(x16);
}

EMITTER(LOAD_HOST, CONSTRAINTS(REG_ALL, REG_I64)) {
  a64_backend_load_mem(backend, e, RES, MemOperand(ARG0_REG, 0));
}

EMITTER(STORE_HOST, CONSTRAINTS(NONE, REG_I64, VAL_ALL)) {
  a64_backend_store_mem(backend, e, MemOperand(ARG0_REG, 0), ARG1);
}

EMITTER(LOAD_GUEST, CONSTRAINTS(REG_ALL, REG_I64 | IMM_I32)) {
  struct jit_guest *guest = backend->base.guest;

  if (ir_is_constant(ARG0)) {
    void *userdata;
    uint8_t *ptr;
    mem_read_cb read;
    guest->lookup(guest->mem, ARG0->i32, &userdata, &ptr, &read, NULL);

    if (ptr) {
      e.Ldr(x16, (uint64_t)ptr);
      a64_backend_load_mem(backend, e, RES, MemOperand(x16, 0));
    } else {
      int data_size = ir_type_size(RES->type);
      uint32_t data_mask = (uint32_t)((UINT64_C(1) << (data_size * 8)) - 1);

      e.Ldr(x0, (uint64_t)userdata);
      e.Mov(w1, (uint32_t)ARG0->i32);
      e.Mov(w2, data_mask);
      e.Ldr(x16, (uint64_t)read);
      e.Blr(x16);
      e.Mov(RES_REG, w0);
    }
  } else {
    void *fn = NULL;
    switch (RES->type) {
      case VALUE_I8:
        fn = (void *)guest->r8;
        break;
      case VALUE_I16:
        fn = (void *)guest->r16;
        break;
      case VALUE_I32:
        fn = (void *)guest->r32;
        break;
      case VALUE_I64:
        fn = (void *)guest->r64;
        break;
      default:
        LOG_FATAL("unexpected load result type");
        break;
    }

    e.Ldr(x0, (uint64_t)guest->mem);
    e.Mov(x1, ARG0_REG);
    e.Ldr(x16, (uint64_t)fn);
    e.Blr(x16);
    e.Mov(RES_REG, Register(0, RES_REG.GetSizeInBits()));
  }
}

EMITTER(STORE_GUEST, CONSTRAINTS(NONE, REG_I64 | IMM_I32, VAL_ALL)) {
  struct jit_guest *guest = backend->base.guest;

  if (ir_is_constant(ARG0)) {
    void *userdata;
    uint8_t *ptr;
    mem_write_cb write;
    guest->lookup(guest->mem, ARG0->i32, &userdata, &ptr, NULL, &write);

    if (ptr) {
      e.Ldr(x16, (uint64_t)ptr);
      a64_backend_store_mem(backend, e, MemOperand(x16, 0), ARG1);
    } else {
      int data_size = ir_type_size(ARG1->type);
      uint32_t data_mask = (uint32_t)((UINT64_C(1) << (data_size * 8)) - 1);

      e.Ldr(x0, (uint64_t)userdata);
      e.Mov(w1, (uint32_t)ARG0->i32);
      a64_backend_mov_value(backend, e, Register(2, 64), ARG1);
      e.Mov(w3, data_mask);
      e.Ldr(x16, (uint64_t)write);
      e.Blr(x16);
    }
  } else {
    void *fn = NULL;
    switch (ARG1->type) {
      case VALUE_I8:
        fn = (void *)guest->w8;
        break;
      case VALUE_I16:
        fn = (void *)guest->w16;
        break;
      case VALUE_I32:
        fn = (void *)guest->w32;
        break;
      case VALUE_I64:
        fn = (void *)guest->w64;
        break;
      default:
        LOG_FATAL("unexpected store value type");
        break;
    }

    Register addr = ARG0_REG;
    const struct ir_value *value = ARG1;
    auto call = [=](MacroAssembler &e) {
      e.Ldr(x0, (uint64_t)guest->mem);
      e.Mov(x1, addr);
      a64_backend_mov_value(backend, e, Register(2, 64), value);
      e.Ldr(x16, (uint64_t)fn);
      e.Blr(x16);
    };

    if (guest->store_window &&
        (ARG1->type == VALUE_I32 || ARG1->type == VALUE_I64)) {
      /* stores into the window are made directly, relative to the context
         when it's in reach, and the call is kept out of line so they fall
         straight through */
      Label *ret;
      Label *slow = a64_backend_slow_path(backend, &ret, call);
      intptr_t offset = guest->store_window - (uint8_t *)guest->ctx;
      uint32_t size = guest->store_window_mask + 4;
      e.Lsr(w16, addr.W(), 26);
      e.Cmp(w16, guest->store_window_tag);
      e.B(slow, ne);
      if (offset == -(intptr_t)size && !(size & (size - 1))) {
        /* a window that ends where the context starts is indexed back from
           it, addr | -size being the word's distance from the window's end
           for an aligned store, as the guest's always are */
        e.Orr(w16, addr.W(), ~(size - 1));
        a64_backend_store_mem(backend, e, MemOperand(a64_guestctx, w16, SXTW),
                              ARG1);
      } else if (offset >= 0 && offset < (1 << 24) && !(offset & 3)) {
        /* the offset's upper bits are added, and its low 12 bits encoded in
           the store */
        e.And(w16, addr.W(), guest->store_window_mask);
        e.Add(x16, a64_guestctx, x16);
        if (offset & ~0xfff) {
          e.Add(x16, x16, offset & ~0xfff);
        }
        a64_backend_store_mem(backend, e, MemOperand(x16, offset & 0xfff),
                              ARG1);
      } else {
        e.And(w16, addr.W(), guest->store_window_mask);
        e.Mov(x17, (uint64_t)guest->store_window);
        e.Add(x16, x17, x16);
        a64_backend_store_mem(backend, e, MemOperand(x16, 0), ARG1);
      }
      e.Bind(ret);
    } else {
      call(e);
    }
  }
}

EMITTER(LOAD_FAST, CONSTRAINTS(REG_ALL, REG_I64)) {
  a64_backend_load_mem(backend, e, RES, MemOperand(a64_guestmem, ARG0_REG));
}

EMITTER(STORE_FAST, CONSTRAINTS(NONE, REG_I64, VAL_ALL)) {
  a64_backend_store_mem(backend, e, MemOperand(a64_guestmem, ARG0_REG), ARG1);
}

/* a guest pref of ram becomes a host one. the store queue addresses that also
   come through here are skipped: nothing is mapped there, and a prefetch of
   them stalls on a page table walk every flush */
EMITTER(PREFETCH, CONSTRAINTS(NONE, REG_I64)) {
  Label skip;
  e.Lsr(w16, ARG0_REG.W(), 26);
  e.Cmp(w16, 0x38);
  e.B(&skip, eq);
  e.Prfm(PLDL1KEEP, MemOperand(a64_guestmem, ARG0_REG));
  e.Bind(&skip);
}

EMITTER(LOAD_CONTEXT, CONSTRAINTS(REG_ALL, IMM_I32)) {
  a64_backend_load_mem(backend, e, RES, MemOperand(a64_guestctx, ARG0->i32));
}

EMITTER(STORE_CONTEXT, CONSTRAINTS(NONE, IMM_I32, VAL_ALL)) {
  a64_backend_store_mem(backend, e, MemOperand(a64_guestctx, ARG0->i32), ARG1);
}

EMITTER(LOAD_LOCAL, CONSTRAINTS(REG_ALL, IMM_I32)) {
  a64_backend_load_mem(backend, e, RES, MemOperand(sp, ARG0->i32));
}

EMITTER(STORE_LOCAL, CONSTRAINTS(NONE, IMM_I32, VAL_ALL)) {
  a64_backend_store_mem(backend, e, MemOperand(sp, ARG0->i32), ARG1);
}

EMITTER(FTOI, CONSTRAINTS(REG_I64, REG_F64)) {
  switch (RES->type) {
    case VALUE_I32:
      if (ARG0->type == VALUE_F32) {
        e.Fcvtzs(WRegister(RES_REG.GetCode()), VRegister(ARG0_VREG.GetCode(), 32));
      } else {
        e.Fcvtzs(WRegister(RES_REG.GetCode()), VRegister(ARG0_VREG.GetCode(), 64));
      }
      break;
    default:
      LOG_FATAL("unexpected result type");
      break;
  }
}

EMITTER(ITOF, CONSTRAINTS(REG_F64, REG_I64)) {
  switch (RES->type) {
    case VALUE_F32:
      e.Scvtf(VRegister(RES_VREG.GetCode(), 32), WRegister(ARG0_REG.GetCode()));
      break;
    case VALUE_F64:
      e.Scvtf(VRegister(RES_VREG.GetCode(), 64), XRegister(ARG0_REG.GetCode()));
      break;
    default:
      LOG_FATAL("unexpected result type");
      break;
  }
}

EMITTER(TRUNC, CONSTRAINTS(REG_I64, REG_I64)) {
  Register rd = RES_REG;
  Register ra = ARG0_REG;

  switch (RES->type) {
    case VALUE_I8:
      e.Uxtb(rd, ra);
      break;
    case VALUE_I16:
      e.Uxth(rd, ra);
      break;
    case VALUE_I32:
      e.Mov(rd, ra);
      break;
    default:
      LOG_FATAL("unexpected value type");
      break;
  }
}

EMITTER(SEXT, CONSTRAINTS(REG_I64, REG_I64)) {
  Register rd = RES_REG;
  Register ra = ARG0_REG;
  int src_bits = ARG0->type == VALUE_I8    ? 8
                 : ARG0->type == VALUE_I16 ? 16
                                           : 32;

  if (RES->type == VALUE_I64) {
    switch (src_bits) {
      case 8:
        e.Sxtb(rd, ra);
        break;
      case 16:
        e.Sxth(rd, ra);
        break;
      default:
        e.Sxtw(rd, ra);
        break;
    }
  } else {
    switch (src_bits) {
      case 8:
        e.Sxtb(rd, ra);
        break;
      case 16:
        e.Sxth(rd, ra);
        break;
      default:
        e.Mov(rd, ra);
        break;
    }
  }
}

EMITTER(ZEXT, CONSTRAINTS(REG_I64, REG_I64)) {
  Register rd = RES_REG;
  Register ra = ARG0_REG;
  int src_bits = ARG0->type == VALUE_I8    ? 8
                 : ARG0->type == VALUE_I16 ? 16
                                           : 32;

  if (ARG0->def && (ARG0->def->op == OP_CMP || ARG0->def->op == OP_FCMP)) {
    /* a comparison's cset already wrote a zero-extended 0 or 1 */
    if (rd.GetCode() != ra.GetCode()) {
      e.Mov(WRegister(rd.GetCode()), WRegister(ra.GetCode()));
    }
  } else if (RES->type == VALUE_I64 && src_bits == 32) {
    /* writing a W register zeroes the upper 32 bits */
    e.Mov(WRegister(rd.GetCode()), WRegister(ra.GetCode()));
  } else {
    switch (src_bits) {
      case 8:
        e.Uxtb(rd, ra);
        break;
      case 16:
        e.Uxth(rd, ra);
        break;
      default:
        e.Mov(rd, ra);
        break;
    }
  }
}

EMITTER(FEXT, CONSTRAINTS(REG_F64, REG_F64)) {
  e.Fcvt(VRegister(RES_VREG.GetCode(), 64), VRegister(ARG0_VREG.GetCode(), 32));
}

EMITTER(FTRUNC, CONSTRAINTS(REG_F64, REG_F64)) {
  e.Fcvt(VRegister(RES_VREG.GetCode(), 32), VRegister(ARG0_VREG.GetCode(), 64));
}

EMITTER(SELECT, CONSTRAINTS(REG_I64, REG_I64, REG_I64, REG_I64)) {
  e.Cmp(ARG2_REG, 0);
  e.Csel(RES_REG, ARG0_REG, ARG1_REG, ne);
}

EMITTER(CMP, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32, IMM_I32)) {
  Register ra = ARG0_REG;

  if (ir_is_constant(ARG1)) {
    e.Cmp(ra, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Cmp(ra, ARG1_REG);
  }

  e.Cset(RES_REG, a64_cmp_cond((enum ir_cmp)ARG2->i32));
}

EMITTER(FCMP, CONSTRAINTS(REG_I64, REG_F64, REG_F64, IMM_I32)) {
  if (ARG0->type == VALUE_F32) {
    e.Fcmp(VRegister(ARG0_VREG.GetCode(), 32), VRegister(ARG1_VREG.GetCode(), 32));
  } else {
    e.Fcmp(VRegister(ARG0_VREG.GetCode(), 64), VRegister(ARG1_VREG.GetCode(), 64));
  }

  enum ir_cmp cmp = (enum ir_cmp)ARG2->i32;
  switch (cmp) {
    case CMP_EQ:
    case CMP_NE:
    case CMP_SGE:
    case CMP_SGT:
    case CMP_SLE:
    case CMP_SLT:
      e.Cset(RES_REG, a64_cmp_cond(cmp));
      break;
    default:
      LOG_FATAL("unexpected comparison type");
      break;
  }
}

EMITTER(ADD, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Add(RES_REG, ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Add(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(SUB, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Sub(RES_REG, ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Sub(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(SMUL, CONSTRAINTS(REG_I64, REG_I64, REG_I64)) {
  e.Mul(RES_REG, ARG0_REG, ARG1_REG);
}

EMITTER(UMUL, CONSTRAINTS(REG_I64, REG_I64, REG_I64)) {
  e.Mul(RES_REG, ARG0_REG, ARG1_REG);
}

EMITTER(DIV, CONSTRAINTS(NONE)) { LOG_FATAL("unsupported"); }

EMITTER(NEG, CONSTRAINTS(REG_I64, REG_I64)) { e.Neg(RES_REG, ARG0_REG); }

EMITTER(ABS, CONSTRAINTS(REG_I64, REG_I64)) {
  e.Cmp(ARG0_REG, 0);
  e.Cneg(RES_REG, ARG0_REG, lt);
}

EMITTER(FADD, CONSTRAINTS(REG_F64, REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fadd(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits),
         VRegister(ARG1_VREG.GetCode(), bits));
}

EMITTER(FSUB, CONSTRAINTS(REG_F64, REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fsub(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits),
         VRegister(ARG1_VREG.GetCode(), bits));
}

EMITTER(FMUL, CONSTRAINTS(REG_F64, REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fmul(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits),
         VRegister(ARG1_VREG.GetCode(), bits));
}

EMITTER(FDIV, CONSTRAINTS(REG_F64, REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fdiv(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits),
         VRegister(ARG1_VREG.GetCode(), bits));
}

EMITTER(FNEG, CONSTRAINTS(REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fneg(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits));
}

EMITTER(FABS, CONSTRAINTS(REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fabs(VRegister(RES_VREG.GetCode(), bits),
         VRegister(ARG0_VREG.GetCode(), bits));
}

EMITTER(SQRT, CONSTRAINTS(REG_F64, REG_F64)) {
  int bits = RES->type == VALUE_F32 ? 32 : 64;
  e.Fsqrt(VRegister(RES_VREG.GetCode(), bits),
          VRegister(ARG0_VREG.GetCode(), bits));
}

EMITTER(VBROADCAST, CONSTRAINTS(REG_V128, REG_F64)) {
  e.Dup(VRegister(RES_VREG.GetCode(), 128, 4),
        VRegister(ARG0_VREG.GetCode(), 32), 0);
}

EMITTER(VADD, CONSTRAINTS(REG_V128, REG_V128, REG_V128)) {
  e.Fadd(VRegister(RES_VREG.GetCode(), 128, 4),
         VRegister(ARG0_VREG.GetCode(), 128, 4),
         VRegister(ARG1_VREG.GetCode(), 128, 4));
}

EMITTER(VMUL, CONSTRAINTS(REG_V128, REG_V128, REG_V128)) {
  e.Fmul(VRegister(RES_VREG.GetCode(), 128, 4),
         VRegister(ARG0_VREG.GetCode(), 128, 4),
         VRegister(ARG1_VREG.GetCode(), 128, 4));
}

EMITTER(VMUL_B, CONSTRAINTS(REG_V128, REG_V128, REG_F64)) {
  e.fmul(VRegister(RES_VREG.GetCode(), 128, 4),
         VRegister(ARG0_VREG.GetCode(), 128, 4),
         VRegister(ARG1_VREG.GetCode(), 32), 0);
}

/* ir_vdot makes an F32 (scalar dot product), not a V128, so the
   result must be allowed in an F64 register (v8-v15 are F64-only) */
EMITTER(VDOT, CONSTRAINTS(REG_F64, REG_V128, REG_V128)) {
  VRegister rd = VRegister(RES_VREG.GetCode(), 128, 4);
  VRegister ra = VRegister(ARG0_VREG.GetCode(), 128, 4);
  VRegister rb = VRegister(ARG1_VREG.GetCode(), 128, 4);

  e.Fmul(rd, ra, rb);
  /* faddp.4s gives [p0+p1, p2+p3, p0+p1, p2+p3], faddp.2s sums the two
     partials, leaving the full dot product in lane 0 */
  e.Faddp(rd, rd, rd);
  VRegister rd2 = VRegister(RES_VREG.GetCode(), 64, 2);
  e.Faddp(rd2, rd2, rd2);
}

EMITTER(AND, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.And(RES_REG, ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.And(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(OR, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Orr(RES_REG, ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Orr(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(XOR, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Eor(RES_REG, ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Eor(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(NOT, CONSTRAINTS(REG_I64, REG_I64)) { e.Mvn(RES_REG, ARG0_REG); }

EMITTER(SHL, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Lsl(RES_REG, ARG0_REG,
          (unsigned)ir_zext_constant(ARG1) & (RES->type == VALUE_I64 ? 63 : 31));
  } else {
    e.Lsl(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(ASHR, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Asr(RES_REG, ARG0_REG,
          (unsigned)ir_zext_constant(ARG1) & (RES->type == VALUE_I64 ? 63 : 31));
  } else {
    e.Asr(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(LSHR, CONSTRAINTS(REG_I64, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Lsr(RES_REG, ARG0_REG,
          (unsigned)ir_zext_constant(ARG1) & (RES->type == VALUE_I64 ? 63 : 31));
  } else {
    e.Lsr(RES_REG, ARG0_REG, ARG1_REG);
  }
}

EMITTER(ROR, CONSTRAINTS(REG_I64, REG_I64, IMM_I32)) {
  e.Ror(RES_REG, ARG0_REG,
        (unsigned)ir_zext_constant(ARG1) & (RES->type == VALUE_I64 ? 63 : 31));
}

EMITTER(ASHD, CONSTRAINTS(REG_I64, REG_I64, REG_I64)) {
  Label shr, overflow, end;
  e.Tst(ARG1_REG, 0x80000000);
  e.B(&shr, ne);
  e.Lsl(RES_REG, ARG0_REG, ARG1_REG);
  e.B(&end);
  e.Bind(&shr);
  e.Tst(ARG1_REG, 0x1f);
  e.B(&overflow, eq);
  e.Neg(w16, WRegister(ARG1_REG.GetCode()));
  e.Asr(RES_REG, ARG0_REG, w16);
  e.B(&end);
  e.Bind(&overflow);
  e.Asr(RES_REG, ARG0_REG, 31);
  e.Bind(&end);
}

EMITTER(LSHD, CONSTRAINTS(REG_I64, REG_I64, REG_I64)) {
  Label shr, overflow, end;
  e.Tst(ARG1_REG, 0x80000000);
  e.B(&shr, ne);
  e.Lsl(RES_REG, ARG0_REG, ARG1_REG);
  e.B(&end);
  e.Bind(&shr);
  e.Tst(ARG1_REG, 0x1f);
  e.B(&overflow, eq);
  e.Neg(w16, WRegister(ARG1_REG.GetCode()));
  e.Lsr(RES_REG, ARG0_REG, w16);
  e.B(&end);
  e.Bind(&overflow);
  e.Mov(RES_REG, 0);
  e.Bind(&end);
}

EMITTER(BRANCH, CONSTRAINTS(NONE, REG_I64 | IMM_I32 | IMM_BLK)) {
  a64_backend_emit_branch(backend, e, ir, ARG0);
}

EMITTER(BRANCH_COND,
        CONSTRAINTS(NONE, REG_I64 | IMM_I32 | IMM_BLK,
                    REG_I64 | IMM_I32 | IMM_BLK, REG_I64)) {
  Label next;
  e.Cbz(ARG2_REG, &next);
  a64_backend_emit_branch(backend, e, ir, ARG0);
  e.Bind(&next);
  a64_backend_emit_branch(backend, e, ir, ARG1);
}

EMITTER(EXIT_COND, CONSTRAINTS(NONE, IMM_I32, REG_I64, IMM_I32)) {
  /* the prolog charged the block for all of its instructions, so the exit gives
     back the cycles of the ones after it */
  int refund = 0;
  for (struct ir_instr *next = list_next_entry(instr, struct ir_instr, it);
       next; next = list_next_entry(next, struct ir_instr, it)) {
    if (next->op == OP_SOURCE_INFO) {
      refund += next->arg[1]->i32;
    }
  }

  const struct ir_value *target = ARG0;
  Label *ret;
  Label *exit = a64_backend_slow_path(backend, &ret, [=](MacroAssembler &e) {
    e.Add(w26, w26, refund);
    a64_backend_emit_branch(backend, e, ir, target);
  });

  if (ARG2->i32) {
    e.Cbnz(ARG1_REG, exit);
  } else {
    e.Cbz(ARG1_REG, exit);
  }
  e.Bind(ret);
}

EMITTER(CALL, CONSTRAINTS(NONE, VAL_I64, OPT_I64, OPT_I64)) {
  if (ARG1) {
    a64_backend_mov_value(backend, e, a64_arg0, ARG1);
  }
  if (ARG2) {
    a64_backend_mov_value(backend, e, a64_arg1, ARG2);
  }

  if (ir_is_constant(ARG0)) {
    e.Ldr(x16, (uint64_t)ARG0->i64);
    e.Blr(x16);
  } else {
    e.Blr(ARG0_REG);
  }
}

EMITTER(CALL_COND, CONSTRAINTS(NONE, VAL_I64, VAL_I64, OPT_I64, OPT_I64)) {
  Label skip;
  e.Cbz(ARG1_REG, &skip);

  if (ARG2) {
    a64_backend_mov_value(backend, e, a64_arg0, ARG2);
  }
  if (ARG3) {
    a64_backend_mov_value(backend, e, a64_arg1, ARG3);
  }

  if (ir_is_constant(ARG0)) {
    e.Ldr(x16, (uint64_t)ARG0->i64);
    e.Blr(x16);
  } else {
    e.Blr(ARG0_REG);
  }

  e.Bind(&skip);
}

EMITTER(DEBUG_BREAK, CONSTRAINTS(NONE)) { e.Brk(0); }

static void a64_debug_log(uint64_t a, uint64_t b, uint64_t c) {
  LOG_INFO("DEBUG_LOG a=0x%" PRIx64 " b=0x%" PRIx64 " c=0x%" PRIx64, a, b, c);
}

EMITTER(DEBUG_LOG, CONSTRAINTS(NONE, VAL_I64, OPT_I64, OPT_I64)) {
  a64_backend_mov_value(backend, e, a64_arg0, ARG0);
  if (ARG1) {
    a64_backend_mov_value(backend, e, a64_arg1, ARG1);
  }
  if (ARG2) {
    a64_backend_mov_value(backend, e, a64_arg2, ARG2);
  }
  e.Ldr(x16, (uint64_t)&a64_debug_log);
  e.Blr(x16);
}

EMITTER(ASSERT_EQ, CONSTRAINTS(NONE, REG_I64, REG_I64 | IMM_I32)) {
  if (ir_is_constant(ARG1)) {
    e.Cmp(ARG0_REG, (uint32_t)ir_zext_constant(ARG1));
  } else {
    e.Cmp(ARG0_REG, ARG1_REG);
  }
  e.B(a64_backend_stub(backend, 0, 0, NULL), ne);
}

EMITTER(ASSERT_LT, CONSTRAINTS(NONE, REG_I64, REG_I64)) {
  Label skip;
  e.Cmp(ARG0_REG, ARG1_REG);
  e.B(&skip, lt);
  e.Brk(0);
  e.Bind(&skip);
}

EMITTER(COPY, CONSTRAINTS(REG_ALL, VAL_ALL)) {
  if (ir_is_float(RES->type) || ir_is_vector(RES->type)) {
    int bits = RES->type == VALUE_F32 ? 32
               : RES->type == VALUE_F64 ? 64
                                        : 128;
    if (ir_is_constant(ARG0)) {
      if (RES->type == VALUE_F32) {
        uint32_t v;
        memcpy(&v, &ARG0->f32, 4);
        e.Mov(w16, v);
        e.Fmov(VRegister(RES_VREG.GetCode(), 32), w16);
      } else if (RES->type == VALUE_F64) {
        uint64_t v;
        memcpy(&v, &ARG0->f64, 8);
        e.Mov(x16, v);
        e.Fmov(VRegister(RES_VREG.GetCode(), 64), x16);
      } else {
        LOG_FATAL("unexpected vector constant");
      }
    } else if (ir_is_int(ARG0->type)) {
      /* the load store elimination pass reuses an integer as a float */
      e.Fmov(VRegister(RES_VREG.GetCode(), bits), ARG0_REG);
    } else {
      e.Fmov(VRegister(RES_VREG.GetCode(), bits),
             VRegister(ARG0_VREG.GetCode(), bits));
    }
  } else if (ir_is_float(ARG0->type) && !ir_is_constant(ARG0)) {
    /* and a float as an integer */
    e.Fmov(RES_REG, ARG0_VREG);
  } else {
    a64_backend_mov_value(backend, e, RES_REG, ARG0);
  }
}
