#include <Hypervisor/Hypervisor.h>
#include <Hypervisor/hv_vcpu.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Diagnostics
#define ASSERT_HV(r) assert((hv_return_t)(r) == HV_SUCCESS)

#define GUEST_SIZE 0x1000000
#define GUEST_ADDR 0x80000000ULL

static void load_bin(const char *path, void *dest) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    struct stat st;
    assert(fstat(fileno(f), &st) == 0);
    size_t size = st.st_size;
    assert(fread(dest, 1, size, f) == size);
    fclose(f);
}

// Add the dump function
 static void dump_guest_memory(void *guest_mem, uint64_t guest_addr, uint64_t dump_size) {
     uint64_t start_offset = (guest_addr - GUEST_ADDR) > (dump_size / 2) ? (guest_addr - GUEST_ADDR) - (dump_size / 2) : 0;
     uint8_t *mem_ptr = (uint8_t *)guest_mem + start_offset;

     printf("   Memory Dump around 0x%llx:\n", guest_addr);
     printf("     Address        | 00 01 02 03 04 05 06 07 | 08 09 0A 0B 0C 0D 0E 0F\n");
     printf("     ------------------------------------------------------------------\n");

     for (int i = 0; i < dump_size; i += 16) {
         printf("     0x%012llx | ", GUEST_ADDR + start_offset + i);
         for (int j = 0; j < 16; j++) {
             if (i + j < dump_size) {
                 printf("%02x ", mem_ptr[i + j]);
             } else {
                 printf("   ");
             }
             if (j == 7) printf("| ");
         }
         printf("\n");
     }
 }

