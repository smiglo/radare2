/* radare2 - LGPL - Copyright 2017-2018 - xvilka */

#include <string.h>
#include <r_types.h>
#include <r_lib.h>
#include <r_asm.h>
#include <r_anal.h>
#undef R_IPI
#define R_IPI static
#include "../../bin/format/wasm/wasm.h"
#include "../../asm/arch/wasm/wasm.c"

#define WASM_STACK_SIZE 256
#define WASM_END_SIZE (1)

static struct wasm_stack_t {
	ut64 loop;
	ut64 end;
	int size;
} wasm_stack [WASM_STACK_SIZE];

static int wasm_stack_ptr = 0;
static ut64 addr_old = 0;
static WasmOpCodes op_old = 0;

// finds the address of the call function (essentially where to jump to).
static ut64 get_cf_offset(RAnal *anal, const ut8 *data) {
	r_cons_push ();
	char *s = anal->coreb.cmdstrf (anal->coreb.core, "isq~[0:%d]", data[1]);
	r_cons_pop ();
	if (s) {
		ut64 n = r_num_get (NULL, s);
		free (s);
		return n;
	}
	return UT64_MAX;
}

/*
 * Searches where the opcode ends.
 * A wasm scope is formed by:
 *   block/loop -> end
 *   if -> end
 *   if -> else -> end
 * Also a function call has an end.
 *   call X -> end
 */
static ut64 find_scope(ut64 addr, const ut8 *data, int len, bool is_loop) {
	WasmOp wop = {0};
	st32 count = 0;
	ut32 offset = addr;
	while (len > 0) {
		int ret = wasm_dis (&wop, data, len);
		switch (wop.op) {
		/* Calls here are using index instead of address */
		case WASM_OP_BLOCK:
			count++;
			break;
		case WASM_OP_LOOP:
			count++;
			break;
		case WASM_OP_IF:
			count++;
			break;
		case WASM_OP_ELSE:
			if (!count && !is_loop) {
				return offset + ret;
			}
			break;
		case WASM_OP_END:
			if (!count) {
				return offset;
			} else {
				count--;
			}
			break;
		default:
			break;
		}
		offset += ret;
		data += ret;
		len -= ret;
	}
	return UT64_MAX;
}

// set where the branch instruction should go

static void set_br_jump(RAnalOp *op, const ut8 *data, int len) {
	ut32 val;
	read_u32_leb128 (data + 1, data + len, &val);
	ut32 pos = wasm_stack_ptr - (val + 1);
	if (pos < wasm_stack_ptr) {
		ut64 jump = wasm_stack[pos].loop;
		if (jump != UT64_MAX) {
			// if it is a loop, branch to beginning.
			op->jump = jump;
		} else {
			// if it is a block, branch to the end.
			jump = wasm_stack[pos].end;
			if (jump != UT64_MAX) {
				// always pointing to an 'end'
				op->jump = jump + 1;
			}
		}
	}
}

