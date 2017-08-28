/*
 * ARM SMMUv3 support - Internal API
 *
 * Copyright (C) 2014-2016 Broadcom Corporation
 * Copyright (c) 2017 Red Hat, Inc.
 * Written by Prem Mallappa, Eric Auger
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_ARM_SMMU_V3_INTERNAL_H
#define HW_ARM_SMMU_V3_INTERNAL_H

#include "hw/arm/smmu-common.h"

/* MMIO Registers */

REG32(IDR0,                0x0)
    FIELD(IDR0, S1P,         1 , 1)
    FIELD(IDR0, TTF,         2 , 2)
    FIELD(IDR0, COHACC,      4 , 1)
    FIELD(IDR0, ASID16,      12, 1)
    FIELD(IDR0, TTENDIAN,    21, 2)
    FIELD(IDR0, STALL_MODEL, 24, 2)
    FIELD(IDR0, TERM_MODEL,  26, 1)
    FIELD(IDR0, STLEVEL,     27, 2)

REG32(IDR1,                0x4)
    FIELD(IDR1, SIDSIZE,      0 , 6)
    FIELD(IDR1, EVENTQS,      16, 5)
    FIELD(IDR1, CMDQS,        21, 5)

#define SMMU_IDR1_SIDSIZE 16
#define SMMU_CMDQS   19
#define SMMU_EVENTQS 19

REG32(IDR2,                0x8)
REG32(IDR3,                0xc)
REG32(IDR4,                0x10)
REG32(IDR5,                0x14)
     FIELD(IDR5, OAS,         0, 3);
     FIELD(IDR5, GRAN4K,      4, 1);
     FIELD(IDR5, GRAN16K,     5, 1);
     FIELD(IDR5, GRAN64K,     6, 1);

#define SMMU_IDR5_OAS 4

REG32(IIDR,                0x1c)
REG32(CR0,                 0x20)
    FIELD(CR0, SMMU_ENABLE,   0, 1)
    FIELD(CR0, EVENTQEN,      2, 1)
    FIELD(CR0, CMDQEN,        3, 1)

#define SMMU_CR0_RESERVED 0xFFFFFC20

REG32(CR0ACK,              0x24)
REG32(CR1,                 0x28)
REG32(CR2,                 0x2c)
REG32(STATUSR,             0x40)
REG32(IRQ_CTRL,            0x50)
    FIELD(IRQ_CTRL, GERROR_IRQEN,        0, 1)
    FIELD(IRQ_CTRL, PRI_IRQEN,           1, 1)
    FIELD(IRQ_CTRL, EVENTQ_IRQEN,        2, 1)

REG32(IRQ_CTRL_ACK,        0x54)
REG32(GERROR,              0x60)
    FIELD(GERROR, CMDQ_ERR,           0, 1)
    FIELD(GERROR, EVENTQ_ABT_ERR,     2, 1)
    FIELD(GERROR, PRIQ_ABT_ERR,       3, 1)
    FIELD(GERROR, MSI_CMDQ_ABT_ERR,   4, 1)
    FIELD(GERROR, MSI_EVENTQ_ABT_ERR, 5, 1)
    FIELD(GERROR, MSI_PRIQ_ABT_ERR,   6, 1)
    FIELD(GERROR, MSI_GERROR_ABT_ERR, 7, 1)
    FIELD(GERROR, MSI_SFM_ERR,        8, 1)

REG32(GERRORN,             0x64)

#define A_GERROR_IRQ_CFG0  0x68 /* 64b */
REG32(GERROR_IRQ_CFG1, 0x70)
REG32(GERROR_IRQ_CFG2, 0x74)

#define A_STRTAB_BASE      0x80 /* 64b */

#define SMMU_BASE_ADDR_MASK 0xffffffffffe0

REG32(STRTAB_BASE_CFG,     0x88)
    FIELD(STRTAB_BASE_CFG, FMT,      16, 2)
    FIELD(STRTAB_BASE_CFG, SPLIT,    6 , 5)
    FIELD(STRTAB_BASE_CFG, LOG2SIZE, 0 , 6)

#define A_CMDQ_BASE        0x90 /* 64b */
REG32(CMDQ_PROD,           0x98)
REG32(CMDQ_CONS,           0x9c)
    FIELD(CMDQ_CONS, ERR, 24, 7)

#define A_EVENTQ_BASE      0xa0 /* 64b */
REG32(EVENTQ_PROD,         0xa8)
REG32(EVENTQ_CONS,         0xac)

#define A_EVENTQ_IRQ_CFG0  0xb0 /* 64b */
REG32(EVENTQ_IRQ_CFG1,     0xb8)
REG32(EVENTQ_IRQ_CFG2,     0xbc)

#define A_IDREGS           0xfd0

static inline int smmu_enabled(SMMUv3State *s)
{
    return FIELD_EX32(s->cr[0], CR0, SMMU_ENABLE);
}

/* Command Queue Entry */
typedef struct Cmd {
    uint32_t word[4];
} Cmd;

/* Event Queue Entry */
typedef struct Evt  {
    uint32_t word[8];
} Evt;