int main() {
    // Use posix_memalign for aligned allocation to ensure compatibility
    void *guest_mem;
    assert(posix_memalign(&guest_mem, getpagesize(), GUEST_SIZE) == 0);

    // Page tables (4KB granule, 48-bit VA, L0 at 0x80001000, L1 at 0x80002000)
    uint64_t *l0 = (uint64_t *)((uintptr_t)guest_mem + 0x1000);
    uint64_t *l1 = (uint64_t *)((uintptr_t)guest_mem + 0x2000);
    // Allocate space for our new L2 table
    uint64_t *l2 = (uint64_t *)((uintptr_t)guest_mem + 0x3000);

    l0[0] = 0x80002000ULL | 0b11; // L0[0] -> L1 table

    // L1[0]: 1GB block 0x0-0x3fffffff, device, Attr0, UXN=1, PXN=1, AF=1, SH=0b10, AP=0b00, NS=0
    l1[0] = (0x0ULL & 0xFFFFFFC0000000ULL) | (1ULL << 54) /*UXN*/ | (1ULL << 53) /*PXN*/ | (1ULL << 10) /*AF*/ | (0b10ULL << 8) /*SH*/ | (0b00ULL << 6) /*AP*/ | (1ULL << 5) /*NS*/ | (0ULL << 2) /*Attr0*/ | 0b01ULL;

    // L1[1]: 1GB block 0x40000000-0x7fffffff, normal, Attr1, UXN=0, PXN=0, AF=1, SH=0b11, AP=0b00, NS=0
    l1[1] = (0x40000000ULL & 0xFFFFFFC0000000ULL) | (0ULL << 54) /*UXN*/ | (0ULL << 53) /*PXN*/ | (1ULL << 10) /*AF*/ | (0b11ULL << 8) /*SH*/ | (0b11ULL << 6) /*AP*/ | (1ULL << 5) /*NS*/ | (1ULL << 2) /*Attr1*/ | 0b01ULL;

    // // Add L1[2] for our guest: 0x80000000-0xbfffffff, normal, Attr1, UXN=0, PXN=0, AF=1, SH=0b11, AP=0b00, NS=0
    // l1[2] = (0x80000000ULL & 0xFFFFFFC0000000ULL) | (0ULL << 54) /*UXN*/ | (0ULL << 53) /*PXN*/ | (1ULL << 10) /*AF*/ | (0b11ULL << 8) /*SH*/ | (0b00ULL << 6) /*AP*/ | (0ULL << 5) /*NS*/ | (1ULL << 2) /*Attr1*/ | 0b01ULL;

    // Replace the l1[2] block descriptor with a table descriptor pointing to our new L2 table.
    l1[2] = 0x80003000ULL | 0b11; // This is now a pointer to the L2 table

    // Now, configure the first entry of the L2 table. It will cover the first 2MB
    // of the 0x80000000 address space, where the shim and user code live.
    // We map it as RO for EL1 and RO for EL0 to satisfy W^X.
    l2[0] = (0x80000000ULL & 0XFFFFFFFFFE00000ULL) // PA of the 2MB block
            | (0ULL << 54)      /* UXN=0, EL0 can execute */
            | (0ULL << 53)      /* PXN=0, EL1 can execute */
            | (1ULL << 10)      /* AF=1, Access Flag */
            | (0b11ULL << 8)    /* SH=Inner Shareable */
            | (0b11ULL << 6)    /* AP=RO for EL1, RO for EL0 */
            | (1ULL << 5)       /* NS=1 */
            | (1ULL << 2)       /* Attr1 */
            | 0b01;             /* L2 Block descriptor */

    // Add l2[1] to map the next 2MB block as a writable data page.
    // This covers VA 0x80200000 to 0x803FFFFF.
    // We mark it as Read-Write for EL0 and Execute-Never.
    l2[1] = (0x80200000ULL & 0XFFFFFFFFFE00000ULL)
            | (1ULL << 54)      /* UXN=1, EL0 cannot execute */
            | (1ULL << 53)      /* PXN=1, EL1 cannot execute */
            | (1ULL << 10)      /* AF=1, Access Flag */
            | (0b11ULL << 8)    /* SH=Inner Shareable */
            | (0b01ULL << 6)    /* AP=RW for EL1, RW for EL0 */
            | (1ULL << 5)       /* NS=1, Non-Secure */
            | (1ULL << 2)       /* Attr1 */
            | 0b01;             /* L2 Block descriptor */

    // Load user.bin first.
    load_bin("user.bin", (uint8_t *)guest_mem + 0x4000);
    // THEN, load shim.bin. This ensures the shim's exception handlers are not overwritten.
    load_bin("shim.bin", guest_mem);

    ASSERT_HV(hv_vm_create(NULL));

    ASSERT_HV(hv_vm_map(guest_mem, GUEST_ADDR, GUEST_SIZE, HV_MEMORY_READ | HV_MEMORY_WRITE | HV_MEMORY_EXEC));

    hv_vcpu_t vcpu;
    hv_vcpu_exit_t *exit;
    ASSERT_HV(hv_vcpu_create(&vcpu, &exit, NULL));

    // Set initial PC to shim entry at GUEST_ADDR, PSTATE to EL1h (0x3c5)
    ASSERT_HV(hv_vcpu_set_reg(vcpu, HV_REG_PC, GUEST_ADDR));
    ASSERT_HV(hv_vcpu_set_reg(vcpu, HV_REG_CPSR, 0x3c5));
    // Set the EL1 stack pointer from the host before starting the vCPU.
    // Use the top of the first 4KB page as the stack.
    ASSERT_HV(hv_vcpu_set_sys_reg(vcpu, HV_SYS_REG_SP_EL1, GUEST_ADDR + 0x1000));

    for(size_t i = 0; i < 31; i++) {
        ASSERT_HV(hv_vcpu_set_reg(vcpu, HV_REG_X0+i, 0x0));
    }

    uint64_t hcr_el2;
    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_HCR_EL2, &hcr_el2);
    printf("Before start: HCR_EL2=0x%llx\n", hcr_el2);

    while (1) {
        uint64_t current_pc;
        ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_PC, &current_pc));
        printf("Running vcpu, PC=0x%llx\n", current_pc);
        ASSERT_HV(hv_vcpu_run(vcpu));
        if (exit->reason == HV_EXIT_REASON_EXCEPTION) {
            uint32_t ec = (exit->exception.syndrome >> 26) & 0x3F;
            if (ec == 0x16) { // HVC
                uint16_t imm = exit->exception.syndrome & 0xFFFF;
                uint64_t x0, x1, x2, x3, pc;
                ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_X0, &x0));
                ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_X1, &x1));
                ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_X2, &x2));
                ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_X3, &x3));
                ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_PC, &pc));
                printf("Post-HVC PC=0x%llx\n", pc);

                if (imm == 0) {
                    printf("Guest normal exit HVC #0 with x0=0x%llx\n", x0);
                    break;
                } else if (imm == 1) {
                    uint64_t hcr_el2;
                    hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_HCR_EL2, &hcr_el2);
                    printf("Guest debug HVC #1 step=0x%llx PC=0x%llx; HCR_EL2=0x%llx\n", x0, pc, hcr_el2);
                    continue;
                } else if (imm == 2) {
                    // Bad exception handler - x5 contains vector offset
                    uint64_t x5;
                    ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_X5, &x5));
                    const char *exception_name;
                    switch (x5) {
                        case 0x000: exception_name = "Synchronous, current EL, SP0"; break;
                        case 0x080: exception_name = "IRQ, current EL, SP0"; break;
                        case 0x100: exception_name = "FIQ, current EL, SP0"; break;
                        case 0x180: exception_name = "SError, current EL, SP0"; break;
                        case 0x200: exception_name = "Synchronous, current EL, SPx"; break;
                        case 0x280: exception_name = "IRQ, current EL, SPx"; break;
                        case 0x300: exception_name = "FIQ, current EL, SPx"; break;
                        case 0x380: exception_name = "SError, current EL, SPx"; break;
                        case 0x480: exception_name = "IRQ, lower EL, A64"; break;
                        case 0x500: exception_name = "FIQ, lower EL, A64"; break;
                        case 0x580: exception_name = "SError, lower EL, A64"; break;
                        case 0x600: exception_name = "Synchronous, lower EL, A32"; break;
                        case 0x680: exception_name = "IRQ, lower EL, A32"; break;
                        case 0x700: exception_name = "FIQ, lower EL, A32"; break;
                        case 0x780: exception_name = "SError, lower EL, A32"; break;
                        default: exception_name = "Unknown"; break;
                    }
                    printf("Guest bad exception at vector 0x%03llx: %s\n", x5, exception_name);
                    printf("  ESR_EL1=0x%llx FAR_EL1=0x%llx ELR_EL1=0x%llx SPSR_EL1=0x%llx\n", x0, x1, x2, x3);

                    // Decode ESR_EL1 EC field
                    uint32_t esr_ec = (x0 >> 26) & 0x3F;
                    printf("  Exception Class (EC): 0x%02x\n", esr_ec);

                    uint64_t vbar, ttbr0;
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &vbar));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &ttbr0));
                    printf("Dumping faulting instruction area:\n");
                    dump_guest_memory(guest_mem, x2, 0x100);
                    printf("Dumping vector table area:\n");
                    dump_guest_memory(guest_mem, vbar, 0x800);
                    printf("Dumping page table area:\n");
                    dump_guest_memory(guest_mem, ttbr0, 0x1000);

                    break;
                } else if (imm == 3) {
                    // Dump all system registers configured in shim.S
                    uint64_t vbar_el1, mair_el1, tcr_el1, ttbr0_el1, sctlr_el1, cpacr_el1, elr_el1, spsr_el1;
                    uint64_t ttbr1_el1;
                    uint64_t cpsr;
                    uint64_t sp_el1;

                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_VBAR_EL1, &vbar_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_MAIR_EL1, &mair_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TCR_EL1, &tcr_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR0_EL1, &ttbr0_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SCTLR_EL1, &sctlr_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_CPACR_EL1, &cpacr_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_ELR_EL1, &elr_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SPSR_EL1, &spsr_el1));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_TTBR1_EL1, &ttbr1_el1));
                    ASSERT_HV(hv_vcpu_get_reg(vcpu, HV_REG_CPSR, &cpsr));
                    ASSERT_HV(hv_vcpu_get_sys_reg(vcpu, HV_SYS_REG_SP_EL1, &sp_el1));


                    printf("System registers after shim.S setup:\n");
                    printf("  VBAR_EL1  = 0x%016llx (Vector Base Address)\n", vbar_el1);
                    printf("  MAIR_EL1  = 0x%016llx (Memory Attribute Indirection)\n", mair_el1);
                    printf("  TCR_EL1   = 0x%016llx (Translation Control)\n", tcr_el1);
                    printf("  TTBR0_EL1 = 0x%016llx (Translation Table Base 0)\n", ttbr0_el1);
                    printf("  TTBR1_EL1 = 0x%016llx (Translation Table Base 1)\n", ttbr1_el1);
                    printf("  SCTLR_EL1 = 0x%016llx (System Control)\n", sctlr_el1);
                    printf("    M=%d A=%d C=%d I=%d\n",
                           (int)(sctlr_el1 & 1), (int)((sctlr_el1 >> 1) & 1),
                           (int)((sctlr_el1 >> 2) & 1), (int)((sctlr_el1 >> 12) & 1));
                    printf("  CPACR_EL1 = 0x%016llx (Coprocessor Access Control)\n", cpacr_el1);
                    printf("  ELR_EL1   = 0x%016llx (Exception Link Register)\n", elr_el1);
                    printf("  SPSR_EL1  = 0x%016llx (Saved Program Status Register)\n", spsr_el1);
                    printf("  CPSR      = 0x%016llx (Current Processor Status Register)\n", cpsr);
                    printf("  SP_EL1    = 0x%016llx (Stack Pointer EL1)\n", sp_el1);

                    // Do not advance PC
                    continue;
                } else if (imm == 4) {
                    // HVC #4: Set system register
                    // x0 = register ID, x1 = value
                    uint64_t reg_id = x0, value = x1;

                    printf("HVC #4: reg_id=%llu, value=0x%llx\n", reg_id, value);

                    // Map register IDs to HV_SYS_REG constants
                    hv_sys_reg_t hv_reg;
                    const char *reg_name;
                    switch (reg_id) {
                        case 0: hv_reg = HV_SYS_REG_VBAR_EL1; reg_name = "VBAR_EL1"; break;
                        case 1: hv_reg = HV_SYS_REG_MAIR_EL1; reg_name = "MAIR_EL1"; break;
                        case 2: hv_reg = HV_SYS_REG_TCR_EL1; reg_name = "TCR_EL1"; break;
                        case 3: hv_reg = HV_SYS_REG_TTBR0_EL1; reg_name = "TTBR0_EL1"; break;
                        case 4: hv_reg = HV_SYS_REG_SCTLR_EL1; reg_name = "SCTLR_EL1"; break;
                        case 5: hv_reg = HV_SYS_REG_CPACR_EL1; reg_name = "CPACR_EL1"; break;
                        case 6: hv_reg = HV_SYS_REG_ELR_EL1; reg_name = "ELR_EL1"; break;
                        case 7: hv_reg = HV_SYS_REG_SPSR_EL1; reg_name = "SPSR_EL1"; break;
                        case 8: hv_reg = HV_SYS_REG_TTBR1_EL1; reg_name = "TTBR1_EL1"; break;
                        default:
                            printf("Unknown register ID %llu in HVC #4\n", reg_id);
                            continue;
                    }

                    printf("Setting %s = 0x%llx\n", reg_name, value);
                    ASSERT_HV(hv_vcpu_set_sys_reg(vcpu, hv_reg, value));

                    // Do not advance PC
                    continue;
                } else {
                    printf("Unexpected HVC #%u with x0=0x%llx\n", imm, x0);
                    break;
                }
            } else {
                printf("Unexpected exception class 0x%x syndrome 0x%llx virtual_address 0x%llx physical_address 0x%llx\n",
                       ec, (unsigned long long)exit->exception.syndrome, (unsigned long long)exit->exception.virtual_address, (unsigned long long)exit->exception.physical_address);
                break;
            }
        } else if (exit->reason == HV_EXIT_REASON_CANCELED) {
            printf("VM exit canceled\n");
            break;
        } else {
            printf("Unexpected exit reason 0x%x\n", exit->reason);
            break;
        }
    }

    hv_vcpu_destroy(vcpu);
    hv_vm_destroy();
    free(guest_mem);
    return 0;
}