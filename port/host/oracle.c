/* Differential oracle: an Allegrex interpreter, and a bisector that compares it
 * against the recompiled code.
 *
 * Static recompilation fails quietly -- a wrong value is usually plausible,
 * and surfaces far from its cause. The cure is a second, independent
 * execution of the same code to diff against. This interpreter shares only
 * the *decoder* with the recompiler (both call a_decode), not the emitter, so
 * a control-flow or lowering bug in the emitter shows up as a divergence.
 *
 * Use (trace builds only, where every label executes PSP_MARK):
 *
 *   psp2i --oracle 0x08DE0C40 ...
 *
 * When that function is next entered, the oracle snapshots CPU and memory,
 * runs the function to completion in both engines from the same snapshot, and
 * compares the results. If they differ it binary-searches the number of
 * labels executed to find the first block after which register state
 * differs, prints both register files there, then restores the snapshot so
 * the game continues exactly as if nothing had happened.
 *
 * Scope: the integer ISA, Allegrex extensions, COP1 moves/loads/stores and
 * the control flow. Anything else (the VFPU, FPU arithmetic, firmware calls)
 * stops the interpreter with a message -- the oracle is for self-contained
 * routines such as decompressors, checksums and parsers.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprecomp/cpu.h>
#include <psprecomp/mem.h>
#include <psprecomp/hle.h>
#include <psprecomp/dispatch.h>
#include <psprecomp/recomp_rt.h>

#include "decode.h"

#define SENTINEL_RA 0x0FFFFFF0u

static uint32_t g_stub_lo, g_stub_hi;

/* ---- the interpreter ------------------------------------------------------ */

static char g_err[256];

#define R(n) psp_cpu.r[n]
static void wr(unsigned n, uint32_t v) { if (n) psp_cpu.r[n] = v; }

