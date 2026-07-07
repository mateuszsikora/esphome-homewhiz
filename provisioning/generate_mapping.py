#!/usr/bin/env python3
"""Generate a self-contained ESPHome ``mapping.h`` from a HomeWhiz config.

The generator walks the ``ApplianceConfiguration`` **generically** — it never
looks up washer field names. Every feature in any section is classified into one
of four primitive kinds (plan §3.7):

* ``ENUM``     — read index + ``{byte -> strKey}`` table
* ``NUMERIC``  — read index + ``factor`` (value = ``byte * factor``)
* ``PROGRESS`` — hour index + minute index (minutes = ``h*60 + m``)
* ``FLAG``     — read index + bit index

Pointed at any HomeWhiz BT appliance's config it emits a valid header with no
code changes — the only appliance semantics live in the generated table.
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field

from dacite import Config, from_dict

from vendor.appliance_config import (
    ApplianceConfiguration,
    ApplianceFeature,
    ApplianceProgress,
    ApplianceState,
    ApplianceSubState,
    ApplianceWarning,
)

KIND_ENUM = 0
KIND_NUMERIC = 1
KIND_PROGRESS = 2
KIND_FLAG = 3


@dataclass
class EnumEntry:
    value: int
    key: str


@dataclass
class Field:
    key: str
    kind: int
    index: int
    index2: int = 0
    factor: float = 0.0
    enums: list[EnumEntry] = field(default_factory=list)
    label: str | None = None
    unit: str | None = None
    diagnostic: bool = False  # -> Home Assistant entity_category: diagnostic


@dataclass
class Write:
    key: str
    index: int


class Walker:
    """Collects fields/writes from a config, generically, deduping by key."""

    def __init__(self, localization: dict[str, str]):
        self.localization = localization
        self.fields: list[Field] = []
        self.writes: list[Write] = []
        self._seen: set[str] = set()
        self._seen_writes: set[str] = set()

    def _label(self, key: str) -> str | None:
        # Upstream lowercases localization keys (api.fetch_localizations).
        return self.localization.get(key.lower())

    def _add(self, f: Field) -> None:
        if f.key in self._seen:
            # first definition wins; keeps the table deterministic
            print(
                f"warning: duplicate strKey {f.key!r} — keeping the first definition",
                file=sys.stderr,
            )
            return
        self._seen.add(f.key)
        f.label = self._label(f.key)
        self.fields.append(f)

    def _add_write(self, key: str, index: int | None) -> None:
        if index is None or key in self._seen_writes:
            return
        self._seen_writes.add(key)
        self.writes.append(Write(key, index))

    # -- per-section handlers ------------------------------------------------

    def add_state(self, key: str, state: ApplianceState) -> None:
        if state is None or state.wifiArrayReadIndex is None:
            return
        enums = [EnumEntry(o.wifiArrayValue, o.strKey) for o in state.states]
        self._add(Field(key, KIND_ENUM, state.wifiArrayReadIndex, enums=enums))
        self._add_write(key, state.wifiArrayWriteIndex)

    def add_substate(self, key: str, sub: ApplianceSubState) -> None:
        if sub is None:
            return
        enums = [EnumEntry(o.wifiArrayValue, o.strKey) for o in sub.subStates]
        self._add(Field(key, KIND_ENUM, sub.wifiArrayReadIndex, enums=enums, diagnostic=True))

    def add_program(self, program) -> None:  # ApplianceProgram
        if program is None:
            return
        enums = [EnumEntry(o.wifiArrayValue, o.strKey) for o in program.values]
        self._add(Field(program.strKey, KIND_ENUM, program.wifiArrayIndex, enums=enums))
        self._add_write(program.strKey, program.wfaWriteIndex)

    def add_features(
        self, features: list[ApplianceFeature] | None, *, diagnostic: bool = False
    ) -> None:
        for feat in features or []:
            if feat.strKey is None:
                continue
            if feat.enumValues:
                enums = [EnumEntry(o.wifiArrayValue, o.strKey) for o in feat.enumValues]
                self._add(
                    Field(
                        feat.strKey, KIND_ENUM, feat.wifiArrayIndex,
                        enums=enums, diagnostic=diagnostic,
                    )
                )
            elif feat.boundedValues:
                bounds = feat.boundedValues[0]
                self._add(
                    Field(
                        feat.strKey,
                        KIND_NUMERIC,
                        feat.wifiArrayIndex,
                        factor=bounds.factor,
                        unit=bounds.unit,
                        diagnostic=diagnostic,
                    )
                )
            else:
                continue  # nothing decodable
            self._add_write(feat.strKey, feat.wfaWriteIndex)

    def add_progress(self, progress: ApplianceProgress | None) -> None:
        if progress is None:
            return
        for attr in (
            "autoOff", "autoOn", "delay", "duration", "elapsed",
            "fermentedremaining", "remaining", "remainingOrElapsed",
        ):
            pf = getattr(progress, attr, None)
            if pf is None:
                continue
            self._add(
                Field(
                    pf.strKey,
                    KIND_PROGRESS,
                    pf.hour.wifiArrayIndex,
                    index2=pf.minute.wifiArrayIndex,
                )
            )
            self._add_write(pf.strKey, pf.wfaWriteIndex)

    def add_warnings(self, warning: ApplianceWarning | None) -> None:
        if warning is None:
            return
        for opt in warning.warnings:
            self._add(
                Field(
                    opt.strKey, KIND_FLAG, warning.wifiArrayReadIndex,
                    index2=opt.bitIndex, diagnostic=True,
                )
            )

    def walk(self, config: ApplianceConfiguration) -> None:
        # Primary controls: state, program, program options, remaining/delay time.
        self.add_state("STATE", config.deviceStates)
        self.add_program(config.program)
        self.add_features(config.subPrograms)
        self.add_progress(config.progressVariables)
        # Diagnostic: sub-state, settings, monitorings, custom/commands, warnings.
        self.add_substate("SUBSTATE", config.deviceSubStates)
        self.add_features(config.customSubPrograms, diagnostic=True)
        self.add_features(config.monitorings, diagnostic=True)
        self.add_features(config.settings, diagnostic=True)
        self.add_features(config.commands, diagnostic=True)
        self.add_warnings(config.warnings)
        self.add_warnings(config.deviceWarnings)
        self.add_warnings(config.deviceWarningsExtra)


# -- C++ emission ------------------------------------------------------------

_KIND_NAME = {
    KIND_ENUM: "KIND_ENUM",
    KIND_NUMERIC: "KIND_NUMERIC",
    KIND_PROGRESS: "KIND_PROGRESS",
    KIND_FLAG: "KIND_FLAG",
}


def _c_str(s: str | None) -> str:
    if s is None:
        return "nullptr"
    out = s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return f'"{out}"'


def _c_ident(key: str) -> str:
    return "".join(c if c.isalnum() else "_" for c in key).upper()


def _c_float(x: float) -> str:
    # A C++ float literal needs a decimal point/exponent before the `f` suffix:
    # `0f`/`1f` are ill-formed, `0.0f`/`1.0f` are fine. JSON factors may be ints
    # (dacite doesn't coerce), so force float — repr(float) always keeps the dot
    # (or exponent), so it round-trips safely.
    return f"{float(x)!r}f"


def emit_header(walker: Walker, meta: dict) -> str:
    L: list[str] = []
    model = meta.get("model", "unknown")
    type_name = meta.get("applianceTypeName", "unknown")
    type_code = meta.get("applianceType", "?")
    brand = meta.get("brand", "unknown")

    L.append("// AUTO-GENERATED by generate_mapping.py — DO NOT EDIT.")
    L.append(f"// Source model : {model}")
    L.append(f"// Appliance    : {type_name} ({type_code})   brand: {brand}")
    L.append(f"// Fields       : {len(walker.fields)}   Write targets: {len(walker.writes)}")
    L.append("#pragma once")
    L.append("#include <cstdint>")
    L.append("")
    L.append("namespace esphome {")
    L.append("namespace homewhiz {")
    L.append("")
    L.append("enum FieldKind : uint8_t {")
    L.append("  KIND_ENUM     = 0,  // value = enum lookup of (byte & 0x7F)")
    L.append("  KIND_NUMERIC  = 1,  // value = (byte & 0x7F) * factor")
    L.append("  KIND_PROGRESS = 2,  // minutes = (b[index] & 0x7F)*60 + (b[index2] & 0x7F)")
    L.append("  KIND_FLAG     = 3,  // value = (b[index] >> index2) & 1")
    L.append("};")
    L.append("")
    L.append("struct EnumEntry { uint8_t value; const char *key; };")
    L.append("")
    L.append("struct FieldDesc {")
    L.append("  const char *key;")
    L.append("  const char *label;")
    L.append("  uint8_t kind;")
    L.append("  uint8_t index;")
    L.append("  uint8_t index2;    // minute index (PROGRESS) or bit index (FLAG)")
    L.append("  float   factor;    // NUMERIC only")
    L.append("  const EnumEntry *enums;")
    L.append("  uint8_t enum_count;")
    L.append("};")
    L.append("")
    L.append("struct WriteDesc { const char *key; uint8_t index; };")
    L.append("")

    # enum tables
    for f in walker.fields:
        if f.kind != KIND_ENUM:
            continue
        name = f"HW_ENUM_{_c_ident(f.key)}"
        L.append(f"static const EnumEntry {name}[] = {{")
        for e in f.enums:
            L.append(f"  {{{e.value}, {_c_str(e.key)}}},")
        L.append("};")
    L.append("")

    # field table
    L.append("static const FieldDesc HW_FIELDS[] = {")
    for f in walker.fields:
        if f.kind == KIND_ENUM:
            enums_ref = f"HW_ENUM_{_c_ident(f.key)}"
            enum_count = len(f.enums)
        else:
            enums_ref = "nullptr"
            enum_count = 0
        L.append(
            f"  {{{_c_str(f.key)}, {_c_str(f.label)}, {_KIND_NAME[f.kind]}, "
            f"{f.index}, {f.index2}, {_c_float(f.factor)}, {enums_ref}, {enum_count}}},"
        )
    L.append("};")
    L.append("static const uint8_t HW_FIELD_COUNT = "
             "sizeof(HW_FIELDS) / sizeof(HW_FIELDS[0]);")
    L.append("")

    # write table
    L.append("static const WriteDesc HW_WRITES[] = {")
    for w in walker.writes:
        L.append(f"  {{{_c_str(w.key)}, {w.index}}},")
    if not walker.writes:
        L.append("  {nullptr, 0},  // no writable targets in this config")
    L.append("};")
    if walker.writes:
        L.append("static const uint8_t HW_WRITE_COUNT = "
                 "sizeof(HW_WRITES) / sizeof(HW_WRITES[0]);")
    else:
        L.append("static const uint8_t HW_WRITE_COUNT = 0;")
    L.append("")
    L.append("}  // namespace homewhiz")
    L.append("}  // namespace esphome")
    L.append("")
    return "\n".join(L)


def _prettify(key: str) -> str:
    return key.replace("_", " ").title()


def emit_entities(walker: Walker, hub_id: str = "appliance") -> str:
    """One ESPHome entity per decodable field — the generic entity set.

    numeric/progress -> sensor, enum -> text_sensor, flag -> binary_sensor, with
    Home Assistant semantics attached (device_class / state_class /
    entity_category). Pull into a device-agnostic bridge yaml via `packages`, so
    any appliance is just "regenerate + include", no hand-wiring (plan §6-T-B3).
    """
    sensors: list[str] = []
    text_sensors: list[str] = []
    binary_sensors: list[str] = []

    for f in walker.fields:
        name = f.label or _prettify(f.key)
        entry = [
            "  - platform: homewhiz",
            f"    homewhiz_id: {hub_id}",
            f"    key: {f.key}",
            f'    name: "{name}"',
        ]
        if f.diagnostic:
            entry.append("    entity_category: diagnostic")

        if f.kind == KIND_PROGRESS:
            entry.append('    unit_of_measurement: "min"')
            entry.append("    device_class: duration")
            entry.append("    state_class: measurement")
            sensors.append("\n".join(entry))
        elif f.kind == KIND_NUMERIC:
            if f.unit:
                entry.append(f'    unit_of_measurement: "{f.unit}"')
            entry.append("    state_class: measurement")
            sensors.append("\n".join(entry))
        elif f.kind == KIND_FLAG:
            entry.append("    device_class: problem")
            binary_sensors.append("\n".join(entry))
        else:  # KIND_ENUM
            text_sensors.append("\n".join(entry))

    out = [
        "# AUTO-GENERATED by generate_mapping.py — DO NOT EDIT.",
        "# One entity per decodable field. Pull into a bridge yaml via:",
        "#   packages:",
        "#     entities: !include homewhiz_entities.yaml",
        "",
    ]
    for header, block in (
        ("text_sensor:", text_sensors),
        ("sensor:", sensors),
        ("binary_sensor:", binary_sensors),
    ):
        if block:
            out.append(header)
            out.extend(block)
            out.append("")
    return "\n".join(out)


def _build_walker(config_dict: dict, localization: dict[str, str]) -> tuple[Walker, dict]:
    meta = config_dict.pop("_meta", {}) if isinstance(config_dict, dict) else {}
    config = from_dict(
        ApplianceConfiguration, config_dict, config=Config(strict=False)
    )
    walker = Walker(localization)
    walker.walk(config)
    if not walker.fields:
        sys.exit("Generated an empty table — is this a valid appliance config?")
    return walker, meta


def generate(config_dict: dict, localization: dict[str, str]) -> str:
    walker, meta = _build_walker(config_dict, localization)
    return emit_header(walker, meta)


def generate_entities(
    config_dict: dict, localization: dict[str, str], hub_id: str = "appliance"
) -> str:
    walker, _ = _build_walker(config_dict, localization)
    return emit_entities(walker, hub_id)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("config", help="config.json from fetch_config.py")
    parser.add_argument("--localization", help="localization.json (optional)")
    parser.add_argument("--out", default="mapping.h", help="output header path")
    parser.add_argument(
        "--entities-out", help="also write an ESPHome entities include here"
    )
    args = parser.parse_args()

    with open(args.config, encoding="utf-8") as f:
        config_dict = json.load(f)
    localization: dict[str, str] = {}
    if args.localization:
        with open(args.localization, encoding="utf-8") as f:
            localization = json.load(f)

    # _build_walker pops _meta, so build entities from a copy first.
    if args.entities_out:
        entities = generate_entities(dict(config_dict), localization)
        with open(args.entities_out, "w", encoding="utf-8") as f:
            f.write(entities)
        print(f"Wrote {args.entities_out}", file=sys.stderr)

    header = generate(config_dict, localization)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(header)
    print(f"Wrote {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
