/*
 * ID registers and their field definition
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef TARGET_ARM_CPU_IDREGS_H
#define TARGET_ARM_CPU_IDREGS_H

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "cpu.h"
#include "cpu-sysregs.h"

typedef struct ARM64SysRegArchValueRange {
    uint64_t lo;
    uint64_t hi;
} ARM64SysRegArchValueRange;

typedef struct ARM64SysRegField {
    const char *name; /* name of the field, for instance CTR_EL0_IDC */
    ARMIDRegisterIdx index; /* parent register, e.g. CTR_EL0_IDX */
    int shift; /* lsb of the field in the register */
    int length; /* highest bit number */
    const ARM64SysRegArchValueRange *arch_value_ranges;
    uint32_t arch_value_ranges_count;
} ARM64SysRegField;

typedef struct ARM64SysReg {
    const char *name;   /* name of the sysreg, for instance CTR_EL0 */
    ARMIDRegisterIdx index; /* register index, e.g. CTR_EL0_IDX */
    struct ARM64SysRegField *fields;
    uint32_t fields_count;
    uint64_t writable_mask;
} ARM64SysReg;

/*
 * List of exposed ID regs (automatically populated from AARCHMRS Registers.json)
 */
extern ARM64SysReg arm64_id_regs[NUM_ID_IDX];

/* accelerator specific callback used to apply a new field value on a vcpu */
typedef int (*ARMSysregPropApplyFn)(ARMCPU *cpu, ARM64SysRegField *field,
                                    uint64_t newfv, Error **errp);

/**
 * arm_cpu_apply_sysreg_props:
 *
 * Apply all SYSREG prop values
 * @cpu:  vcpu on which the values are applied
 * @accel_apply_fn: acceleration specific callback used to override
 *                  a single field value
 * @errp: error handle
 *
 * return 0 on sucess or negative value with errp set on failure
 */
int arm_cpu_apply_sysreg_props(ARMCPU *cpu, ARMSysregPropApplyFn accel_apply_fn,
                               Error **errp);

/**
 * arm_cpu_validate_sysreg_prop:
 *
 * accelerator agnostic validation of a field value (especially regarding
 * the consistency with legacy composite options such as virtualization, pmu,
 * sve ...)
 *
 * @cpu: vcpu the field value is validated against
 * @field: field the value is applied to
 * @value: field value to be validated
 * @errp: error handle set if validation fails
 *
 * returns true if validation succeeds
 */
bool arm_cpu_validate_sysreg_prop(ARMCPU *cpu, ARM64SysRegField *field,
                                  uint64_t value, Error **errp);

#endif