/* Execute one non-control-flow instruction. Returns 0, or -1 if unsupported. */
static int exec(const a_insn *in) {
    const uint32_t rs = R(in->rs), rt = R(in->rt);
    const uint32_t addr = rs + (uint32_t)in->imm;
    switch (in->op) {
    case A_NOP: case A_SYNC: case A_CACHE: case A_PREF: return 0;

    case A_SLL:   wr(in->rd, psp_sll(rt, in->sa)); return 0;
    case A_SRL:   wr(in->rd, psp_srl(rt, in->sa)); return 0;
    case A_SRA:   wr(in->rd, psp_sra(rt, in->sa)); return 0;
    case A_ROTR:  wr(in->rd, psp_rotr(rt, in->sa)); return 0;
    case A_SLLV:  wr(in->rd, psp_sll(rt, rs)); return 0;
    case A_SRLV:  wr(in->rd, psp_srl(rt, rs)); return 0;
    case A_SRAV:  wr(in->rd, psp_sra(rt, rs)); return 0;
    case A_ROTRV: wr(in->rd, psp_rotr(rt, rs)); return 0;

    case A_MOVZ: if (rt == 0) wr(in->rd, rs); return 0;
    case A_MOVN: if (rt != 0) wr(in->rd, rs); return 0;
    case A_MFHI: wr(in->rd, psp_cpu.hi); return 0;
    case A_MFLO: wr(in->rd, psp_cpu.lo); return 0;
    case A_MTHI: psp_cpu.hi = rs; return 0;
    case A_MTLO: psp_cpu.lo = rs; return 0;
    case A_CLZ:  wr(in->rd, psp_clz(rs)); return 0;
    case A_CLO:  wr(in->rd, psp_clo(rs)); return 0;

    case A_MULT:  psp_mult(rs, rt);  return 0;
    case A_MULTU: psp_multu(rs, rt); return 0;
    case A_DIV:   psp_div(rs, rt);   return 0;
    case A_DIVU:  psp_divu(rs, rt);  return 0;
    case A_MADD:  psp_madd(rs, rt);  return 0;
    case A_MADDU: psp_maddu(rs, rt); return 0;
    case A_MSUB:  psp_msub(rs, rt);  return 0;
    case A_MSUBU: psp_msubu(rs, rt); return 0;

    case A_ADD: case A_ADDU: wr(in->rd, rs + rt); return 0;
    case A_SUB: case A_SUBU: wr(in->rd, rs - rt); return 0;
    case A_AND:  wr(in->rd, rs & rt); return 0;
    case A_OR:   wr(in->rd, rs | rt); return 0;
    case A_XOR:  wr(in->rd, rs ^ rt); return 0;
    case A_NOR:  wr(in->rd, ~(rs | rt)); return 0;
    case A_SLT:  wr(in->rd, psp_slt(rs, rt)); return 0;
    case A_SLTU: wr(in->rd, psp_sltu(rs, rt)); return 0;
    case A_MAX:  wr(in->rd, psp_max(rs, rt)); return 0;
    case A_MIN:  wr(in->rd, psp_min(rs, rt)); return 0;

    case A_ADDI: case A_ADDIU: wr(in->rt, rs + (uint32_t)in->imm); return 0;
    case A_SLTI:  wr(in->rt, (int32_t)rs < in->imm); return 0;
    case A_SLTIU: wr(in->rt, rs < (uint32_t)in->imm); return 0;
    case A_ANDI:  wr(in->rt, rs & (uint32_t)in->imm); return 0;
    case A_ORI:   wr(in->rt, rs | (uint32_t)in->imm); return 0;
    case A_XORI:  wr(in->rt, rs ^ (uint32_t)in->imm); return 0;
    case A_LUI:   wr(in->rt, (uint32_t)in->imm << 16); return 0;

    case A_EXT:    wr(in->rt, psp_ext(rs, in->sa, in->rd + 1u)); return 0;
    case A_INS:    wr(in->rt, psp_ins(rt, rs, in->sa, in->rd - in->sa + 1u)); return 0;
    case A_WSBH:   wr(in->rd, psp_wsbh(rt)); return 0;
    case A_WSBW:   wr(in->rd, psp_wsbw(rt)); return 0;
    case A_SEB:    wr(in->rd, psp_seb(rt)); return 0;
    case A_SEH:    wr(in->rd, psp_seh(rt)); return 0;
    case A_BITREV: wr(in->rd, psp_bitrev(rt)); return 0;

    case A_LB:  wr(in->rt, (uint32_t)(int32_t)(int8_t)psp_read8(addr)); return 0;
    case A_LBU: wr(in->rt, psp_read8(addr)); return 0;
    case A_LH:  wr(in->rt, (uint32_t)(int32_t)(int16_t)psp_read16(addr)); return 0;
    case A_LHU: wr(in->rt, psp_read16(addr)); return 0;
    case A_LW: case A_LL: wr(in->rt, psp_read32(addr)); return 0;
    case A_LWL: wr(in->rt, psp_lwl(rt, addr)); return 0;
    case A_LWR: wr(in->rt, psp_lwr(rt, addr)); return 0;
    case A_SB:  psp_write8(addr, (uint8_t)rt); return 0;
    case A_SH:  psp_write16(addr, (uint16_t)rt); return 0;
    case A_SW:  psp_write32(addr, rt); return 0;
    case A_SC:  psp_write32(addr, rt); wr(in->rt, 1); return 0;
    case A_SWL: psp_swl(rt, addr); return 0;
    case A_SWR: psp_swr(rt, addr); return 0;

    case A_LWC1: psp_cpu.f[in->ft] = psp_read_f32(addr); return 0;
    case A_SWC1: psp_write_f32(addr, psp_cpu.f[in->ft]); return 0;
    case A_MTC1: psp_cpu.f[in->fs] = psp_bits_to_f32(rt); return 0;
    case A_MFC1: wr(in->rt, psp_f32_to_bits(psp_cpu.f[in->fs])); return 0;
    default: break;
    }
    snprintf(g_err, sizeof g_err, "unsupported instruction %s at 0x%08X", a_mnemonic(in->op), in->addr);
    return -1;
}

static int branch_taken(const a_insn *in) {
    const int32_t rs = (int32_t)R(in->rs), rt = (int32_t)R(in->rt);
    switch (in->op) {
    case A_BEQ: case A_BEQL:   return rs == rt;
    case A_BNE: case A_BNEL:   return rs != rt;
    case A_BLEZ: case A_BLEZL: return rs <= 0;
    case A_BGTZ: case A_BGTZL: return rs > 0;
    case A_BLTZ: case A_BLTZL: case A_BLTZAL: case A_BLTZALL: return rs < 0;
    case A_BGEZ: case A_BGEZL: case A_BGEZAL: case A_BGEZALL: return rs >= 0;
    case A_BC1F: case A_BC1FL: return !psp_fpu_cond();
    case A_BC1T: case A_BC1TL: return psp_fpu_cond();
    default: return -1;
    }
}

