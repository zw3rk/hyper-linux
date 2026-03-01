/* vdso.c — Minimal vDSO (Virtual Dynamic Shared Object) for hl
 *
 * Copyright 2025 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Builds a minimal vDSO ELF image in guest memory. The vDSO is required
 * by Rosetta's JIT translator, which parses it to locate four kernel
 * helper functions via the SHT_DYNSYM section:
 *
 *   __kernel_rt_sigreturn    (NR 139)
 *   __kernel_clock_getres    (NR 114)
 *   __kernel_clock_gettime   (NR 113)
 *   __kernel_gettimeofday    (NR 169)
 *
 * Each function is a simple ARM64 trampoline: mov x8, #NR; svc #0; ret.
 * The image is mapped into guest memory at VDSO_BASE and its address is
 * provided via AT_SYSINFO_EHDR in the auxiliary vector.
 *
 * CRITICAL: Rosetta requires DT_HASH in the vDSO's dynamic section for
 * symbol resolution (Vdso.cpp:78). Without it, rosetta aborts with:
 *   "assertion failed [hash_table != nullptr]: Failed to find vdso DT_HASH"
 * We provide PT_DYNAMIC with DT_HASH, DT_SYMTAB, DT_STRTAB entries, and
 * a minimal ELF hash table (.hash section) with 1 bucket.
 *
 * The vDSO is built as a position-independent ET_DYN with p_vaddr=0.
 * Rosetta computes load_offset = AT_SYSINFO_EHDR - p_vaddr, then
 * accesses sections via load_offset + sh_offset. All internal addresses
 * (sh_addr, st_value, e_entry) are relative to 0; rosetta adds
 * load_offset (== VDSO_BASE) to get the actual guest virtual addresses.
 *
 * Layout of the 4KB vDSO page (file offsets from 0):
 *
 *   Offset  Content              Size
 *   0x000   ELF64 header         64 bytes
 *   0x040   Phdr[0] (PT_LOAD)    56 bytes
 *   0x078   Phdr[1] (PT_DYNAMIC) 56 bytes
 *   0x0B0   .text trampolines    48 bytes (4 × 12)
 *   0x0E0   .dynstr strings      90 bytes
 *   0x13C   .dynsym entries      120 bytes (NULL + 4 symbols)
 *   0x1B4   .hash (ELF hash)     32 bytes (1 bucket, 5 chains)
 *   0x1D8   .dynamic             96 bytes (6 entries × 16)
 *   0x238   Shdr[0..5]           384 bytes (6 × 64)
 *   0x3B8   end
 */
#include "vdso.h"
#include "elf.h"

#include <string.h>
#include <stdio.h>

/* ---------- ELF section header (not in our elf.h) ---------- */

typedef struct {
    uint32_t sh_name;       /* Section name (index into shstrtab) */
    uint32_t sh_type;       /* Section type */
    uint64_t sh_flags;      /* Section flags */
    uint64_t sh_addr;       /* Section virtual address (relative to 0) */
    uint64_t sh_offset;     /* Section file offset */
    uint64_t sh_size;       /* Section size in bytes */
    uint32_t sh_link;       /* Link to another section */
    uint32_t sh_info;       /* Additional section info */
    uint64_t sh_addralign;  /* Section alignment */
    uint64_t sh_entsize;    /* Entry size if section holds table */
} elf64_shdr_t;

/* ---------- ELF dynamic section entry ---------- */

typedef struct {
    int64_t  d_tag;         /* Dynamic entry type */
    uint64_t d_val;         /* Value (address or size) */
} elf64_dyn_t;

/* ELF section types */
#define SHT_NULL     0
#define SHT_STRTAB   3
#define SHT_HASH     5
#define SHT_DYNAMIC  6
#define SHT_DYNSYM  11

/* ELF section flags */
#define SHF_ALLOC     (1ULL << 1)
#define SHF_EXECINSTR (1ULL << 2)

/* ELF dynamic tags */
#define DT_NULL     0
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ   10
#define DT_SYMENT  11

/* ---------- ELF symbol table entry ---------- */

typedef struct {
    uint32_t st_name;       /* Symbol name (index into strtab) */
    uint8_t  st_info;       /* Symbol type and binding */
    uint8_t  st_other;      /* Symbol visibility */
    uint16_t st_shndx;      /* Section index */
    uint64_t st_value;      /* Symbol value (relative to 0) */
    uint64_t st_size;       /* Symbol size */
} elf64_sym_t;