// analyzes the wasm opcode.
static int wasm_op(RAnal *anal, RAnalOp *op, ut64 addr, const ut8 *data, int len) {
	ut64 addr2 = UT64_MAX;
	int i;
	WasmOp wop = {0};
	memset (op, '\0', sizeof (RAnalOp));
	int ret = wasm_dis (&wop, data, len);
	op->jump = UT64_MAX;
	op->fail = UT64_MAX;
	op->ptr = op->val = UT64_MAX;
	op->size = ret;
	op->addr = addr;
	op->sign = true;
	op->type = R_ANAL_OP_TYPE_UNK;
	op->id = wop.op;

	if (!strncmp (wop.txt, "invalid", 7)) {
		op->type = R_ANAL_OP_TYPE_ILL;
		wasm_stack_ptr = 0;
		return -1;
	}
	if (wasm_stack_ptr >= WASM_STACK_SIZE) {
		wasm_stack_ptr = 0;
		op->type = R_ANAL_OP_TYPE_NULL;
		return -1;
	}
	switch (wop.op) {
	/* Calls here are using index instead of address */
	case WASM_OP_LOOP:
		addr2 = find_scope (addr + op->size, data + op->size, len - op->size, true);
		op->type = R_ANAL_OP_TYPE_NOP;
		if (addr2 != UT64_MAX && addr_old != addr) {
			//eprintf("0x%016x > stack %u (loop)\n", addr, wasm_stack_ptr);
			wasm_stack[wasm_stack_ptr].loop = addr;
			wasm_stack[wasm_stack_ptr].end = addr2;
			wasm_stack[wasm_stack_ptr].size = wop.len;
			wasm_stack_ptr++;
		}
		//op->fail = addr + op->size;
		break;
	case WASM_OP_BLOCK:
		op->type = R_ANAL_OP_TYPE_NOP;
		addr2 = find_scope (addr + op->size, data + op->size, len - op->size, true);
		if (addr2 != UT64_MAX && addr_old != addr) {
			//eprintf("0x%016x > stack %u (block)\n", addr, wasm_stack_ptr);
			wasm_stack[wasm_stack_ptr].loop = UT64_MAX;
			wasm_stack[wasm_stack_ptr].end = addr2;
			wasm_stack[wasm_stack_ptr].size = wop.len;
			wasm_stack_ptr++;
		}
		break;
	case WASM_OP_IF:
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = find_scope (addr + op->size, data + op->size, len - op->size, false);
		op->fail = addr + op->size;
		if (op->jump != UT64_MAX && addr_old != addr) {
			//eprintf("0x%016x > stack %u (if)\n", addr, wasm_stack_ptr);
			wasm_stack[wasm_stack_ptr].loop = UT64_MAX;
			wasm_stack[wasm_stack_ptr].end = op->fail;
			wasm_stack[wasm_stack_ptr].size = wop.len;
			wasm_stack_ptr++;
		}
		break;
	case WASM_OP_ELSE:
		op->type = R_ANAL_OP_TYPE_JMP;
		op->jump = find_scope (addr + op->size, data + op->size, len - op->size, false);
		break;
	case WASM_OP_I32REMS:
	case WASM_OP_I32REMU:
		op->type = R_ANAL_OP_TYPE_MOD;
		break;
	case WASM_OP_END:
		//eprintf("0x%016x < stack %u (end)\n", addr, wasm_stack_ptr);
		if (wasm_stack_ptr > 0) {
			op->type = R_ANAL_OP_TYPE_NOP;
			if (addr != UT64_MAX) {
				for (i = wasm_stack_ptr - 1; i > 0; i--) {
					if (wasm_stack[i].end == addr && wasm_stack[i].loop != UT64_MAX) {
						op->type = R_ANAL_OP_TYPE_CJMP;
						op->jump = wasm_stack[i].loop;
						op->fail = addr + op->size;
						break;
					}
				}
			}
			wasm_stack_ptr--;
		} else {
			// all wasm routines ends with an end.
			op->eob = true;
			op->type = R_ANAL_OP_TYPE_RET;
		}
		break;
	case WASM_OP_GETLOCAL:
	case WASM_OP_I32LOAD:
	case WASM_OP_I64LOAD:
	case WASM_OP_F32LOAD:
	case WASM_OP_F64LOAD:
	case WASM_OP_I32LOAD8S:
	case WASM_OP_I32LOAD8U:
	case WASM_OP_I32LOAD16S:
	case WASM_OP_I32LOAD16U:
	case WASM_OP_I64LOAD8S:
	case WASM_OP_I64LOAD8U:
	case WASM_OP_I64LOAD16S:
	case WASM_OP_I64LOAD16U:
	case WASM_OP_I64LOAD32S:
	case WASM_OP_I64LOAD32U:
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case WASM_OP_SETLOCAL:
	case WASM_OP_TEELOCAL:
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case WASM_OP_I32EQZ:
	case WASM_OP_I32EQ:
	case WASM_OP_I32NE:
	case WASM_OP_I32LTS:
	case WASM_OP_I32LTU:
	case WASM_OP_I32GTS:
	case WASM_OP_I32GTU:
	case WASM_OP_I32LES:
	case WASM_OP_I32LEU:
	case WASM_OP_I32GES:
	case WASM_OP_I32GEU:
	case WASM_OP_I64EQZ:
	case WASM_OP_I64EQ:
	case WASM_OP_I64NE:
	case WASM_OP_I64LTS:
	case WASM_OP_I64LTU:
	case WASM_OP_I64GTS:
	case WASM_OP_I64GTU:
	case WASM_OP_I64LES:
	case WASM_OP_I64LEU:
	case WASM_OP_I64GES:
	case WASM_OP_I64GEU:
	case WASM_OP_F32EQ:
	case WASM_OP_F32NE:
	case WASM_OP_F32LT:
	case WASM_OP_F32GT:
	case WASM_OP_F32LE:
	case WASM_OP_F32GE:
	case WASM_OP_F64EQ:
	case WASM_OP_F64NE:
	case WASM_OP_F64LT:
	case WASM_OP_F64GT:
	case WASM_OP_F64LE:
	case WASM_OP_F64GE:
		op->type = R_ANAL_OP_TYPE_CMP;
		break;
	case WASM_OP_I64OR:
	case WASM_OP_I32OR:
		op->type = R_ANAL_OP_TYPE_OR;
		break;
	case WASM_OP_I64XOR:
	case WASM_OP_I32XOR:
		op->type = R_ANAL_OP_TYPE_XOR;
		break;
	case WASM_OP_I32CONST:
	case WASM_OP_I64CONST:
	case WASM_OP_F32CONST:
	case WASM_OP_F64CONST:
		op->type = R_ANAL_OP_TYPE_MOV;
		{
			ut8 arg = data[1];
			r_strbuf_setf (&op->esil, "4,sp,-=,%d,sp,=[4]", arg);
		}
		break;
	case WASM_OP_I64ADD:
	case WASM_OP_I32ADD:
	case WASM_OP_F32ADD:
	case WASM_OP_F64ADD:
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case WASM_OP_I64SUB:
	case WASM_OP_I32SUB:
	case WASM_OP_F32SUB:
	case WASM_OP_F64SUB:
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case WASM_OP_NOP:
		op->type = R_ANAL_OP_TYPE_NOP;
		r_strbuf_setf (&op->esil, "");
		break;
	case WASM_OP_CALL:
	case WASM_OP_CALLINDIRECT:
		op->type = R_ANAL_OP_TYPE_CALL;
		op->jump = get_cf_offset (anal, data);
		op->fail = addr + op->size;
		if (op->jump != UT64_MAX) {
			op->ptr = op->jump;
		}
		r_strbuf_setf (&op->esil, "4,sp,-=,0x%"PFMT64x",sp,=[4],0x%"PFMT64x",pc,=", op->fail, op->jump);
		break;
	case WASM_OP_BR:
		op->type = R_ANAL_OP_TYPE_JMP;
		set_br_jump(op, data, len - op->size);
		break;
	case WASM_OP_BRIF:
		op->fail = addr + op->size;
		op->type = R_ANAL_OP_TYPE_CJMP;
		set_br_jump(op, data, len - op->size);
		break;
	case WASM_OP_RETURN:
		// should be ret, but if there the analisys is stopped.
		op->type = R_ANAL_OP_TYPE_CRET;
	default:
		break;
	}
	op_old = wop.op;
	addr_old = addr;
	return op->size;
}

