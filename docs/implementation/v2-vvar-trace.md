# V2 — [vvar] / [vdso] / page-table-pool EL0 hardening: trace + fix

Status: **fixed** (branch `work/v2-vvar-hardening`). This doc records the
end-to-end trace that resolved the model mismatch which caused the prior
attempt to be reverted, the hard evidence, the fix, and verification.

## The defect

The guest runs at EL0 but could **read and write** hl-internal state that
backs the low 2 MB block (VA `0x0..0x1FFFFF`, "block 0"):

- the stage-1 **page-table pool** (`PT_POOL_BASE 0x10000 .. PT_POOL_END
  0x100000`) — the guest could read hl's translation tables and, by writing
  page `0x10000` (the live L0 table), rewrite its own translation;
- the **[vvar]** time page (`0xE000`) — documented "RO to EL0" but writable,
  so the guest could forge its own vDSO clock;
- **[vdso]** code (`0xF000`) — writable, so the guest could rewrite it.

This is a **guest-integrity / defense-in-depth** gap, **not a host escape**:
HVF stage-2 still bounds the host (a crafted EL0 PTE produces a stage-2
permission fault). Severity is confinement completeness.

## Why the prior attempt was reverted (the model mismatch)

The prior attempt inspected block 0's L2 descriptor, saw `l2[0]=0x7e5`
(AP[2:1]=0b11 → **RO** at EL0+EL1, executable), set per-page perms on it, and
yet EL0 could *still* write [vvar]/pt-pool. The descriptor being RO but the
memory being writable "did not match", so the change was reverted as unsafe.

**Resolved:** the descriptor really is RO. The write succeeds because of the
**shim's W^X demand-toggle**, not because of a different mapping/TTBR.

## The resolved end-to-end model

### How block 0 is mapped

1. `guest_build_page_tables()` (src/guest.c) has no explicit region for block 0.
   The **shim-code region** is `SHIM_BASE=0x100000 .. +shim_bin_len` with
   `MEM_PERM_RX`. `ALIGN_2MB_DOWN(0x100000) == 0x0`, so this region's single
   2 MB block *is* block 0 → mapped **RX**, descriptor `0x7e5` (AP=RO + X).
   That one block covers holes, [vvar] (`0xE000`), [vdso] (`0xF000`), the
   pt-pool (`0x10000..0xFFFFF`), and shim code (`0x100000..0x1FFFFF`).

2. `hl.c` then calls `guest_invalidate_ptes(&g, 0, 0x1000)` (null guard). A
   *partial* invalidation of a 2 MB block forces a **split to 512×4 KB L3
   pages**. So at runtime **block 0 is already L3-split**: `l2[0]` is a *table*
   descriptor, page 0 is invalid, pages `0x1000..0x1FFFFF` are RX (AP=RO).

   (So the "split block 0 to L3" step the task feared was already shipped and
   proven — the fix only sets per-page perms on the existing L3 table.)

3. TTBR0 holds the L0 table **IPA**. Every stage-1 page-table walk reads
   descriptors by IPA, translated by **stage-2** — it never re-enters stage-1
   for the pt-pool VAs. So the stage-1 mapping of the pt-pool VA range is
   irrelevant to the MMU walker; only the guest (EL0) uses it.

### Why an EL0 write to a RO page succeeds — the W^X toggle

An EL0 store to a mapped **RO** page (RX block) raises a data abort
(EC=0x24), **DFSC = permission (0x0C)**, WnR=1. The shim's `handle_data_abort`
(src/shim.S) treats *any* write permission fault as a **JIT W^X demand-toggle**:
it calls `HVC #9` with type=1. The host handler (src/syscall_proc.c case 9)
**unconditionally**:

```
page_start = far & ~0xFFF;
guest_split_block(g, block_start);
guest_update_perms(g, page_start, page_end, MEM_PERM_RW);   // RO -> RW
```

The faulting 4 KB page is promoted to RW and the store is retried — it lands.
There is no check that the page is a legitimate JIT region. That is the whole
bug: RO on block-0 pages is defeated by the toggle on the first write.

