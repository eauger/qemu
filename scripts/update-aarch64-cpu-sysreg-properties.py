#!/usr/bin/env python3

# This script takes as input the Registers.json file delivered in
# the AARCHMRS Features Model downloadable from the Arm Developer
# A-Profile Architecture Exploration Tools page:
# https://developer.arm.com/Architectures/A-Profile%20Architecture#Downloads
# and outputs target/arm/cpu-idregs.h.inc content.
# ID regs are defined using this pattern:
#
# IDREG_START(REG)
#    IDREG_FIELD(REG, FIELD, SHIFT, LENGTH)
# ...
# IDREG_END(REG)
#
# Copyright (C) 2026 Red Hat, Inc.
#
# Authors: Eric Auger <eric.auger@redhat.com>
#
# SPDX-License-Identifier: GPL-2.0-or-later


import json
import os
import sys
from aarch64_sysreg_helpers import extract_idregs_from_registers_json

# look for all the fields belonging to a register fieldset
# this is a recursive parser where bit_offset is used to convey
# bit positions from the parent container if any
def collect_fields(fieldset, bit_offset=0):
    fields = []
    if not isinstance(fieldset, dict):
        return fields

    _type = fieldset.get('_type', '')

    # Leaf array type (for example CLIDR_EL1 Ctype<n>, Ttype<n>)
    if _type == 'Fields.Array':
        name_template = fieldset.get('name') or fieldset.get('label', '')
        index_info = fieldset.get('indexes', [{}])[0]
        start_idx = index_info.get('start', 0)
        count = index_info.get('width', 0)

        full_range = fieldset.get('rangeset', [{}])[0]
        bit_start = full_range.get('start', 0) + bit_offset
        elem_width = full_range.get('width', 0) // count if count else 0

        for i in range(count):
            idx = start_idx + i
            # Append the actual index to the field by replacing <n>
            field_name = name_template.replace('<n>', str(idx))
            fields.append({
                'name': field_name,
                'rangeset': [{
                    'start': bit_start + (i * elem_width),
                    'width': elem_width
                }],
                '_type': 'Fields.Field'
            })
        return fields

    # Conditional field which depend on specific FEAT_*
    # like ID_AA64ZFR0_EL1_SVEver or ID_AA64MMFR4_EL1_E2H0
    elif _type == 'Fields.ConditionalField':
        container_bit_offset = bit_offset
        if fieldset.get('rangeset'):
            # Parent container defines the absolute start bit
            container_bit_offset = fieldset['rangeset'][0].get('start', bit_offset)

        for entry in fieldset.get('fields', []):
            inner = entry.get('field')
            if inner:
                fields.extend(collect_fields(inner, container_bit_offset))
        return fields

    # Field Types
    leaf_types = ['Fields.Field', 'Fields.ConstantField',
                  'Fields.EnumeratedField', 'Fields.Bitfield']
    if _type in leaf_types:
        field_copy = fieldset.copy()
        if field_copy.get('rangeset'):
            new_ranges = []
            for r in field_copy['rangeset']:
                nr = r.copy()
                nr['start'] = r.get('start', 0) + bit_offset
                new_ranges.append(nr)
            field_copy['rangeset'] = new_ranges
        fields.append(field_copy)
        return fields

    # Traverse the hierarchy for other cases
    for key in ['fields', 'values', 'fieldsets']:
        for nested in fieldset.get(key, []):
            fields.extend(collect_fields(nested, bit_offset))

    return fields

def generate_sysreg_properties_from_registers_json(idregs, raw_json_path):
    with open(raw_json_path, 'r') as f:
        register_data = json.load(f)

    regs = {r.get('name'): r for r in register_data if r.get('_type') == 'Register'}

    final_output = ""

    for reg_name in idregs:
        register = regs.get(reg_name)
        if not register:
            print(f"Error: Inconsistency when analyzing registers!", file=sys.stderr)
            sys.exit(1)

        final_output += f"IDREG_START({reg_name})\n"

        unique_fields = {}
        for fieldset in register.get('fieldsets', []):
            candidates = collect_fields(fieldset)
            for val in candidates:
                name = (val.get('name') or val.get('label', '')).strip()
                if not name or "RESERVED" in name.upper():
                    continue
                for r in val.get('rangeset', []):
                    lsb = int(r.get('start'))
                    width = r.get('width')
                    msb = lsb + int(width) - 1

                    # In json, some idregs may have multiple fielsets, depending
                    # on some architecture configurations. If this happens, pick
                    # up a single one with the largest msb (known one is CCSIDR
                    # which is currently skipped)
                    if name not in unique_fields or msb > unique_fields[name]['msb']:
                       unique_fields[name] = {'lsb': lsb, 'msb': msb, 'width': width}

        # Sort by decreasing lsbs
        sorted_fields = sorted(unique_fields.items(),
                               key=lambda x: x[1]['lsb'], reverse=True)

        for name, bits in sorted_fields:
            line = (f"    IDREG_FIELD({reg_name}, "
                    f"{name}, {bits['lsb']}, {bits['width']})\n")
            final_output += line
        final_output += f"IDREG_END({reg_name})\n"
        final_output += "\n"

    os.makedirs("target/arm", exist_ok=True)
    with open("target/arm/cpu-idregs.h.inc", 'w') as f:
        f.write("/* AUTOMATICALLY GENERATED, DO NOT MODIFY */\n\n")
        f.write("/* SPDX-License-Identifier: GPL-2.0-or-later */\n\n")
        f.write("/* IDREG_START(REG) */\n")
        f.write("/*     IDREG_FIELD(REG, FIELD, SHIFT, LENGTH) */\n")
        f.write("/* ... */\n")
        f.write("/* IDREG_END(REG) */\n\n")
        f.write(final_output)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/update-aarch64-cpu-sysreg-properties.py "
              "<path_to_registers_json>")
    else:
        json_path = sys.argv[1]

        idregs = extract_idregs_from_registers_json(json_path)

        if idregs:
            generate_sysreg_properties_from_registers_json(idregs, json_path)
            print("Generated target/arm/cpu-idregs.h.inc")