static int archinfo(RAnal *a, int q) {
	return 1;
}

static int wasm_pre_anal(RAnal *a, struct r_anal_state_type_t *state, ut64 addr) {
	int i;
	for (i = 0; i < WASM_STACK_SIZE; ++i) {
		wasm_stack[i].loop = UT64_MAX;
		wasm_stack[i].end = UT64_MAX;
	}
	wasm_stack_ptr = 0;
	return 1;
}

static char *get_reg_profile(RAnal *anal) {
	return strdup (
		"=PC	pc\n"
		"=BP	bp\n"
		"=SP	sp\n"
		"gpr	sp	.32	0	0\n" // stack pointer
		"gpr	pc	.32	4	0\n" // program counter
		"gpr	bp	.32	8	0\n" // base pointer // unused
	);
}

RAnalPlugin r_anal_plugin_wasm = {
	.name = "wasm",
	.desc = "WebAssembly analysis plugin",
	.license = "LGPL3",
	.arch = "wasm",
	.bits = 64,
	.archinfo = archinfo,
	.pre_anal_fn_cb = wasm_pre_anal,
	.get_reg_profile = get_reg_profile,
	.op = &wasm_op,
	.esil = true,
};

#ifndef CORELIB
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_ANAL,
	.data = &r_anal_plugin_wasm,
	.version = R2_VERSION
};
#endif