static inline uint32_t smmuv3_idreg(int regoffset)
{
    /*
     * Return the value of the Primecell/Corelink ID registers at the
     * specified offset from the first ID register.
     * These value indicate an ARM implementation of MMU600 p1
     */
    static const uint8_t smmuv3_ids[] = {
        0x04, 0, 0, 0, 0x84, 0xB4, 0xF0, 0x10, 0x0D, 0xF0, 0x05, 0xB1
    };
    return smmuv3_ids[regoffset / 4];
}

static inline bool smmuv3_eventq_irq_enabled(SMMUv3State *s)
{
    return FIELD_EX32(s->irq_ctrl, IRQ_CTRL, EVENTQ_IRQEN);
}

static inline bool smmuv3_gerror_irq_enabled(SMMUv3State *s)
{
    return FIELD_EX32(s->irq_ctrl, IRQ_CTRL, GERROR_IRQEN);
}

/* Queue Handling */

#define Q_BASE(q)          ((q)->base & SMMU_BASE_ADDR_MASK)
#define WRAP_MASK(q)       (1 << (q)->log2size)
#define INDEX_MASK(q)      (((1 << (q)->log2size)) - 1)
#define WRAP_INDEX_MASK(q) ((1 << ((q)->log2size + 1)) - 1)

#define Q_CONS(q) ((q)->cons & INDEX_MASK(q))
#define Q_PROD(q) ((q)->prod & INDEX_MASK(q))

#define Q_CONS_ENTRY(q)  (Q_BASE(q) + (q)->entry_size * Q_CONS(q))
#define Q_PROD_ENTRY(q)  (Q_BASE(q) + (q)->entry_size * Q_PROD(q))

#define Q_CONS_WRAP(q) (((q)->cons & WRAP_MASK(q)) >> (q)->log2size)
#define Q_PROD_WRAP(q) (((q)->prod & WRAP_MASK(q)) >> (q)->log2size)

static inline bool smmuv3_q_full(SMMUQueue *q)
{
    return ((q->cons ^ q->prod) & WRAP_INDEX_MASK(q)) == WRAP_MASK(q);
}

static inline bool smmuv3_q_empty(SMMUQueue *q)
{
    return (q->cons & WRAP_INDEX_MASK(q)) == (q->prod & WRAP_INDEX_MASK(q));
}

static inline void queue_prod_incr(SMMUQueue *q)
{
    q->prod = (q->prod + 1) & WRAP_INDEX_MASK(q);
}

static inline void queue_cons_incr(SMMUQueue *q)
{
    /*
     * We have to use deposit for the CONS registers to preserve
     * the ERR field in the high bits.
     */
    q->cons = deposit32(q->cons, 0, q->log2size + 1, q->cons + 1);
}

static inline bool smmuv3_cmdq_enabled(SMMUv3State *s)
{
    return FIELD_EX32(s->cr[0], CR0, CMDQEN);
}

static inline bool smmuv3_eventq_enabled(SMMUv3State *s)
{
    return FIELD_EX32(s->cr[0], CR0, EVENTQEN);
}

static inline void smmu_write_cmdq_err(SMMUv3State *s, uint32_t err_type)
{
    s->cmdq.cons = FIELD_DP32(s->cmdq.cons, CMDQ_CONS, ERR, err_type);
}

void smmuv3_write_eventq(SMMUv3State *s, Evt *evt);

/* Commands */

typedef enum SMMUCommandType {
    SMMU_CMD_NONE            = 0x00,
    SMMU_CMD_PREFETCH_CONFIG       ,
    SMMU_CMD_PREFETCH_ADDR,
    SMMU_CMD_CFGI_STE,
    SMMU_CMD_CFGI_STE_RANGE,
    SMMU_CMD_CFGI_CD,
    SMMU_CMD_CFGI_CD_ALL,
    SMMU_CMD_CFGI_ALL,
    SMMU_CMD_TLBI_NH_ALL     = 0x10,
    SMMU_CMD_TLBI_NH_ASID,
    SMMU_CMD_TLBI_NH_VA,
    SMMU_CMD_TLBI_NH_VAA,
    SMMU_CMD_TLBI_EL3_ALL    = 0x18,
    SMMU_CMD_TLBI_EL3_VA     = 0x1a,
    SMMU_CMD_TLBI_EL2_ALL    = 0x20,
    SMMU_CMD_TLBI_EL2_ASID,
    SMMU_CMD_TLBI_EL2_VA,
    SMMU_CMD_TLBI_EL2_VAA,
    SMMU_CMD_TLBI_S12_VMALL  = 0x28,
    SMMU_CMD_TLBI_S2_IPA     = 0x2a,
    SMMU_CMD_TLBI_NSNH_ALL   = 0x30,
    SMMU_CMD_ATC_INV         = 0x40,
    SMMU_CMD_PRI_RESP,
    SMMU_CMD_RESUME          = 0x44,
    SMMU_CMD_STALL_TERM,
    SMMU_CMD_SYNC,
} SMMUCommandType;