/* Symbol binding/type macros */
#define STB_GLOBAL   1
#define STT_FUNC     2
#define ELF_ST_INFO(bind, type)  (((bind) << 4) | ((type) & 0xf))

/* ---------- vDSO image layout offsets ---------- */

#define VDSO_OFF_EHDR    0x000
#define VDSO_OFF_PHDR    0x040   /* = sizeof(elf64_ehdr_t) */
#define VDSO_OFF_PHDR1   0x078   /* = PHDR + sizeof(elf64_phdr_t) */
#define VDSO_OFF_TEXT    0x0B0   /* = PHDR1 + sizeof(elf64_phdr_t) */
#define VDSO_OFF_DYNSTR  0x0E0   /* after .text (4 × 12 = 48 bytes) */
/* DYNSTR_SIZE = 90 bytes → ends at 0x13A, round up to 4-byte align: 0x13C */
#define VDSO_OFF_DYNSYM  0x13C   /* after .dynstr (92 bytes padded) */
/* DYNSYM = 5 × 24 = 120 bytes → ends at 0x1B4 (already 4-byte aligned) */
#define VDSO_OFF_HASH    0x1B4   /* after .dynsym */
/* HASH = 4+4+4+20 = 32 bytes → ends at 0x1D4, round up to 8-byte: 0x1D8 */
#define VDSO_OFF_DYNAMIC 0x1D8   /* after .hash */
/* DYNAMIC = 6 × 16 = 96 bytes → ends at 0x238 */
#define VDSO_OFF_SHDR    0x238   /* after .dynamic */
/* SHDR = 6 × 64 = 384 bytes → ends at 0x3B8 */

/* Number of vDSO symbols (excluding the mandatory NULL entry) */
#define VDSO_NUM_SYMS 4

/* Number of ELF hash table entries: NULL symbol + 4 named symbols */
#define HASH_NCHAIN (VDSO_NUM_SYMS + 1)  /* 5 */
/* Single-bucket hash table: all symbols chain through bucket 0 */
#define HASH_NBUCKET 1
/* Hash table size: 2 words (nbucket, nchain) + nbucket + nchain */
#define HASH_SIZE ((2 + HASH_NBUCKET + HASH_NCHAIN) * sizeof(uint32_t))

/* .text trampolines: each is 3 ARM64 instructions (12 bytes).
 * The trampolines are simple syscall wrappers that rosetta uses
 * instead of the original Linux vDSO implementations.
 *
 * Order matches the symbol table entries below:
 *   [0] __kernel_rt_sigreturn:  mov x8, #139; svc #0; ret
 *   [1] __kernel_clock_getres:  mov x8, #114; svc #0; ret
 *   [2] __kernel_clock_gettime: mov x8, #113; svc #0; ret
 *   [3] __kernel_gettimeofday:  mov x8, #169; svc #0; ret */
static const uint32_t trampoline_code[VDSO_NUM_SYMS][3] = {
    { 0xD2801168, 0xD4000001, 0xD65F03C0 },  /* NR 139: rt_sigreturn */
    { 0xD2800E48, 0xD4000001, 0xD65F03C0 },  /* NR 114: clock_getres */
    { 0xD2800E28, 0xD4000001, 0xD65F03C0 },  /* NR 113: clock_gettime */
    { 0xD2801528, 0xD4000001, 0xD65F03C0 },  /* NR 169: gettimeofday */
};
#define TRAMPOLINE_SIZE 12  /* bytes per trampoline */

/* .dynstr: concatenated NUL-terminated strings.
 *   offset 0:  "" (mandatory empty string)
 *   offset 1:  "__kernel_rt_sigreturn"
 *   offset 23: "__kernel_clock_getres"
 *   offset 45: "__kernel_clock_gettime"
 *   offset 68: "__kernel_gettimeofday" */
static const char dynstr_data[] =
    "\0__kernel_rt_sigreturn"      /*  0: 1 + 21 = 22, next at 23 */
    "\0__kernel_clock_getres"      /* 23: 1 + 21 = 22, next at 45 */
    "\0__kernel_clock_gettime"     /* 45: 1 + 22 = 23, next at 68 */
    "\0__kernel_gettimeofday";     /* 68: 1 + 21 = 22, total = 90 */
#define DYNSTR_SIZE (sizeof(dynstr_data))  /* 90 bytes */