Writing pt-pool page `0x10000` (the live L0 table) promotes it to RW and the
guest's store corrupts the L0 table → translation collapse → hang.

## Hard evidence (pre-fix, aarch64 static probe under hl)

`AT_SYSINFO_EHDR = 0xf000` (== VDSO_BASE), so `[vvar]=0xe000`, `pt-pool=0x10000`.

| access            | pre-fix         | mechanism                                   |
|-------------------|-----------------|---------------------------------------------|
| [vvar]   read     | ok              | RX block → EL0 read allowed                 |
| **[vvar] write**  | **ok** (forged) | perm fault → W^X toggle → RW → store lands   |
| pt-pool  read     | ok (leak)       | RX block → EL0 read allowed                 |
| **pt-pool write** | **hang**        | toggle promotes L0 table → guest corrupts it |
| [vdso]   write    | ok              | perm fault → W^X toggle → RW → store lands   |

## The fix (two parts)

### Part A — shim rejects block-0 permission faults (src/shim.S)

Nothing in block 0 legitimately needs a W^X toggle (JIT code lives in the
mmap RX region ≥`0x10000000` and the kbuf). In both `handle_data_abort` and
`handle_inst_abort`, a permission fault with `FAR_EL1 < HL_LOW_PROTECT_END`
(`0x200000` = `SHIM_DATA_BASE`, i.e. within block 0) branches to
`handle_el0_fault` (→ `HVC #11` → **SIGSEGV**) instead of the toggle. This
turns EL0 writes/execs to [vvar]/[vdso]/pt-pool/shim into faults.

### Part B — per-page stage-1 perms (src/vdso.c `vdso_harden_low_block`)

Called after the null-guard split at startup (`hl.c`) and execve re-setup
(`syscall_exec.c`):

- **pt-pool** `0x10000..0x100000`: `guest_invalidate_ptes` → stage-1 invalid.
  EL0 read/write now **translation-fault** → SIGSEGV (no toggle, since it is
  not a permission fault). The MMU still walks the tables by IPA via stage-2,
  so translation is undisturbed. This closes the read leak too.
- **holes** `0x1000..0xE000`: invalidated.
- **[vvar]**: `guest_update_perms(..., MEM_PERM_R)` → EL0 **read-only +
  execute-never**. The host publisher still writes it via `host_base`
  (bypasses stage-1). Writes fault → Part A → SIGSEGV.
- **[vdso]**: left **RX** (guest reads + executes it). Writes fault → Part A.

Fork inherits the hardened tables (COW child shares parent memory; the legacy
IPC path copies the pt-pool), so no fork-side change is needed.

## Post-fix evidence (same probe / the regression test)

| access            | post-fix  | fault syndrome (from hl fault reporter)      |
|-------------------|-----------|----------------------------------------------|
| [vvar]   read     | ok        | (still readable — vDSO clock fast path works)|
| [vvar]   write    | SIGSEGV   | `FSC=0xf` permission fault (RO) → Part A      |
| pt-pool  read     | SIGSEGV   | `FSC=0x7` translation fault (invalid) → Part B|
| pt-pool  write    | SIGSEGV   | `FSC=0x7` translation fault (invalid) → Part B|
| [vdso]   write    | SIGSEGV   | `FSC=0xf` permission fault (RX) → Part A      |

Regression test: `test/test-vvar-protect.c` (4 assertions), wired into
`make test-all`. Positive control asserts [vvar] stays readable; the other
three assert the writes/reads fault. Mutation-verified: reverting the
hardening makes the fault assertions FAIL (the writes/reads succeed again).

## Verification

- `make test-both-modes`: **66/66 → 67/67** in both fs modes (legacy +
  rooted) with the new test; `test-vdso-time` / `test-vdso-fork` stay green
  (the vDSO clock still reads [vvar]); host `test-diagnostics.sh` 10/10.
- Not run here: the x86_64/rosetta lane (`hl-x64`). Block 0 is identical
  under rosetta (shim/vdso/vvar/pt-pool at the same low VAs; rosetta's own
  code/JIT is at ≥`0x400000` / 128 TB / kbuf, all above the `0x200000`
  guard), so the change is expected to be inert there, but this is unverified.