/* Run from `entry` until it returns (to SENTINEL_RA) or until `stop_at`
 * labels have been executed. A label is any address in the dispatch table --
 * the same set the recompiled code marks. Returns 0 on return, 1 when
 * stopped, -1 on error (g_err). */
static uint64_t g_interp_labels;
static uint32_t g_interp_stop_pc;

static int interp(uint32_t entry, uint64_t stop_at, uint64_t max_insns) {
    uint32_t pc = entry;
    g_interp_labels = 0;
    for (uint64_t n = 0; n < max_insns; n++) {
        if (pc == SENTINEL_RA) return 0;
        if (pc >= g_stub_lo && pc < g_stub_hi) {
            snprintf(g_err, sizeof g_err, "firmware call through stub 0x%08X", pc);
            return -1;
        }
        if (psp_lookup(pc) && ++g_interp_labels == stop_at) { g_interp_stop_pc = pc; return 1; }

        a_insn in;
        a_decode(psp_read32(pc), pc, &in);
        if (!in.has_delay_slot) {
            if (in.op == A_INVALID || exec(&in) != 0) {
                if (!g_err[0]) snprintf(g_err, sizeof g_err, "invalid instruction at 0x%08X", pc);
                return -1;
            }
            pc += 4;
            continue;
        }

        /* Control transfer: decide before the delay slot runs. */
        int taken = 1;
        uint32_t target = in.target;
        if (in.is_branch) {
            taken = branch_taken(&in);
            if (taken < 0) { snprintf(g_err, sizeof g_err, "unsupported branch %s at 0x%08X", a_mnemonic(in.op), pc); return -1; }
        } else if (in.is_indirect) {
            target = R(in.rs);
        }
        if (in.is_call) wr(in.op == A_JALR ? in.rd : 31, pc + 8);

        if (in.is_likely && !taken) { pc += 8; continue; }   /* slot nullified */

        a_insn slot;
        a_decode(psp_read32(pc + 4), pc + 4, &slot);
        if (slot.has_delay_slot) { snprintf(g_err, sizeof g_err, "branch in delay slot at 0x%08X", pc + 4); return -1; }
        if (exec(&slot) != 0) return -1;
        pc = taken ? target : pc + 8;
    }
    snprintf(g_err, sizeof g_err, "instruction budget exhausted");
    return -1;
}

/* ---- snapshots ------------------------------------------------------------- */

typedef struct {
    psp_cpu_state cpu;
    uint8_t *ram, *vram, *scratch;
} snapshot;

static void snap_alloc(snapshot *s) {
    s->ram = (uint8_t *)malloc(PSP_RAM_SIZE);
    s->vram = (uint8_t *)malloc(PSP_VRAM_SIZE);
    s->scratch = (uint8_t *)malloc(PSP_SCRATCH_SIZE);
}
static void snap_free(snapshot *s) { free(s->ram); free(s->vram); free(s->scratch); }
static void snap_take(snapshot *s) {
    s->cpu = psp_cpu;
    memcpy(s->ram, psp_mem.ram, PSP_RAM_SIZE);
    memcpy(s->vram, psp_mem.vram, PSP_VRAM_SIZE);
    memcpy(s->scratch, psp_mem.scratch, PSP_SCRATCH_SIZE);
}
static void snap_restore(const snapshot *s) {
    psp_cpu = s->cpu;
    memcpy(psp_mem.ram, s->ram, PSP_RAM_SIZE);
    memcpy(psp_mem.vram, s->vram, PSP_VRAM_SIZE);
    memcpy(psp_mem.scratch, s->scratch, PSP_SCRATCH_SIZE);
}

/* ---- the recompiled side, stoppable after N labels --------------------------- */

static jmp_buf  g_stop_jb;
static uint64_t g_rc_labels, g_rc_stop_at;
static uint32_t g_rc_stop_pc;
static int      g_rc_counting;

static void count_mark(uint32_t addr) {
    if (!g_rc_counting) return;
    if (++g_rc_labels == g_rc_stop_at) { g_rc_stop_pc = addr; longjmp(g_stop_jb, 1); }
}

/* A dispatch miss inside an oracle run must not end the process: it is a
 * result like any other (the recompiled code went somewhere unregistered). */
static uint32_t g_rc_miss;
static void oracle_miss(uint32_t addr) { g_rc_miss = addr; longjmp(g_stop_jb, 2); }