static const char *cmd_stringify[] = {
    [SMMU_CMD_PREFETCH_CONFIG] = "SMMU_CMD_PREFETCH_CONFIG",
    [SMMU_CMD_PREFETCH_ADDR]   = "SMMU_CMD_PREFETCH_ADDR",
    [SMMU_CMD_CFGI_STE]        = "SMMU_CMD_CFGI_STE",
    [SMMU_CMD_CFGI_STE_RANGE]  = "SMMU_CMD_CFGI_STE_RANGE",
    [SMMU_CMD_CFGI_CD]         = "SMMU_CMD_CFGI_CD",
    [SMMU_CMD_CFGI_CD_ALL]     = "SMMU_CMD_CFGI_CD_ALL",
    [SMMU_CMD_CFGI_ALL]        = "SMMU_CMD_CFGI_ALL",
    [SMMU_CMD_TLBI_NH_ALL]     = "SMMU_CMD_TLBI_NH_ALL",
    [SMMU_CMD_TLBI_NH_ASID]    = "SMMU_CMD_TLBI_NH_ASID",
    [SMMU_CMD_TLBI_NH_VA]      = "SMMU_CMD_TLBI_NH_VA",
    [SMMU_CMD_TLBI_NH_VAA]     = "SMMU_CMD_TLBI_NH_VAA",
    [SMMU_CMD_TLBI_EL3_ALL]    = "SMMU_CMD_TLBI_EL3_ALL",
    [SMMU_CMD_TLBI_EL3_VA]     = "SMMU_CMD_TLBI_EL3_VA",
    [SMMU_CMD_TLBI_EL2_ALL]    = "SMMU_CMD_TLBI_EL2_ALL",
    [SMMU_CMD_TLBI_EL2_ASID]   = "SMMU_CMD_TLBI_EL2_ASID",
    [SMMU_CMD_TLBI_EL2_VA]     = "SMMU_CMD_TLBI_EL2_VA",
    [SMMU_CMD_TLBI_EL2_VAA]    = "SMMU_CMD_TLBI_EL2_VAA",
    [SMMU_CMD_TLBI_S12_VMALL]  = "SMMU_CMD_TLBI_S12_VMALL",
    [SMMU_CMD_TLBI_S2_IPA]     = "SMMU_CMD_TLBI_S2_IPA",
    [SMMU_CMD_TLBI_NSNH_ALL]   = "SMMU_CMD_TLBI_NSNH_ALL",
    [SMMU_CMD_ATC_INV]         = "SMMU_CMD_ATC_INV",
    [SMMU_CMD_PRI_RESP]        = "SMMU_CMD_PRI_RESP",
    [SMMU_CMD_RESUME]          = "SMMU_CMD_RESUME",
    [SMMU_CMD_STALL_TERM]      = "SMMU_CMD_STALL_TERM",
    [SMMU_CMD_SYNC]            = "SMMU_CMD_SYNC",
};

static inline const char *smmu_cmd_string(SMMUCommandType type)
{
    if (type > SMMU_CMD_NONE && type < ARRAY_SIZE(cmd_stringify)) {
        return cmd_stringify[type] ? cmd_stringify[type] : "UNKNOWN";
    } else {
        return "INVALID";
    }
}

/* CMDQ fields */

typedef enum {
    SMMU_CERROR_NONE = 0,
    SMMU_CERROR_ILL,
    SMMU_CERROR_ABT,
    SMMU_CERROR_ATC_INV_SYNC,
} SMMUCmdError;

enum { /* Command completion notification */
    CMD_SYNC_SIG_NONE,
    CMD_SYNC_SIG_IRQ,
    CMD_SYNC_SIG_SEV,
};

#define CMD_TYPE(x)         extract32((x)->word[0], 0 , 8)
#define CMD_SSEC(x)         extract32((x)->word[0], 10, 1)
#define CMD_SSV(x)          extract32((x)->word[0], 11, 1)
#define CMD_RESUME_AC(x)    extract32((x)->word[0], 12, 1)
#define CMD_RESUME_AB(x)    extract32((x)->word[0], 13, 1)
#define CMD_SYNC_CS(x)      extract32((x)->word[0], 12, 2)
#define CMD_SSID(x)         extract32((x)->word[0], 12, 20)
#define CMD_SID(x)          ((x)->word[1])
#define CMD_VMID(x)         extract32((x)->word[1], 0 , 16)
#define CMD_ASID(x)         extract32((x)->word[1], 16, 16)
#define CMD_RESUME_STAG(x)  extract32((x)->word[2], 0 , 16)
#define CMD_RESP(x)         extract32((x)->word[2], 11, 2)
#define CMD_LEAF(x)         extract32((x)->word[2], 0 , 1)
#define CMD_STE_RANGE(x)    extract32((x)->word[2], 0 , 5)
#define CMD_ADDR(x) ({                                        \
            uint64_t high = (uint64_t)(x)->word[3];           \
            uint64_t low = extract32((x)->word[2], 12, 20);    \
            uint64_t addr = high << 32 | (low << 12);         \
            addr;                                             \
        })

#define SMMU_FEATURE_2LVL_STE (1 << 0)

#endif
