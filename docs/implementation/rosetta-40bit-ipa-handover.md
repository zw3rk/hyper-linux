# Rosetta on macOS 26 (40-bit HVF IPA) — investigation handover

> ## RESOLVED — the fault was a POISONED AOT CACHE, not IPA width.
> The `0x8af44c` (and varied-ELR) faults were caused by a corrupted
> `~/.cache/hl-rosettad/<sha>.aot`, NOT by the 40-bit IPA cap. Proof: the SAME hl
> binary runs x86_64 fine on M4 `aarch64-darwin-6.lan` (also 40-bit), and on this
> M5 after `rm -rf ~/.cache/hl-rosettad ~/.cache/rosetta`. Rosetta also runs fine
> under Lima/VZ here at ~33-bit IPA. So **Rosetta does NOT need 48-bit IPA**; the
> whole "48-bit" thread below was a confound (48-bit era = healthy cache; 40-bit
> era = poisoned cache). **Fixed** in `d247ded`: hl now validates the AOT cache
> (structural header + content SHA256 sidecar), never serves a corrupt `.aot`, and
> self-heals by re-translating. TIER 1 (negotiate IPA to the 40-bit host max) and
> the mprotect VA-keying fix remain valid, independent improvements. The rest of
> this doc is kept for the record but its premise is superseded.


## Goal
Make x86_64/Rosetta work when HVF caps the VM IPA at **40-bit** (macOS 26 on this
M5 rejects `hv_vm_config_set_ipa_size(>40)` with HV_UNSUPPORTED). See the memory
note `hvf-ipa-40bit-macos26.md`. Branch: `feat/rosetta-low-ipa` (off `master`).

## What shipped (committed, correct, keep)
1. **TIER 1 — IPA negotiation** (`79ff27a`): `guest_pick_ipa()` caps the requested
   IPA to the host max; `guest_create_vm_ipa()` checks `set_ipa_size` + reads back
   `get_ipa_size` and fails loudly instead of silently creating a 36-bit VM; an
   explicit `is_rosetta` flag gates the file-backed COW path (capping 48→40 would
   otherwise `ftruncate` a 1TB temp file); overflow rejects past `2^ipa_bits`.
   Effect: the VM now creates at a real **40-bit** width, the 1TB primary maps,
   and Rosetta **loads and runs its setup** (was: cryptic `hv_vm_map` failure).