/* Symbol name offsets in dynstr (index of first char after each NUL) */
static const uint32_t sym_name_offsets[VDSO_NUM_SYMS] = {
    1,   /* __kernel_rt_sigreturn */
    23,  /* __kernel_clock_getres */
    45,  /* __kernel_clock_gettime */
    68,  /* __kernel_gettimeofday */
};

/* ---------- ELF hash function (SysV / DT_HASH) ---------- */

static uint32_t elf_hash(const char *name) {
    uint32_t h = 0, g;
    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xf0000000;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uint64_t vdso_build(guest_t *g) {
    /* Get host pointer to the vDSO page */
    uint8_t *page = (uint8_t *)guest_ptr(g, VDSO_BASE);
    if (!page) {
        fprintf(stderr, "vdso: VDSO_BASE 0x%llx out of guest memory\n",
                (unsigned long long)VDSO_BASE);
        return 0;
    }

    /* Zero the entire 4KB page */
    memset(page, 0, VDSO_SIZE);

    /* ---- ELF header ---- */
    elf64_ehdr_t *ehdr = (elf64_ehdr_t *)(page + VDSO_OFF_EHDR);
    ehdr->e_ident[0] = ELFMAG0;
    ehdr->e_ident[1] = ELFMAG1;
    ehdr->e_ident[2] = ELFMAG2;
    ehdr->e_ident[3] = ELFMAG3;
    ehdr->e_ident[EI_CLASS] = ELFCLASS64;
    ehdr->e_ident[EI_DATA]  = ELFDATA2LSB;
    ehdr->e_ident[6] = 1;  /* EV_CURRENT */
    ehdr->e_type      = ET_DYN;      /* Shared object (vDSO convention) */
    ehdr->e_machine   = EM_AARCH64;
    ehdr->e_version   = 1;           /* EV_CURRENT */
    ehdr->e_entry     = VDSO_OFF_TEXT;  /* Relative to load base (0) */
    ehdr->e_phoff     = VDSO_OFF_PHDR;
    ehdr->e_shoff     = VDSO_OFF_SHDR;
    ehdr->e_flags     = 0;
    ehdr->e_ehsize    = sizeof(elf64_ehdr_t);
    ehdr->e_phentsize = sizeof(elf64_phdr_t);
    ehdr->e_phnum     = 2;           /* PT_LOAD + PT_DYNAMIC */
    ehdr->e_shentsize = sizeof(elf64_shdr_t);
    ehdr->e_shnum     = 6;           /* NULL + .text + .dynstr + .dynsym + .hash + .dynamic */
    ehdr->e_shstrndx  = 2;           /* .dynstr doubles as shstrtab */

    /* ---- Phdr[0]: PT_LOAD covering the whole page ----
     * p_vaddr = 0: standard position-independent convention for ET_DYN.
     * Rosetta computes load_offset = AT_SYSINFO_EHDR - p_vaddr = VDSO_BASE.
     * All file offsets are then resolved as load_offset + offset. */
    elf64_phdr_t *phdr0 = (elf64_phdr_t *)(page + VDSO_OFF_PHDR);
    phdr0->p_type   = PT_LOAD;
    phdr0->p_flags  = PF_R | PF_X;
    phdr0->p_offset = 0;
    phdr0->p_vaddr  = 0;              /* Position-independent: base = 0 */
    phdr0->p_paddr  = 0;
    phdr0->p_filesz = VDSO_SIZE;
    phdr0->p_memsz  = VDSO_SIZE;
    phdr0->p_align  = 0x1000;         /* Page-aligned */

    /* ---- Phdr[1]: PT_DYNAMIC pointing to .dynamic section ----
     * Rosetta locates .dynamic via this phdr, then walks DT entries to find
     * DT_HASH for symbol resolution during vDSO initialization. */
    elf64_phdr_t *phdr1 = (elf64_phdr_t *)(page + VDSO_OFF_PHDR1);
    phdr1->p_type   = PT_DYNAMIC;
    phdr1->p_flags  = PF_R;
    phdr1->p_offset = VDSO_OFF_DYNAMIC;
    phdr1->p_vaddr  = VDSO_OFF_DYNAMIC;   /* Relative to load base (0) */
    phdr1->p_paddr  = VDSO_OFF_DYNAMIC;
    phdr1->p_filesz = 6 * sizeof(elf64_dyn_t);  /* 6 entries */
    phdr1->p_memsz  = 6 * sizeof(elf64_dyn_t);
    phdr1->p_align  = 8;

    /* ---- .text: syscall trampolines ---- */
    memcpy(page + VDSO_OFF_TEXT, trampoline_code, sizeof(trampoline_code));

    /* ---- .dynstr: string table ---- */
    memcpy(page + VDSO_OFF_DYNSTR, dynstr_data, DYNSTR_SIZE);

    /* ---- .dynsym: symbol table ---- */
    elf64_sym_t *sym = (elf64_sym_t *)(page + VDSO_OFF_DYNSYM);

    /* sym[0]: mandatory NULL entry */
    memset(&sym[0], 0, sizeof(elf64_sym_t));

    /* sym[1..4]: kernel helper function symbols.
     * st_value is relative to 0 (position-independent); rosetta adds
     * load_offset (VDSO_BASE) to get the actual guest virtual address. */
    for (int i = 0; i < VDSO_NUM_SYMS; i++) {
        sym[i + 1].st_name  = sym_name_offsets[i];
        sym[i + 1].st_info  = ELF_ST_INFO(STB_GLOBAL, STT_FUNC);
        sym[i + 1].st_other = 0;
        sym[i + 1].st_shndx = 1;  /* .text section index */
        sym[i + 1].st_value = VDSO_OFF_TEXT +
                               (uint64_t)i * TRAMPOLINE_SIZE;
        sym[i + 1].st_size  = TRAMPOLINE_SIZE;
    }

    /* ---- .hash: ELF hash table for symbol lookup ----
     * Rosetta parses the vDSO's PT_DYNAMIC looking for DT_HASH and asserts
     * it's non-NULL (Vdso.cpp:78). The hash table must be a valid SysV ELF
     * hash table (not GNU hash).
     *
     * Layout: { nbucket, nchain, bucket[nbucket], chain[nchain] }
     * With 1 bucket, all symbols chain through bucket[0].
     * chain[i] = next symbol index with same hash, or STN_UNDEF (0). */
    uint32_t *hash = (uint32_t *)(page + VDSO_OFF_HASH);
    hash[0] = HASH_NBUCKET;     /* nbucket = 1 */
    hash[1] = HASH_NCHAIN;      /* nchain = 5 (NULL + 4 symbols) */

    /* bucket[0]: first symbol in the single chain.
     * Build chain by computing hash of each symbol name (mod nbucket = 0
     * for all since nbucket=1) and linking them. */
    hash[2] = 0;  /* bucket[0]: will be overwritten below */

    /* chain[0..4]: chain[i] = next symbol index with same bucket, or 0 */
    uint32_t *chain = &hash[2 + HASH_NBUCKET];  /* chain starts after bucket[] */
    memset(chain, 0, HASH_NCHAIN * sizeof(uint32_t));

    /* With 1 bucket, all symbols go in bucket[0]. Build a linked list.
     * chain[0] = 0 (NULL symbol has no "next" in bucket sense, but bucket[0]
     * starts with the first real symbol). Walk symbols 1..4 and link them. */
    uint32_t first_sym = 0;  /* No symbols yet in bucket 0 */
    for (int i = VDSO_NUM_SYMS; i >= 1; i--) {
        /* Each symbol hashes to bucket[elf_hash(name) % 1] = bucket[0].
         * Prepend to chain: chain[i] = old head, bucket[0] = i. */
        const char *name = dynstr_data + sym_name_offsets[i - 1];
        (void)elf_hash(name);  /* Verify hash works; all go to bucket 0 */
        chain[i] = first_sym;
        first_sym = (uint32_t)i;
    }
    hash[2] = first_sym;  /* bucket[0] = head of chain */

    /* ---- .dynamic: dynamic section entries ----
     * Rosetta walks PT_DYNAMIC entries to locate DT_HASH, DT_SYMTAB, and
     * DT_STRTAB. All addresses are relative to load base 0; rosetta adds
     * the load_offset (VDSO_BASE) to resolve them. */
    elf64_dyn_t *dyn = (elf64_dyn_t *)(page + VDSO_OFF_DYNAMIC);
    dyn[0].d_tag = DT_HASH;    dyn[0].d_val = VDSO_OFF_HASH;
    dyn[1].d_tag = DT_SYMTAB;  dyn[1].d_val = VDSO_OFF_DYNSYM;
    dyn[2].d_tag = DT_STRTAB;  dyn[2].d_val = VDSO_OFF_DYNSTR;
    dyn[3].d_tag = DT_STRSZ;   dyn[3].d_val = DYNSTR_SIZE;
    dyn[4].d_tag = DT_SYMENT;  dyn[4].d_val = sizeof(elf64_sym_t);
    dyn[5].d_tag = DT_NULL;    dyn[5].d_val = 0;

    /* ---- Section header table ---- */
    elf64_shdr_t *shdr = (elf64_shdr_t *)(page + VDSO_OFF_SHDR);

    /* shdr[0]: SHN_UNDEF (required NULL entry) */
    memset(&shdr[0], 0, sizeof(elf64_shdr_t));

    /* shdr[1]: .text */
    shdr[1].sh_name      = 0;
    shdr[1].sh_type      = 1;        /* SHT_PROGBITS */
    shdr[1].sh_flags     = SHF_ALLOC | SHF_EXECINSTR;
    shdr[1].sh_addr      = VDSO_OFF_TEXT;    /* Relative to 0 */
    shdr[1].sh_offset    = VDSO_OFF_TEXT;
    shdr[1].sh_size      = sizeof(trampoline_code);
    shdr[1].sh_link      = 0;
    shdr[1].sh_info      = 0;
    shdr[1].sh_addralign = 4;
    shdr[1].sh_entsize   = 0;

    /* shdr[2]: .dynstr (SHT_STRTAB) — also serves as shstrtab */
    shdr[2].sh_name      = 0;
    shdr[2].sh_type      = SHT_STRTAB;
    shdr[2].sh_flags     = SHF_ALLOC;
    shdr[2].sh_addr      = VDSO_OFF_DYNSTR;  /* Relative to 0 */
    shdr[2].sh_offset    = VDSO_OFF_DYNSTR;
    shdr[2].sh_size      = DYNSTR_SIZE;
    shdr[2].sh_link      = 0;
    shdr[2].sh_info      = 0;
    shdr[2].sh_addralign = 1;
    shdr[2].sh_entsize   = 0;

    /* shdr[3]: .dynsym (SHT_DYNSYM) — linked to .dynstr (section 2) */
    shdr[3].sh_name      = 0;
    shdr[3].sh_type      = SHT_DYNSYM;
    shdr[3].sh_flags     = SHF_ALLOC;
    shdr[3].sh_addr      = VDSO_OFF_DYNSYM;  /* Relative to 0 */
    shdr[3].sh_offset    = VDSO_OFF_DYNSYM;
    shdr[3].sh_size      = (VDSO_NUM_SYMS + 1) * sizeof(elf64_sym_t);
    shdr[3].sh_link      = 2;        /* Index of associated .dynstr */
    shdr[3].sh_info      = 1;        /* First non-local symbol index */
    shdr[3].sh_addralign = 8;
    shdr[3].sh_entsize   = sizeof(elf64_sym_t);

    /* shdr[4]: .hash (SHT_HASH) — linked to .dynsym (section 3) */
    shdr[4].sh_name      = 0;
    shdr[4].sh_type      = SHT_HASH;
    shdr[4].sh_flags     = SHF_ALLOC;
    shdr[4].sh_addr      = VDSO_OFF_HASH;
    shdr[4].sh_offset    = VDSO_OFF_HASH;
    shdr[4].sh_size      = HASH_SIZE;
    shdr[4].sh_link      = 3;        /* Index of associated .dynsym */
    shdr[4].sh_info      = 0;
    shdr[4].sh_addralign = 4;
    shdr[4].sh_entsize   = 4;

    /* shdr[5]: .dynamic (SHT_DYNAMIC) — linked to .dynstr (section 2) */
    shdr[5].sh_name      = 0;
    shdr[5].sh_type      = SHT_DYNAMIC;
    shdr[5].sh_flags     = SHF_ALLOC;
    shdr[5].sh_addr      = VDSO_OFF_DYNAMIC;
    shdr[5].sh_offset    = VDSO_OFF_DYNAMIC;
    shdr[5].sh_size      = 6 * sizeof(elf64_dyn_t);
    shdr[5].sh_link      = 2;        /* Index of associated .dynstr */
    shdr[5].sh_info      = 0;
    shdr[5].sh_addralign = 8;
    shdr[5].sh_entsize   = sizeof(elf64_dyn_t);

    return VDSO_BASE;
}
