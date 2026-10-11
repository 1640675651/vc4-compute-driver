#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Probes VideoCore IV QPU behavior relevant for running OpenCL work-items as SIMD lanes, using hand-assembled QPU
programs submitted through the vc4 compute ioctls (vc4_compute_drm.h).

  A: Does a per-lane condition on a VPM write keep the other lanes' VPM contents?
  B: Can a VPM-to-memory DMA (VDW) store a partial row (start column, width)?
  C: Does a per-lane condition on a TMU address write (tmu0_s) suppress the request for masked lanes, and what do
     masked lanes read?
  D: What happens if a TMU address write has all lanes masked (no lane writes an address)? Run with --test-d. May hang
     the QPU, which the driver resets after the 1 s timeout.

Needs the patched vc4 module and root (or the compute_gid group).

Results (2026-10-02, Raspberry Pi 3B, V3D at 300 MHz):

  A: No. A write to vpm always writes all 16 lanes, even with condition "never". Masked lanes receive a stale value:
     the result of the previous instruction (e.g. 1 for odd lanes after "and.setf -, elem_num, 1").
  B: Yes. Start column 3, width 5 wrote exactly 5 words; the rest of the destination was untouched.
  C: No. A write to tmu0_s issues a request for all 16 lanes. Masked lanes read from undefined addresses (garbage).
  D: The request is still issued (no hang), all 16 lanes return garbage.

So per-lane conditions only work for register writes. For running work-items as lanes, this means:
  - Loads: select a safe address for inactive lanes in a register (conditional register write), then write tmu0_s
    unconditionally. Skip loads entirely (branch) if no lane is active.
  - Stores: VPM writes cannot be masked. Contiguous stores of a prefix of lanes (bounds checks) can use a shorter
    VDW DMA. Otherwise either merge the old memory contents in registers before an unconditional VPM write (read-
    modify-write, only safe if no other work-item writes the same row concurrently), or store each active lane with
    its own one-word VDW DMA (start column = lane).
