/*
 *   ARM ID register field table.
 *
 *   Builds the per-id-register field descriptor arrays and the global
 *   arm_idregs[] table.
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "cpu.h"
#include "cpu-idregs.h"

/*
 * Generate an array of architecturally defined value ranges for each field
 * that has enum / ValueRange constraints.
 */

#define IDREG_ARCH_VALUE_RANGE(_lo, _hi) { .lo = (_lo), .hi = (_hi) }

#define IDREG_START(reg)
#define IDREG_END(reg)
#define IDREG_FIELD(reg, field, shift, length)
#define IDREG_FIELD_ENUM(reg, field, shift, length, ...) \
    static const ARM64SysRegArchValueRange reg##_##field##_arch_value_ranges[] = { \
        __VA_ARGS__                                                              \
    };

#include "cpu-idregs.h.inc"

#undef IDREG_START
#undef IDREG_END
#undef IDREG_FIELD
#undef IDREG_FIELD_ENUM
#undef IDREG_ARCH_VALUE_RANGE

/* generate an array of per-register ArmIdRegField[] descriptors */

#define IDREG_START(reg) \
    static ARM64SysRegField reg##_fields[] = {

#define IDREG_END(reg) \
    };

#define IDREG_ARCH_VALUE_RANGE(_lo, _hi)

#define IDREG_FIELD_ENUM(reg, field, _shift, _length, ...)           \
    {                                                                \
        .name = #field,                                              \
        .index = reg##_IDX,                                          \
        .shift = (_shift),                                           \
        .length = (_length),                                         \
        .arch_value_ranges = reg##_##field##_arch_value_ranges,      \
        .arch_value_ranges_count =                                   \
            ARRAY_SIZE(reg##_##field##_arch_value_ranges),           \
    },

#define IDREG_FIELD(reg, field, _shift, _length)  \
    {                                             \
        .name = #field,                           \
        .index = reg##_IDX,                       \
        .shift = (_shift),                        \
        .length = (_length),                      \
    },

#include "cpu-idregs.h.inc"

#undef IDREG_START
#undef IDREG_END
#undef IDREG_FIELD
#undef IDREG_FIELD_ENUM
#undef IDREG_ARCH_VALUE_RANGE

/* generate an array of top level ID registers */

#define IDREG_END(reg)
#define IDREG_FIELD(reg, field, shift, length)
#define IDREG_FIELD_ENUM(reg, field, shift, length, ...)
#define IDREG_ARCH_VALUE_RANGE(_lo, _hi)

#define IDREG_START(reg)                            \
    [reg##_IDX] = {                                 \
        .name = #reg,                               \
        .index = reg##_IDX,                         \
        .fields = reg##_fields,                     \
        .fields_count = ARRAY_SIZE(reg##_fields),   \
        .writable_mask = 0ULL,                      \
    },

ARM64SysReg arm64_id_regs[NUM_ID_IDX] = {
#include "cpu-idregs.h.inc"
};