/* 0 = returned, 1 = stopped, 2 = dispatch miss. */
static int recompiled(uint32_t entry, uint64_t stop_at) {
    g_rc_labels = 0;
    g_rc_stop_at = stop_at;
    g_rc_counting = 1;
    g_rc_miss = 0;
    psp_set_miss_handler(oracle_miss);
    int j = setjmp(g_stop_jb);
    if (j) { g_rc_counting = 0; psp_set_miss_handler(NULL); return j; }
    psp_dispatch(entry);
    g_rc_counting = 0;
    psp_set_miss_handler(NULL);
    return 0;
}

/* ---- comparison ------------------------------------------------------------- */

static int same_regs(const psp_cpu_state *a, const psp_cpu_state *b) {
    for (int i = 1; i < 32; i++) {
        if (i == PSP_REG_RA) continue;   /* the two engines link differently at the top */
        if (a->r[i] != b->r[i]) return 0;
    }
    return a->hi == b->hi && a->lo == b->lo;
}

static void print_diff(const psp_cpu_state *a, const psp_cpu_state *b) {
    for (int i = 1; i < 32; i++)
        if (a->r[i] != b->r[i])
            fprintf(stderr, "    %-4s interp 0x%08X   recompiled 0x%08X\n",
                    psp_reg_names[i], a->r[i], b->r[i]);
    if (a->hi != b->hi) fprintf(stderr, "    hi   interp 0x%08X   recompiled 0x%08X\n", a->hi, b->hi);
    if (a->lo != b->lo) fprintf(stderr, "    lo   interp 0x%08X   recompiled 0x%08X\n", a->lo, b->lo);
}

static void print_mem_diff(const uint8_t *a, const uint8_t *b) {
    int shown = 0;
    for (uint32_t off = 0; off < PSP_RAM_SIZE && shown < 8; off += 4) {
        uint32_t x, y;
        memcpy(&x, a + off, 4);
        memcpy(&y, b + off, 4);
        if (x == y) continue;
        uint32_t end = off;
        while (end < PSP_RAM_SIZE && memcmp(a + end, b + end, 4) != 0) end += 4;
        fprintf(stderr, "    memory differs 0x%08X..0x%08X (first word interp 0x%08X, recompiled 0x%08X)\n",
                PSP_RAM_BASE + off, PSP_RAM_BASE + end, x, y);
        off = end;
        shown++;
    }
}

/* ---- one differential check ---------------------------------------------------- */

static snapshot g_s0, g_a, g_b;
static int g_snaps_ready;

/* Check `addr` from the current state, then restore that state. Returns 0 on
 * a match, 1 on a mismatch (bisected and reported), -1 if the interpreter
 * could not run it to completion (firmware call, VFPU, no return). */