"""
import ctypes
import fcntl
import mmap
import os
import struct
import sys

# --- QPU instruction encoding (VideoCore IV 3D Architecture Reference Guide, checked against VC4C below) ---
SIG_NONE, SIG_THREAD_END, SIG_LOAD_TMU0, SIG_SMALL_IMM, SIG_LOAD_IMM = 1, 3, 10, 13, 14
COND_NEVER, COND_ALWAYS, COND_ZS, COND_ZC = 0, 1, 2, 3
OP_NOP, OP_ADD, OP_SHL, OP_AND, OP_OR = 0, 12, 17, 20, 21
# input muxes
R0, R1, R2, R3, R4, R5, RA, RB = range(8)
# write addresses (with ws=1 for the B-side registers vw_setup / vw_addr)
W_R0, W_R1, W_R2, W_R3 = 32, 33, 34, 35
W_TMU_NOSWAP, W_NOP, W_VPM, W_VPM_SETUP, W_VPM_ADDR, W_MUTEX_RELEASE, W_TMU0_S, W_TMU1_S = 36, 39, 48, 49, 50, 51, 56, 60
# read addresses
A_UNIFORM, A_ELEM_NUM, A_NOP, B_VPM_WAIT, A_MUTEX_ACQUIRE = 32, 38, 39, 50, 51


def alu(op_add=OP_NOP, waddr_add=W_NOP, add_a=R0, add_b=R0, raddr_a=A_NOP, raddr_b=A_NOP, cond_add=COND_ALWAYS,
        sf=0, ws=0, sig=SIG_NONE, waddr_mul=W_NOP):
    if op_add == OP_NOP:
        cond_add = COND_NEVER
    hi = (sig << 28) | (cond_add << 17) | (sf << 13) | (ws << 12) | (waddr_add << 6) | waddr_mul
    lo = (op_add << 24) | (raddr_a << 18) | (raddr_b << 12) | (add_a << 9) | (add_b << 6)
    return lo, hi


def small_imm(op_add, waddr_add, add_a, imm, raddr_a=A_NOP, cond_add=COND_ALWAYS, sf=0, ws=0):
    """op_add with the second operand (or both) being the small immediate imm (0-15)"""
    assert 0 <= imm <= 15
    return alu(op_add, waddr_add, add_a, RB, raddr_a=raddr_a, raddr_b=imm, cond_add=cond_add, sf=sf, ws=ws,
               sig=SIG_SMALL_IMM)


def ldi(waddr_add, value, ws=0):
    hi = (SIG_LOAD_IMM << 28) | (COND_ALWAYS << 17) | (ws << 12) | (waddr_add << 6) | W_NOP
    return value & 0xFFFFFFFF, hi


def nop(sig=SIG_NONE):
    return alu(sig=sig)


def vpm_write_setup(row):
    """horizontal, 32-bit, stride 1, starting at the given row"""
    return (1 << 12) | (1 << 11) | (2 << 8) | row


def vdw_setup(units, depth, row, column):
    """VPM -> memory DMA: units rows of depth words each, horizontal, 32-bit, starting at (row, column)"""
    return (2 << 30) | ((units & 0x7F) << 23) | ((depth & 0x7F) << 16) | (1 << 14) | (((row << 4) | column) << 3)


VDW_STRIDE_SETUP = 0xC0010000  # as used by VC4C: stride 0, block mode


def validate_encoder():
    # (expected low, high) as encoded by VC4C, and our encoding of the same instruction
    cases = [
        ((0x009E7000, 0x100009E7), nop(), "nop"),
        ((0x009E7000, 0x300009E7), nop(SIG_THREAD_END), "nop.thrend"),
        ((0x159C1FC0, 0xD0020927), small_imm(OP_OR, W_TMU_NOSWAP, RB, 1), "or tmu_noswap, 1, 1"),
        ((0x15827D80, 0x10020027), alu(OP_OR, 0, RA, RA, raddr_a=A_UNIFORM), "or ra0, unif, unif"),
        ((0x15827D80, 0x100200A7), alu(OP_OR, 2, RA, RA, raddr_a=A_UNIFORM), "or ra2, unif, unif"),
        ((0x149C11C0, 0xD00229E7), small_imm(OP_AND, W_NOP, R0, 1, sf=1), "and.setf -, r0, 1"),
        ((0x159E7000, 0x100610A7), alu(OP_OR, 2, R0, R0, cond_add=COND_ZC, ws=1), "or.ifzc rb2, r0, r0"),
        ((0x15CE7D80, 0x100209E7), alu(OP_OR, W_NOP, RA, RA, raddr_a=A_MUTEX_ACQUIRE), "or -, mutex_acq, mutex_acq"),
        ((0x00001A00, 0xE0021C67), ldi(W_VPM_SETUP, vpm_write_setup(0), ws=1), "ldi vpw_setup, vpm_setup(row 0)"),
        ((0x80814000, 0xE0021C67), ldi(W_VPM_SETUP, vdw_setup(1, 1, 0, 0), ws=1), "ldi vpw_setup, vdw_setup(1, 1)"),
        ((0x0C080DC0, 0x10021CA7), alu(OP_ADD, W_VPM_ADDR, RA, RB, raddr_a=2, raddr_b=0, ws=1), "add vpw_addr, ra2, rb0"),
        ((0x159F2FC0, 0x100209E7), alu(OP_OR, W_NOP, RB, RB, raddr_b=B_VPM_WAIT), "or -, vpw_wait, vpw_wait"),
        ((0x159C1FC0, 0xD0020CE7), small_imm(OP_OR, W_MUTEX_RELEASE, RB, 1), "or mutex_rel, 1, 1"),
        ((0x159E7B40, 0x10020F27), alu(OP_OR, W_TMU1_S, R5, R5), "or tmu1s, r5, r5"),
        ((0x119C2BC0, 0xD0020827), small_imm(OP_SHL, W_R0, R5, 2), "shl r0, r5, 2"),
    ]
    ok = True
    for expected, actual, text in cases:
        if tuple(expected) != tuple(actual):
            ok = False
            print(f"  encoder MISMATCH for {text}: expected {expected[1]:08x}_{expected[0]:08x}, "
                  f"got {actual[1]:08x}_{actual[0]:08x}")
    print(f"Encoder check: {len(cases)} instructions, {'all match VC4C' if ok else 'MISMATCHES'}")
    return ok


def program_main():
    """Tests A, B and C. UNIFORMs: out address, in address."""
    p = []
    p.append(small_imm(OP_OR, W_TMU_NOSWAP, RB, 1))  # TMU0 is TMU0
    p.append(alu(OP_OR, 0, RA, RA, raddr_a=A_UNIFORM))  # ra0 = out
    p.append(alu(OP_OR, 1, RA, RA, raddr_a=A_UNIFORM))  # ra1 = in
    p.append(alu(OP_OR, W_R1, RA, RA, raddr_a=A_ELEM_NUM))  # r1 = lane
    p.append(small_imm(OP_SHL, W_R2, R1, 2))  # r2 = lane * 4

    # A: row 0 = 100 + lane, then (even lanes only) 200 + lane
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(0), ws=1))
    p.append(ldi(W_R3, 100))
    p.append(alu(OP_ADD, W_VPM, R1, R3))
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(0), ws=1))
    p.append(small_imm(OP_AND, W_NOP, R1, 1, sf=1))  # Z set for even lanes
    p.append(ldi(W_R3, 200))
    p.append(alu(OP_ADD, W_VPM, R1, R3, cond_add=COND_ZS))

    # B (data): row 1 = 300 + lane
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(1), ws=1))
    p.append(ldi(W_R3, 300))
    p.append(alu(OP_ADD, W_VPM, R1, R3))

    # C: row 2 = in[lane] (all lanes request), row 3 = result of a request where only even lanes write an address
    # (in[32 + lane])
    p.append(alu(OP_ADD, W_TMU0_S, RA, R2, raddr_a=1))
    p.append(nop(SIG_LOAD_TMU0))
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(2), ws=1))
    p.append(alu(OP_OR, W_VPM, R4, R4))
    p.append(ldi(W_R3, 128))
    p.append(alu(OP_ADD, W_R0, R2, R3))
    p.append(alu(OP_ADD, W_R0, RA, R0, raddr_a=1))  # r0 = in + 128 + lane * 4
    p.append(small_imm(OP_AND, W_NOP, R1, 1, sf=1))  # Z set for even lanes
    p.append(alu(OP_OR, W_TMU0_S, R0, R0, cond_add=COND_ZS))
    p.append(nop(SIG_LOAD_TMU0))
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(3), ws=1))
    p.append(alu(OP_OR, W_VPM, R4, R4))

    # Store: row 0 -> out[0..15], rows 2-3 -> out[32..63], 5 words of row 1 from column 3 -> out[64..]
    p.append(alu(OP_OR, W_NOP, RA, RA, raddr_a=A_MUTEX_ACQUIRE))
    p.append(ldi(W_VPM_SETUP, vdw_setup(1, 16, 0, 0), ws=1))
    p.append(ldi(W_VPM_SETUP, VDW_STRIDE_SETUP, ws=1))
    p.append(alu(OP_OR, W_VPM_ADDR, RA, RA, raddr_a=0, ws=1))
    p.append(alu(OP_OR, W_NOP, RB, RB, raddr_b=B_VPM_WAIT))
    p.append(ldi(W_VPM_SETUP, vdw_setup(2, 16, 2, 0), ws=1))
    p.append(ldi(W_VPM_SETUP, VDW_STRIDE_SETUP, ws=1))
    p.append(ldi(W_R3, 32 * 4))
    p.append(alu(OP_ADD, W_VPM_ADDR, RA, R3, raddr_a=0, ws=1))
    p.append(alu(OP_OR, W_NOP, RB, RB, raddr_b=B_VPM_WAIT))
    p.append(ldi(W_VPM_SETUP, vdw_setup(1, 5, 1, 3), ws=1))
    p.append(ldi(W_VPM_SETUP, VDW_STRIDE_SETUP, ws=1))
    p.append(ldi(W_R3, 64 * 4))
    p.append(alu(OP_ADD, W_VPM_ADDR, RA, R3, raddr_a=0, ws=1))
    p.append(alu(OP_OR, W_NOP, RB, RB, raddr_b=B_VPM_WAIT))
    p.append(small_imm(OP_OR, W_MUTEX_RELEASE, RB, 1))

    p += [nop(SIG_THREAD_END), nop(), nop()]
    return p


def program_test_d():
    """D: a TMU address write with all lanes masked, then waiting for the result. UNIFORMs: out address, in address."""
    p = []
    p.append(small_imm(OP_OR, W_TMU_NOSWAP, RB, 1))
    p.append(alu(OP_OR, 0, RA, RA, raddr_a=A_UNIFORM))  # ra0 = out
    p.append(alu(OP_OR, 1, RA, RA, raddr_a=A_UNIFORM))  # ra1 = in
    p.append(alu(OP_OR, W_R0, RA, RA, raddr_a=1))
    p.append(small_imm(OP_AND, W_NOP, R0, 0, sf=1))  # r0 & 0 = 0 -> Z set for all lanes
    p.append(alu(OP_OR, W_TMU0_S, R0, R0, cond_add=COND_ZC))  # Z clear: no lane writes
    p.append(nop(SIG_LOAD_TMU0))
    p.append(ldi(W_VPM_SETUP, vpm_write_setup(0), ws=1))
    p.append(alu(OP_OR, W_VPM, R4, R4))
    p.append(alu(OP_OR, W_NOP, RA, RA, raddr_a=A_MUTEX_ACQUIRE))
    p.append(ldi(W_VPM_SETUP, vdw_setup(1, 16, 0, 0), ws=1))
    p.append(ldi(W_VPM_SETUP, VDW_STRIDE_SETUP, ws=1))
    p.append(alu(OP_OR, W_VPM_ADDR, RA, RA, raddr_a=0, ws=1))
    p.append(alu(OP_OR, W_NOP, RB, RB, raddr_b=B_VPM_WAIT))
    p.append(small_imm(OP_OR, W_MUTEX_RELEASE, RB, 1))
    p += [nop(SIG_THREAD_END), nop(), nop()]
    return p


# --- vc4 DRM ioctls ---
def drm_iowr(nr, size):
    return (3 << 30) | (size << 16) | (ord('d') << 8) | (0x40 + nr)


IOCTL_CREATE_BO = drm_iowr(0x03, 16)
IOCTL_MMAP_BO = drm_iowr(0x04, 16)
IOCTL_SUBMIT_QPU = drm_iowr(0x20, 48)
IOCTL_WAIT_QPU = drm_iowr(0x21, 24)
IOCTL_QPU_BO_ADDR = drm_iowr(0x22, 8)
IOCTL_GEM_CLOSE = (1 << 30) | (8 << 16) | (ord('d') << 8) | 0x09

BO_SIZE = 64 * 1024
CODE, UNIFORMS, IN, OUT = 0, 4096, 8192, 16384
SENTINEL = 0x5E000000


def run(fd, words, timeout_ms=1000):
    bo = bytearray(struct.pack("IIII", BO_SIZE, 0, 0, 0))
    fcntl.ioctl(fd, IOCTL_CREATE_BO, bo)
    handle = struct.unpack("IIII", bo)[2]
    mm = bytearray(struct.pack("IIQ", handle, 0, 0))
    fcntl.ioctl(fd, IOCTL_MMAP_BO, mm)
    mem = mmap.mmap(fd, BO_SIZE, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=struct.unpack("IIQ", mm)[2])
    addr = bytearray(struct.pack("II", handle, 0))
    fcntl.ioctl(fd, IOCTL_QPU_BO_ADDR, addr)
    base = struct.unpack("II", addr)[1]

    mem[CODE:CODE + 8 * len(words)] = b"".join(struct.pack("II", lo, hi) for lo, hi in words)
    mem[UNIFORMS:UNIFORMS + 8] = struct.pack("II", base + OUT, base + IN)
    mem[IN:IN + 4 * 64] = struct.pack("64I", *[1000 + i for i in range(64)])
    mem[OUT:OUT + 4 * 128] = struct.pack("128I", *[SENTINEL + i for i in range(128)])

    handles = (ctypes.c_uint32 * 1)(handle)
    programs = (ctypes.c_uint32 * 2)(base + CODE, base + UNIFORMS)
    submit = bytearray(struct.pack("QQIIIIIIQ", ctypes.addressof(handles), ctypes.addressof(programs), 1, 1,
                                   timeout_ms, 0, 0, 0, 0))
    fcntl.ioctl(fd, IOCTL_SUBMIT_QPU, submit)
    seqno = struct.unpack("QQIIIIIIQ", submit)[8]
    wait = bytearray(struct.pack("QQiI", seqno, 0xFFFFFFFFFFFFFFFF, 0, 0))
    fcntl.ioctl(fd, IOCTL_WAIT_QPU, wait)
    status = struct.unpack("QQiI", wait)[2]
    out = list(struct.unpack("128I", mem[OUT:OUT + 4 * 128]))
    mem.close()
    fcntl.ioctl(fd, IOCTL_GEM_CLOSE, bytearray(struct.pack("II", handle, 0)))
    return status, out


def fmt(values):
    return " ".join("  ---" if v >= SENTINEL else f"{v:5d}" for v in values)


def main():
    if not validate_encoder():
        return 1
    fd = os.open("/dev/dri/renderD128", os.O_RDWR)
    lanes = " ".join(f"{i:5d}" for i in range(16))

    if "--test-d" in sys.argv:
        status, out = run(fd, program_test_d())
        print(f"D: TMU address write with all lanes masked: job status {status} "
              f"({'completed' if status == 0 else os.strerror(-status)})")
        if status == 0:
            print(f"   lane:  {lanes}\n   value: {fmt(out[0:16])}")
        return 0

    status, out = run(fd, program_main())
    print(f"Job status: {status} ({'completed' if status == 0 else os.strerror(-status)})")
    if status != 0:
        return 1
    print(f"lane:                         {lanes}")
    print(f"A: VPM row, even lanes masked: {fmt(out[0:16])}")
    print("   (expected: 200+lane on even lanes; odd lanes 100+lane if the condition masks VPM writes)")
    print(f"B: 5 words from column 3:      {fmt(out[64:80])}")
    print("   (expected: 303 304 305 306 307, then untouched sentinels ---)")
    print(f"C: TMU, all lanes:             {fmt(out[32:48])}")
    print(f"C: TMU, even lanes write addr: {fmt(out[48:64])}")
    print("   (even lanes: 1032+lane; odd lanes show what masked lanes read)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
