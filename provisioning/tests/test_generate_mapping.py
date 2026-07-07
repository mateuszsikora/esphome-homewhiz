"""Tests for the config-driven mapping.h generator.

Proves two things:
1. For the reference washer, the generated table has the verified offsets.
2. The generator is *not* washer-bound: pointed at other appliance types'
   configs it still emits a valid, non-empty, compilable header with no code
   changes.
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest

import generate_mapping as gm

FIXTURES = Path(__file__).parent / "fixtures"
CXX = shutil.which("g++") or shutil.which("clang++")


def _generate(fixture_name: str) -> str:
    with open(FIXTURES / fixture_name, encoding="utf-8") as f:
        config = json.load(f)
    return gm.generate(config, {})


def _field_line(header: str, key: str) -> str | None:
    m = re.search(rf'^\s*\{{"{re.escape(key)}",.*$', header, re.MULTILINE)
    return m.group(0) if m else None


def _compiles(header: str, tmp_path: Path) -> tuple[bool, str]:
    path = tmp_path / "mapping.h"
    path.write_text(header, encoding="utf-8")
    proc = subprocess.run(
        [CXX, "-x", "c++", "-std=c++11", "-fsyntax-only", "-"],
        input=f'#include "{path}"\n',
        text=True,
        capture_output=True,
    )
    return proc.returncode == 0, proc.stderr


# -- reference washer --------------------------------------------------------

def test_washer_has_verified_offsets():
    header = _generate("example_washing_machine_config.json")

    state = _field_line(header, "STATE")
    assert state is not None, "washer must expose a STATE field"
    assert "KIND_ENUM, 35," in state, f"STATE must be enum @ index 35: {state}"

    program = _field_line(header, "WASHER_PROGRAM")
    assert program is not None, "washer must expose a program field"
    assert ", 36," in program, f"program must be @ index 36: {program}"

    # progress pairs (hour, minute) — verified in plan §3.5
    remaining = _field_line(header, "WASHER_REMAINING")
    assert remaining is not None and "KIND_PROGRESS, 46, 47," in remaining

    # writable state target (start/pause), not program — see write-path finding
    assert re.search(r'^\s*\{"STATE", 34\},', header, re.MULTILINE), \
        "washer must expose STATE write target @ index 34"


@pytest.mark.skipif(CXX is None, reason="no C++ compiler available")
def test_washer_header_compiles(tmp_path):
    ok, err = _compiles(_generate("example_washing_machine_config.json"), tmp_path)
    assert ok, err


# -- generality: other appliance types, no code changes ----------------------

OTHER_TYPES = [
    "example_dishwasher_config.json",
    "arcelik-dryer.json",
    "example_oven_config.json",
]


@pytest.mark.parametrize("fixture", OTHER_TYPES)
def test_other_types_produce_nonempty_table(fixture):
    header = _generate(fixture)
    field_count = header.count("KIND_ENUM,") + header.count("KIND_NUMERIC,") \
        + header.count("KIND_PROGRESS,") + header.count("KIND_FLAG,")
    assert field_count > 0, f"{fixture} produced an empty table"


@pytest.mark.skipif(CXX is None, reason="no C++ compiler available")
@pytest.mark.parametrize("fixture", OTHER_TYPES)
def test_other_types_compile(fixture, tmp_path):
    ok, err = _compiles(_generate(fixture), tmp_path)
    assert ok, err


# -- generic entity include -------------------------------------------------

def _entities(fixture_name: str) -> str:
    with open(FIXTURES / fixture_name, encoding="utf-8") as f:
        config = json.load(f)
    return gm.generate_entities(config, {})


def test_washer_entities_classification():
    y = _entities("example_washing_machine_config.json")
    assert "text_sensor:" in y and "sensor:" in y and "binary_sensor:" in y
    # enum -> text_sensor
    assert "key: STATE" in y
    # progress -> sensor with HA duration semantics
    assert "key: WASHER_REMAINING" in y
    assert 'unit_of_measurement: "min"' in y
    assert "device_class: duration" in y
    assert "state_class: measurement" in y
    # flags -> binary_sensor with device_class: problem
    assert "device_class: problem" in y
    # settings/warnings are diagnostic
    assert "entity_category: diagnostic" in y
    # every entity is wired to the generic hub id
    assert "homewhiz_id: appliance" in y


@pytest.mark.parametrize("fixture", OTHER_TYPES)
def test_other_types_emit_entities(fixture):
    y = _entities(fixture)
    assert y.count("platform: homewhiz") > 0