2. **High-VA mprotect VA-keying** (`af13911`): the high-VA `mprotect` path resolved
   VA→GPA and passed the GPA to `guest_split_block`/`guest_update_perms`, which key
   on VA — so it edited the low identity alias and left the high-VA page unchanged.
   Now keyed on the VA. (Real correctness bug; NOT this fault's cause.)

Both verified not to regress the aarch64 suite; native aarch64 runs fine at 40-bit.

## The remaining fault (unresolved)
Rosetta gets through init (VZ_CHECK/CAPS, /proc/maps, high-VA mmaps, mprotect,
installs SIGSEGV/SIGTRAP handlers) then its JIT-translated code faults:
```
ELR=0x8af44c  insn f8408496 = LDR x22, [x4], #16 (post-index)  X4=0  → data abort at VA 0
```
right after a `BL 0x8b3208` (Rosetta's runtime context-switch stub: saves x86 state
to `[X18+off]`, `str x30,[x18,#8]`, re-tags X18, branches into the runtime). X4 (an
x86 reg) should be a valid pointer but is 0. hl DOES deliver SIGSEGV to Rosetta's
handler (deliver=1); Rosetta's handler inspects it, decides it's a genuine segfault,
resets SIGSEGV to SIG_DFL and re-raises → process dies rc=139. The pre-existing
"JIT gate" `[X18+0x1b8]` reads 0 (retranslation disabled) at that point.

## What was ruled out (with evidence)
- **Memory layout**: for a non-overflow workload (`true`) the layout is byte-identical
  at 40-bit and 48-bit (both: 1TB primary, `mmap_high_va` packs high VAs to low IPA).
- **Overflow / IPA headroom**: `true` uses no overflow; shrinking the primary to 512GB
  (leaving `[512GB,1TB)` headroom) does NOT change the fault.
- **TCR.IPS**: hardcoded 48-bit; native aarch64 works at 40-bit with the same TCR.
- **ID_AA64MMFR0_EL1 / PARange**: hl returns a FIXED VZ-spoofed value (not VM-width
  derived), so the guest sees the same PARange at 40 and 48-bit.
- **The mprotect bug (2)**: fixed; fault unchanged.
- The fault is **byte-identical** whether the VM is 36-bit (pre-TIER-1 fallback, 64GB)
  or 40-bit (post-TIER-1, 1TB) — different layouts, same fault. It correlates **purely
  with VM IPA width < 48**.

## The core finding — it's a FIXABLE hl bug, not a Rosetta/OS requirement
**Rosetta runs fine at LOW IPA under Apple's own Virtualization.framework on THIS exact
M5/macOS 26.5.2.** Proven by starting the Lima `docker` VZ VM (rosetta.enabled=true, 4GB
RAM ⇒ ~33-bit IPA) and running the SAME x86_64 static musl binaries that fault under hl:
`hello-musl` → "Hello from musl!", coreutils `echo` → OK, both rc=0. VZ gives Rosetta a
real Linux guest kernel that maps its high VAs to low guest-physical RAM, so the VM's
stage-2 IPA only spans a few GB. **Therefore Rosetta does NOT need 48-bit IPA; hl's
failure is an hl bug.**

The bug is subtle. For a non-overflow workload the hl guest state is IDENTICAL between a
48-bit and 40-bit VM — same 1TB primary, same page tables, same TCR, same placement; the
ONLY effective difference is the HVF-configured stage-2 IPA width (and `g->ipa_bits`, which
doesn't affect `true`'s execution). Yet hl-Rosetta works ONLY at a 48-bit VM (xmms ran ~5h
earlier when set_ipa_size(48) still succeeded) and faults at 36/40-bit — while VZ works at
~33-bit. So it is NOT "wider IPA is better"; hl has something that is correct only when the
HVF VM width is exactly 48. **Also tested and ruled out: TCR.IPS 48→40 (matching the VM
width, like VZ does) does NOT fix it.** The mechanism (some hl page-table/memory-emulation
behavior that a real guest kernel gets right but hl only gets right at a 48-bit stage-2) is
still unpinned.

Blocked from pinning it purely by static analysis: (a) macOS 26 won't grant a 48-bit VM
here, so there's no working hl baseline to A/B; (b) the failing code is JIT-generated (in
no file); (c) the gpt-5.6-sol advisor consult on the JIT internals was cut off by its
provider's content filter. The VZ run is the working baseline to diff against.

## Recommended next steps
1. **Reboot test (decisive, cheap, user-run):** reboot, then run `scratchpad/hvfprobe`
   BEFORE launching any UTM/Lima VM. If `set_ipa_size(48)` succeeds (48-bit restored),
   Rosetta works with TIER 1 already in place — confirming the 40-bit cap is a runtime
   state reduction, not a fixed macOS-26 limit, and that Rosetta needs the wide VM.
2. **If 48-bit stays unavailable / for a durable 40-bit fix:** single-step the JIT via
   the GDB stub (temporarily lift the rosetta exclusion at `hl.c` GDB setup),
   `hbreak *0x8af44c`, and walk backward through Rosetta's runtime to find the load
   that feeds X4 and why it yields 0 under a <48-bit VM. Then determine whether hl can
   satisfy it at 40-bit. (The stub is documented aarch64-only; the guest runs ARM64
   even under Rosetta, so it may work — untested for Rosetta.)

## Repro
```
hl=_build/hl ; X=<x64-coreutils>/bin
$hl "$X/true"    # → EL0 data fault at 0x0 ELR=0x8af44c, rc=139
```
Diagnostic scaffolding (instruction-window + register-probe dumps in the HVC #11 fault
handler) was used and reverted; re-add from git history of this session if needed.