static int check_function(uint32_t addr, int verbose, uint64_t budget) {
    if (!g_snaps_ready) { snap_alloc(&g_s0); snap_alloc(&g_a); snap_alloc(&g_b); g_snaps_ready = 1; }
    snap_take(&g_s0);
    psp_trace_set_mark_hook(count_mark);

    psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
    g_err[0] = '\0';
    int ri = interp(addr, 0, budget);
    uint64_t li = g_interp_labels;
    if (ri < 0) {
        if (verbose) fprintf(stderr, "  0x%08X: interpreter stopped: %s (after %llu labels)\n",
                             addr, g_err, (unsigned long long)li);
        psp_trace_set_mark_hook(NULL);
        snap_restore(&g_s0);
        return -1;
    }
    snap_take(&g_a);
    snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
    recompiled(addr, 0);
    uint64_t lr = g_rc_labels;
    snap_take(&g_b);

    int regs_ok = same_regs(&g_a.cpu, &g_b.cpu);
    int mem_ok = memcmp(g_a.ram, g_b.ram, PSP_RAM_SIZE) == 0;
    int rc = 0;
    if (regs_ok && mem_ok && li == lr) {
        if (verbose) fprintf(stderr, "  0x%08X: MATCH (%llu labels)\n", addr, (unsigned long long)li);
    } else {
        rc = 1;
        fprintf(stderr, "\n==== oracle MISMATCH in 0x%08X (a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X) ====\n",
                addr, g_s0.cpu.r[PSP_REG_A0], g_s0.cpu.r[PSP_REG_A1], g_s0.cpu.r[PSP_REG_A2], g_s0.cpu.r[PSP_REG_A3]);
        fprintf(stderr, "  labels executed: interpreter %llu, recompiled %llu\n",
                (unsigned long long)li, (unsigned long long)lr);
        if (!regs_ok) print_diff(&g_a.cpu, &g_b.cpu);
        if (!mem_ok) print_mem_diff(g_a.ram, g_b.ram);

        /* Bisect: the first N at which the state after N labels differs. */
        uint64_t lo = 1, hi = li < lr ? li : lr;
        if (hi == 0) hi = 1;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
            interp(addr, mid, budget);
            uint32_t pi = g_interp_stop_pc;
            snap_take(&g_a);
            snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
            recompiled(addr, mid);
            uint32_t pr = g_rc_stop_pc;
            snap_take(&g_b);
            int same = pi == pr && same_regs(&g_a.cpu, &g_b.cpu) &&
                       memcmp(g_a.ram, g_b.ram, PSP_RAM_SIZE) == 0;
            if (same) lo = mid + 1; else hi = mid;
        }
        uint32_t prev = 0;
        if (lo > 1) {
            snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
            interp(addr, lo - 1, budget);
            prev = g_interp_stop_pc;
        }
        snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
        int si = interp(addr, lo, budget);
        uint32_t pi = si == 1 ? g_interp_stop_pc : 0;
        snap_take(&g_a);
        snap_restore(&g_s0); psp_cpu.r[PSP_REG_RA] = SENTINEL_RA;
        int sr = recompiled(addr, lo);
        uint32_t pr = sr == 1 ? g_rc_stop_pc : 0;
        snap_take(&g_b);
        fprintf(stderr, "  FIRST DIVERGENCE at label #%llu\n", (unsigned long long)lo);
        fprintf(stderr, "    last agreeing label: 0x%08X\n", prev);
        fprintf(stderr, "    next label: interpreter 0x%08X%s, recompiled 0x%08X%s\n",
                pi, si == 1 ? "" : " (returned)", pr, sr == 1 ? "" : " (returned)");
        print_diff(&g_a.cpu, &g_b.cpu);
        print_mem_diff(g_a.ram, g_b.ram);
        fprintf(stderr, "==== end of mismatch report ====\n\n");
    }

    psp_trace_set_mark_hook(NULL);
    snap_restore(&g_s0);
    return rc;
}

/* ---- hooks ---------------------------------------------------------------------- */

static uint32_t g_oracle_addr;
static int g_busy;

static void oracle_one(uint32_t addr) {
    static int done;
    if (g_busy || done) return;
    g_busy = 1;
    done = 1;
    fprintf(stderr, "\n==== oracle: 0x%08X ====\n", addr);
    check_function(addr, 1, 2000000000ull);
    fprintf(stderr, "==== oracle done; continuing the game from the snapshot ====\n\n");
    g_busy = 0;
}

/* Sweep: every function, on its first call. */
static uint8_t *g_seen;
static uint32_t g_seen_lo = 0x08800000u, g_seen_words = 0x00800000u >> 2;
static uint64_t g_n_match, g_n_mismatch, g_n_skip;

static void oracle_sweep(uint32_t addr) {
    if (g_busy) return;
    if (addr < g_seen_lo || addr - g_seen_lo >= g_seen_words * 4) return;
    uint32_t i = (addr - g_seen_lo) >> 2;
    if (g_seen[i]) return;
    g_seen[i] = 1;
    g_busy = 1;
    int rc = check_function(addr, 0, 20000000ull);
    if (rc == 0) g_n_match++;
    else if (rc > 0) g_n_mismatch++;
    else g_n_skip++;
    g_busy = 0;
}

void oracle_arm(uint32_t addr, uint32_t stub_lo, uint32_t stub_hi) {
    g_oracle_addr = addr;
    g_stub_lo = stub_lo;
    g_stub_hi = stub_hi;
    psp_trace_watch(addr, oracle_one);
}

void oracle_arm_sweep(uint32_t stub_lo, uint32_t stub_hi) {
    g_stub_lo = stub_lo;
    g_stub_hi = stub_hi;
    g_seen = (uint8_t *)calloc(g_seen_words, 1);
    psp_trace_set_enter_hook(oracle_sweep);
}

