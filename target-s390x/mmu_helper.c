/*
 * S390x MMU related functions
 *
 * Copyright (c) 2011 Alexander Graf
 * Copyright (c) 2015 Thomas Huth, IBM Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "cpu.h"

/* #define DEBUG_S390 */
/* #define DEBUG_S390_PTE */
/* #define DEBUG_S390_STDOUT */

#ifdef DEBUG_S390
#ifdef DEBUG_S390_STDOUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); \
         qemu_log(fmt, ##__VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { qemu_log(fmt, ## __VA_ARGS__); } while (0)
#endif
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef DEBUG_S390_PTE
#define PTE_DPRINTF DPRINTF
#else
#define PTE_DPRINTF(fmt, ...) \
    do { } while (0)
#endif

/* Fetch/store bits in the translation exception code: */
#define FS_READ  0x800
#define FS_WRITE 0x400

static void trigger_prot_fault(CPUS390XState *env, target_ulong vaddr,
                               uint64_t asc, int rw, bool exc)
{
    CPUState *cs = CPU(s390_env_get_cpu(env));
    uint64_t tec;

    tec = vaddr | (rw == 1 ? FS_WRITE : FS_READ) | 4 | asc >> 46;

    DPRINTF("%s: trans_exc_code=%016" PRIx64 "\n", __func__, tec);

    if (!exc) {
        return;
    }

    stq_phys(cs->as, env->psa + offsetof(LowCore, trans_exc_code), tec);
    trigger_pgm_exception(env, PGM_PROTECTION, ILEN_LATER_INC);
}

static void trigger_page_fault(CPUS390XState *env, target_ulong vaddr,
                               uint32_t type, uint64_t asc, int rw, bool exc)
{
    CPUState *cs = CPU(s390_env_get_cpu(env));
    int ilen = ILEN_LATER;
    uint64_t tec;

    tec = vaddr | (rw == 1 ? FS_WRITE : FS_READ) | asc >> 46;

    DPRINTF("%s: vaddr=%016" PRIx64 " bits=%d\n", __func__, vaddr, bits);

    if (!exc) {
        return;
    }

    /* Code accesses have an undefined ilc.  */
    if (rw == 2) {
        ilen = 2;
    }

    stq_phys(cs->as, env->psa + offsetof(LowCore, trans_exc_code), tec);
    trigger_pgm_exception(env, type, ilen);
}

/**
 * Translate real address to absolute (= physical)
 * address by taking care of the prefix mapping.
 */
static target_ulong mmu_real2abs(CPUS390XState *env, target_ulong raddr)
{
    if (raddr < 0x2000) {
        return raddr + env->psa;    /* Map the lowcore. */
    } else if (raddr >= env->psa && raddr < env->psa + 0x2000) {
        return raddr - env->psa;    /* Map the 0 page. */
    }
    return raddr;
}

/* Decode page table entry (normal 4KB page) */
static int mmu_translate_pte(CPUS390XState *env, target_ulong vaddr,
                             uint64_t asc, uint64_t asce,
                             target_ulong *raddr, int *flags, int rw, bool exc)
{
    if (asce & _PAGE_INVALID) {
        DPRINTF("%s: PTE=0x%" PRIx64 " invalid\n", __func__, asce);
        trigger_page_fault(env, vaddr, PGM_PAGE_TRANS, asc, rw, exc);
        return -1;
    }

    if (asce & _PAGE_RO) {
        *flags &= ~PAGE_WRITE;
    }

    *raddr = asce & _ASCE_ORIGIN;

    PTE_DPRINTF("%s: PTE=0x%" PRIx64 "\n", __func__, asce);

    return 0;
}

#define VADDR_PX    0xff000         /* Page index bits */

/* Decode segment table entry */
static int mmu_translate_segment(CPUS390XState *env, target_ulong vaddr,
                                 uint64_t asc, uint64_t st_entry,
                                 target_ulong *raddr, int *flags, int rw,
                                 bool exc)
{
    CPUState *cs = CPU(s390_env_get_cpu(env));
    uint64_t origin, offs, pt_entry;

    if (st_entry & _SEGMENT_ENTRY_RO) {
        *flags &= ~PAGE_WRITE;
    }

    if ((st_entry & _SEGMENT_ENTRY_FC) && (env->cregs[0] & CR0_EDAT)) {
        /* Decode EDAT1 segment frame absolute address (1MB page) */
        *raddr = (st_entry & 0xfffffffffff00000ULL) | (vaddr & 0xfffff);
        PTE_DPRINTF("%s: SEG=0x%" PRIx64 "\n", __func__, st_entry);
        return 0;
    }

    /* Look up 4KB page entry */
    origin = st_entry & _SEGMENT_ENTRY_ORIGIN;
    offs  = (vaddr & VADDR_PX) >> 9;
    pt_entry = ldq_phys(cs->as, origin + offs);
    PTE_DPRINTF("%s: 0x%" PRIx64 " + 0x%" PRIx64 " => 0x%016" PRIx64 "\n",
                __func__, origin, offs, pt_entry);
    return mmu_translate_pte(env, vaddr, asc, pt_entry, raddr, flags, rw, exc);
}

/* Decode region table entries */
static int mmu_translate_region(CPUS390XState *env, target_ulong vaddr,
                                uint64_t asc, uint64_t entry, int level,
                                target_ulong *raddr, int *flags, int rw,
                                bool exc)
{
    CPUState *cs = CPU(s390_env_get_cpu(env));
    uint64_t origin, offs, new_entry;
    const int pchks[4] = {
        PGM_SEGMENT_TRANS, PGM_REG_THIRD_TRANS,
        PGM_REG_SEC_TRANS, PGM_REG_FIRST_TRANS
    };

    PTE_DPRINTF("%s: 0x%" PRIx64 "\n", __func__, entry);

    origin = entry & _REGION_ENTRY_ORIGIN;
    offs = (vaddr >> (17 + 11 * level / 4)) & 0x3ff8;

    new_entry = ldq_phys(cs->as, origin + offs);
    PTE_DPRINTF("%s: 0x%" PRIx64 " + 0x%" PRIx64 " => 0x%016" PRIx64 "\n",
                __func__, origin, offs, new_entry);

    if ((new_entry & _REGION_ENTRY_INV) != 0) {
        /* XXX different regions have different faults */
        DPRINTF("%s: invalid region\n", __func__);
        trigger_page_fault(env, vaddr, PGM_SEGMENT_TRANS, asc, rw, exc);
        return -1;
    }

    if ((new_entry & _REGION_ENTRY_TYPE_MASK) != level) {
        trigger_page_fault(env, vaddr, PGM_TRANS_SPEC, asc, rw, exc);
        return -1;
    }

    /* XXX region protection flags */
    /* *flags &= ~PAGE_WRITE */

    if (level == _ASCE_TYPE_SEGMENT) {
        return mmu_translate_segment(env, vaddr, asc, new_entry, raddr, flags,
                                     rw, exc);
    }

    /* Check region table offset and length */
    offs = (vaddr >> (28 + 11 * (level - 4) / 4)) & 3;
    if (offs < ((new_entry & _REGION_ENTRY_TF) >> 6)
        || offs > (new_entry & _REGION_ENTRY_LENGTH)) {
        DPRINTF("%s: invalid offset or len (%lx)\n", __func__, new_entry);
        trigger_page_fault(env, vaddr, pchks[level / 4 - 1], asc, rw, exc);
        return -1;
    }

    /* yet another region */
    return mmu_translate_region(env, vaddr, asc, new_entry, level - 4,
                                raddr, flags, rw, exc);
}

static int mmu_translate_asc(CPUS390XState *env, target_ulong vaddr,
                             uint64_t asc, target_ulong *raddr, int *flags,
                             int rw, bool exc)
{
    uint64_t asce = 0;
    int level;
    int r;

    switch (asc) {
    case PSW_ASC_PRIMARY:
        PTE_DPRINTF("%s: asc=primary\n", __func__);
        asce = env->cregs[1];
        break;
    case PSW_ASC_SECONDARY:
        PTE_DPRINTF("%s: asc=secondary\n", __func__);
        asce = env->cregs[7];
        break;
    case PSW_ASC_HOME:
        PTE_DPRINTF("%s: asc=home\n", __func__);
        asce = env->cregs[13];
        break;
    }

    if (asce & _ASCE_REAL_SPACE) {
        /* direct mapping */
        *raddr = vaddr;
        return 0;
    }

    level = asce & _ASCE_TYPE_MASK;
    switch (level) {
    case _ASCE_TYPE_REGION1:
        if ((vaddr >> 62) > (asce & _ASCE_TABLE_LENGTH)) {
            trigger_page_fault(env, vaddr, PGM_REG_FIRST_TRANS, asc, rw, exc);
            return -1;
        }
        break;
    case _ASCE_TYPE_REGION2:
        if (vaddr & 0xffe0000000000000ULL) {
            DPRINTF("%s: vaddr doesn't fit 0x%16" PRIx64
                    " 0xffe0000000000000ULL\n", __func__, vaddr);
            trigger_page_fault(env, vaddr, PGM_ASCE_TYPE, asc, rw, exc);
            return -1;
        }
        if ((vaddr >> 51 & 3) > (asce & _ASCE_TABLE_LENGTH)) {
            trigger_page_fault(env, vaddr, PGM_REG_SEC_TRANS, asc, rw, exc);
            return -1;
        }
        break;
    case _ASCE_TYPE_REGION3:
        if (vaddr & 0xfffffc0000000000ULL) {
            DPRINTF("%s: vaddr doesn't fit 0x%16" PRIx64
                    " 0xfffffc0000000000ULL\n", __func__, vaddr);
            trigger_page_fault(env, vaddr, PGM_ASCE_TYPE, asc, rw, exc);
            return -1;
        }
        if ((vaddr >> 40 & 3) > (asce & _ASCE_TABLE_LENGTH)) {
            trigger_page_fault(env, vaddr, PGM_REG_THIRD_TRANS, asc, rw, exc);
            return -1;
        }
        break;
    case _ASCE_TYPE_SEGMENT:
        if (vaddr & 0xffffffff80000000ULL) {
            DPRINTF("%s: vaddr doesn't fit 0x%16" PRIx64
                    " 0xffffffff80000000ULL\n", __func__, vaddr);
            trigger_page_fault(env, vaddr, PGM_ASCE_TYPE, asc, rw, exc);
            return -1;
        }
        if ((vaddr >> 29 & 3) > (asce & _ASCE_TABLE_LENGTH)) {
            trigger_page_fault(env, vaddr, PGM_SEGMENT_TRANS, asc, rw, exc);
            return -1;
        }
        break;
    }

    r = mmu_translate_region(env, vaddr, asc, asce, level, raddr, flags, rw,
                             exc);
    if ((rw == 1) && !(*flags & PAGE_WRITE)) {
        trigger_prot_fault(env, vaddr, asc, rw, exc);
        return -1;
    }

    return r;
}

/**
 * Translate a virtual (logical) address into a physical (absolute) address.
 * @param vaddr  the virtual address
 * @param rw     0 = read, 1 = write, 2 = code fetch
 * @param asc    address space control (one of the PSW_ASC_* modes)
 * @param raddr  the translated address is stored to this pointer
 * @param flags  the PAGE_READ/WRITE/EXEC flags are stored to this pointer
 * @param exc    true = inject a program check if a fault occured
 * @return       0 if the translation was successfull, -1 if a fault occured
 */
int mmu_translate(CPUS390XState *env, target_ulong vaddr, int rw, uint64_t asc,
                  target_ulong *raddr, int *flags, bool exc)
{
    int r = -1;
    uint8_t *sk;

    *flags = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    vaddr &= TARGET_PAGE_MASK;

    if (!(env->psw.mask & PSW_MASK_DAT)) {
        *raddr = vaddr;
        r = 0;
        goto out;
    }

    switch (asc) {
    case PSW_ASC_PRIMARY:
    case PSW_ASC_HOME:
        r = mmu_translate_asc(env, vaddr, asc, raddr, flags, rw, exc);
        break;
    case PSW_ASC_SECONDARY:
        /*
         * Instruction: Primary
         * Data: Secondary
         */
        if (rw == 2) {
            r = mmu_translate_asc(env, vaddr, PSW_ASC_PRIMARY, raddr, flags,
                                  rw, exc);
            *flags &= ~(PAGE_READ | PAGE_WRITE);
        } else {
            r = mmu_translate_asc(env, vaddr, PSW_ASC_SECONDARY, raddr, flags,
                                  rw, exc);
            *flags &= ~(PAGE_EXEC);
        }
        break;
    case PSW_ASC_ACCREG:
    default:
        hw_error("guest switched to unknown asc mode\n");
        break;
    }

 out:
    /* Convert real address -> absolute address */
    *raddr = mmu_real2abs(env, *raddr);

    if (*raddr <= ram_size) {
        sk = &env->storage_keys[*raddr / TARGET_PAGE_SIZE];
        if (*flags & PAGE_READ) {
            *sk |= SK_R;
        }

        if (*flags & PAGE_WRITE) {
            *sk |= SK_C;
        }
    }

    return r;
}
