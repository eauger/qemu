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
#    or for fields with enum / ValueRange constraints
#    IDREG_FIELD_ENUM(REG, FIELD, SHIFT, LENGTH,
#                     IDREG_ARCH_VALUE_RANGE(LO, HI), ...)
#    ../..
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
                  'Fields.EnumeratedField', 'Fields.Bitfield', 'Fields.Reserved']
    if _type in leaf_types:
        field_copy = fieldset.copy()

        if _type == 'Fields.Reserved' and not field_copy.get('name'):
            field_copy['name'] = fieldset.get('value', 'RES0')

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
    for key in ['fields', 'values', 'fieldsets', 'constants']:
        for nested in fieldset.get(key, []):
            fields.extend(collect_fields(nested, bit_offset))

    return fields

# Helper to unwrap integer values from strings and interpret
# the value as a binary or a decimal. example: "'0011'"
def parse_value(val_obj):
    if isinstance(val_obj, dict):
        val_obj = val_obj.get("value")

    if val_obj is None:
        return None

    # remove '
    clean_str = str(val_obj).strip().replace("'", "")
    try:
        # try binary extraction
        return int(clean_str, 2)
    except ValueError:
        try:
            # fall back to decimal extraction
            return int(clean_str, 0)
        except ValueError:
            return None

def process_value_container(node, enums):
    if not isinstance(node, dict):
        return

    entry_type = node.get("_type", "")

    if entry_type == "Values.ValueRange":
        s = parse_value(node.get("start"))
        e = parse_value(node.get("end"))
        if s is not None and e is not None:
            enums.append({'lo': s, 'hi': e})
        return

    # Leaf single value (Values.Value) → singleton range
    int_val = parse_value(node)
    if int_val is not None:
        enums.append({'lo': int_val, 'hi': int_val})
        return

    # Non leaf elements (ImplementationDefined, ConditionalValue, etc.)
    for key in ["constraints", "values", "value", "then", "else", "items"]:
        sub = node.get(key)
        if isinstance(sub, list):
            # list of values
            for item in sub:
                process_value_container(item, enums)
        elif isinstance(sub, dict):
            process_value_container(sub, enums)

def extract_field_enums(field):
    enums = []
    if not isinstance(field, dict):
        return enums

    if "value" in field and isinstance(field["value"], dict):
        process_value_container(field["value"], enums)
    elif "values" in field:
        process_value_container(field["values"], enums)

    # Deduplicate identical (lo, hi) pairs while preserving order.
    already_collected = set()
    unique_enums = []
    for item in enums:
        key = (item['lo'], item['hi'])
        if key not in already_collected:
            already_collected.add(key)
            unique_enums.append(item)

    return unique_enums

def merge_enum_ranges(ranges):
    if not ranges:
        return []
    ordered = sorted(ranges, key=lambda r: (r['lo'], r['hi']))
    merged = [{'lo': ordered[0]['lo'], 'hi': ordered[0]['hi']}]
    for item in ordered[1:]:
        cur = merged[-1]
        if item['lo'] <= cur['hi'] + 1:
            cur['hi'] = max(cur['hi'], item['hi'])
        else:
            merged.append({'lo': item['lo'], 'hi': item['hi']})
    return merged

def format_field_enum(reg_name, field_name, lsb, width, ranges):
    """IDREG_FIELD_ENUM(..., IDREG_ARCH_VALUE_RANGE(...), ...)."""
    lines = [
        f"    IDREG_FIELD_ENUM({reg_name}, {field_name}, {lsb}, {width},\n"
    ]
    for i, item in enumerate(ranges):
        lo, hi = item['lo'], item['hi']
        comma = "," if i + 1 < len(ranges) else ""
        lines.append(f"                 IDREG_ARCH_VALUE_RANGE({lo}, {hi}){comma}\n")
    lines.append("    )\n")
    return "".join(lines)

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

        fieldset_variants = []

        for fieldset in register.get('fieldsets', []):
            current_fieldset_fields = {}
            candidates = collect_fields(fieldset)

            for val in candidates:
                # reserved fields may leave the 'name' empty or set to None and use
                # value or label instead. Last '' garantees we fall back to an empty
                # string and prevent the next line from cashing
                raw_name = val.get('name') or val.get('label') or val.get('value') or ''
                name = raw_name.strip()
                if not name:
                    continue

                name_upper = name.upper()

                base_reserved = None
                for r_type in ["RES0", "RES1", "RAZ", "RAO", "WI", "UNKNOWN"]:
                    if r_type == name_upper:
                        # Turn UNKNOWN to RES0
                        base_reserved = "RES0" if r_type == "UNKNOWN" else r_type
                        break

                if base_reserved:
                    name = base_reserved
                elif "RESERVED" in name_upper:
                    continue

                for r in val.get('rangeset', []):
                    lsb = int(r.get('start'))
                    width = r.get('width')
                    msb = lsb + int(width) - 1

                    if base_reserved:
                        # reserved fields have same name so append their lsb
                        # to prevent fields from overriding each other
                        unique_key = f"{name}_{lsb}"
                    else:
                        unique_key = name

                    # within the same fieldset we can have fields with the
                    # same name and different msbs. Keep the largest.
                    if unique_key not in current_fieldset_fields or \
                       msb > current_fieldset_fields[unique_key]['msb']:
                        enums = extract_field_enums(val)
                        current_fieldset_fields[unique_key] = {
                            'raw_name': name,
                            'lsb': lsb,
                            'msb': msb,
                            'width': width,
                            'enums': enums
                        }

            if current_fieldset_fields:
                fieldset_variants.append(current_fieldset_fields)

        best_fieldset = {}
        max_total_width = -1

        # among several variants of a fieldset keep the largest one
        for variant in fieldset_variants:
            total_width = sum(f['width'] for f in variant.values())

            if total_width > max_total_width:
                max_total_width = total_width
                best_fieldset = variant

        unique_fields = best_fieldset

        # Sort by decreasing lsbs
        sorted_fields = sorted(unique_fields.items(),
                               key=lambda x: x[1]['lsb'], reverse=True)

        for unique_key, bits in sorted_fields:
            enums_list = bits.get('enums', [])
            clean_name = bits.get('raw_name', unique_key)

            if enums_list:
                final_output += format_field_enum(
                    reg_name, clean_name, bits['lsb'], bits['width'],
                    merge_enum_ranges(enums_list))
            else:
                final_output += (
                    f"    IDREG_FIELD({reg_name}, "
                    f"{clean_name}, {bits['lsb']}, {bits['width']})\n"
                )

        final_output += f"IDREG_END({reg_name})\n"
        final_output += "\n"

    os.makedirs("target/arm", exist_ok=True)
    with open("target/arm/cpu-idregs.h.inc", 'w') as f:
        f.write("/* AUTOMATICALLY GENERATED, DO NOT MODIFY */\n\n")
        f.write("/* SPDX-License-Identifier: GPL-2.0-or-later */\n\n")
        f.write("/* IDREG_START(REG) */\n")
        f.write("/*     IDREG_FIELD(REG, FIELD, SHIFT, LENGTH) */\n")
        f.write("/*     or for fields with enum / ValueRange constraints */\n")
        f.write("/*     IDREG_FIELD_ENUM(REG, FIELD, SHIFT, LENGTH, */\n")
        f.write("/*                      IDREG_ARCH_VALUE_RANGE(LO, HI), ...) */\n")
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