void oracle_report(FILE *out) {
    if (!g_seen) return;
    fprintf(out, "  oracle sweep: %llu functions matched, %llu MISMATCHED, %llu skipped "
                 "(firmware/VFPU/no return)\n",
            (unsigned long long)g_n_match, (unsigned long long)g_n_mismatch,
            (unsigned long long)g_n_skip);
}

/* ---- memory dump at a function entry --------------------------------------------- */

static uint32_t g_dump_addr;
static char g_dump_path[512];

static void dump_hook(uint32_t addr) {
    static int done;
    if (done || addr != g_dump_addr) return;
    done = 1;
    FILE *f = fopen(g_dump_path, "wb");
    if (f) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, f); fclose(f); }
    fprintf(stderr, "dump: RAM at entry to 0x%08X written to %s\n", addr, g_dump_path);
    for (int i = 1; i < 32; i++)
        fprintf(stderr, "  %-4s 0x%08X%s", psp_reg_names[i], psp_cpu.r[i], i % 4 == 0 ? "\n" : "");
    fprintf(stderr, "\n");
}

void dump_arm(uint32_t addr, const char *path) {
    g_dump_addr = addr;
    snprintf(g_dump_path, sizeof g_dump_path, "%s", path);
    psp_trace_watch_label(addr, dump_hook);
}

/* ---- arguments at a function entry ------------------------------------------------ */

/* --args-from-flip ADDR FLIP: from display flip FLIP on, print the argument
 * registers, the return address and the 16 floats at $a0 each time ADDR is
 * entered (the first 24 times). For finding which caller hands a function a
 * bad matrix and what the matrix holds. */
static uint32_t g_args_addr;
static uint64_t g_args_flip;

static void args_hook(uint32_t addr) {
    static int n;
    if (addr != g_args_addr || psp_display_flips() < g_args_flip || n >= 24) return;
    n++;
    fprintf(stderr, "args: 0x%08X flip %llu a0 0x%08X a1 0x%08X a2 0x%08X a3 0x%08X ra 0x%08X\n",
            addr, (unsigned long long)psp_display_flips(), psp_cpu.r[4], psp_cpu.r[5], psp_cpu.r[6],
            psp_cpu.r[7], psp_cpu.r[31]);
    uint32_t m = psp_cpu.r[4];
    if (m < 0x08000000u || m >= 0x0A000000u) return;
    fprintf(stderr, "      [a0]");
    for (int i = 0; i < 16; i++) {
        uint32_t w = psp_read32(m + (uint32_t)i * 4);
        float f;
        memcpy(&f, &w, 4);
        fprintf(stderr, " %.4g%s", f, i % 4 == 3 && i < 15 ? " |" : "");
    }
    fprintf(stderr, "\n");
}

void args_arm(uint32_t addr, uint64_t flip) {
    g_args_addr = addr;
    g_args_flip = flip;
    psp_trace_watch(addr, args_hook);
}

static char g_bad_dump_path[512];
static void bad_dump_hook(uint32_t addr) {
    FILE *f = fopen(g_bad_dump_path, "wb");
    if (f) { fwrite(psp_mem.ram, 1, PSP_RAM_SIZE, f); fclose(f); }
    fprintf(stderr, "dump: RAM at first bad access (0x%08X) written to %s\n", addr, g_bad_dump_path);
}

void dump_on_bad_access(const char *path) {
    snprintf(g_bad_dump_path, sizeof g_bad_dump_path, "%s", path);
    psp_mem_set_first_bad_hook(bad_dump_hook);
}

/* ---- recent-label ring, printed at the first bad access ------------------------------ */

#define RING 96
static uint32_t g_ring[RING];
static uint64_t g_ring_n;
static void ring_mark(uint32_t addr) { g_ring[g_ring_n++ % RING] = addr; }

static void ring_print(uint32_t bad) {
    fprintf(stderr, "last %d labels before the bad access at 0x%08X (oldest first):\n", RING, bad);
    uint64_t start = g_ring_n > RING ? g_ring_n - RING : 0;
    for (uint64_t i = start; i < g_ring_n; i++)
        fprintf(stderr, "  0x%08X%s", g_ring[i % RING], (i - start) % 8 == 7 ? "\n" : "");
    fprintf(stderr, "\n");
}

static void bad_ring_hook(uint32_t addr) {
    if (g_bad_dump_path[0]) bad_dump_hook(addr);
    ring_print(addr);
}

void ring_arm(void) {
    psp_trace_set_mark_hook(ring_mark);
    psp_mem_set_first_bad_hook(bad_ring_hook);
}
