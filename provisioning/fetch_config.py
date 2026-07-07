#!/usr/bin/env python3
"""Fetch a HomeWhiz appliance configuration and generate the ESPHome mapping.h.

One-time, host-side step. Credentials come from a ``.env`` file (or the
environment): ``HOMEWHIZ_USER`` / ``HOMEWHIZ_PASS``. When they're absent and the
script runs in a terminal it prompts for them interactively (password hidden).
They are used only here and never written to any artifact.

With several appliances on the account, an interactive terminal offers a
numbered menu; non-interactively, pass ``--model`` / ``--appliance-id``.

By default this fetches the config and writes ``mapping.h`` directly — there is
no ``config.json`` intermediate on disk. Pass ``--dump-config`` if you want to
inspect the raw config or regenerate offline with ``generate_mapping.py``.

The script is deliberately **not** washer-specific: it lists every appliance on
the account and lets you pick one by model or applianceId, so the same tool
serves any HomeWhiz BT appliance.
"""

from __future__ import annotations

import argparse
import asyncio
import dataclasses
import getpass
import json
import os
import sys

from dotenv import find_dotenv, load_dotenv

import generate_mapping
from vendor import api

# Default output: inside the component dir so ESPHome's external_components
# copies it (relative to provisioning/).
DEFAULT_MAPPING_OUT = "../esphome/components/homewhiz/mapping.h"
# Generic entity set, included by bridge.yaml via `packages`.
DEFAULT_ENTITIES_OUT = "../esphome/homewhiz_entities.yaml"

# Human-readable appliance types (from upstream homewhiz.py) — display only,
# never used to filter. See plan §4.
APPLIANCE_TYPES: dict[int, str] = {
    1: "WASHER",
    2: "REFRIGERATOR",
    3: "DISHWASHER",
    4: "OVEN",
    5: "DRYER",
    7: "HOB",
    8: "HOOD",
    9: "AIR_CONDITIONER",
    30: "DRYANDWASHER",
}
BRANDS: dict[int, str] = {2: "Grundig", 3: "Beko", 39: "Bauknecht"}


def _type_name(code: int) -> str:
    return APPLIANCE_TYPES.get(code, f"TYPE_{code}")


def _brand_name(code: int) -> str:
    return BRANDS.get(code, "Arçelik")


def _select(
    appliances: list[api.ApplianceInfo],
    *,
    model: str | None,
    appliance_id: str | None,
) -> api.ApplianceInfo:
    """Pick one appliance. No implicit washer preference — see plan §5/T-A2."""
    if appliance_id is not None:
        matches = [a for a in appliances if a.applianceId == appliance_id]
        if not matches:
            sys.exit(f"No appliance with applianceId={appliance_id!r}")
        return matches[0]
    if model is not None:
        matches = [a for a in appliances if a.model == model]
        if not matches:
            sys.exit(f"No appliance with model={model!r}")
        if len(matches) > 1:
            sys.exit(
                f"Multiple appliances match model={model!r}; "
                f"disambiguate with --appliance-id"
            )
        return matches[0]
    # Nothing specified: only auto-select when there is exactly one appliance.
    if len(appliances) == 1:
        return appliances[0]
    sys.exit(
        "Multiple appliances found — choose one with --model or --appliance-id "
        "(see the list above)."
    )


def _interactive() -> bool:
    return sys.stdin.isatty() and sys.stderr.isatty()


def _resolve_credentials(dotenv_path: str) -> tuple[str, str]:
    """Credentials from .env (with a note), else prompt, else fail.

    ``dotenv_path`` is the .env file that was loaded ("" if none).
    """
    username = os.environ.get("HOMEWHIZ_USER")
    password = os.environ.get("HOMEWHIZ_PASS")
    if username and password:
        if dotenv_path:
            print(f"Read credentials from {dotenv_path}", file=sys.stderr)
        return username, password
    # Something is missing — prompt if we can, otherwise fail with guidance.
    if _interactive():
        if not username:
            username = input("HomeWhiz username/email: ").strip()
        if not password:
            password = getpass.getpass("HomeWhiz password: ")
    if not username or not password:
        sys.exit(
            "Missing credentials. Create a .env (copy .env.example), or run in "
            "a terminal to be prompted."
        )
    return username, password


def _choose_interactively(appliances: list[api.ApplianceInfo]) -> str:
    """Numbered menu; returns the chosen applianceId."""
    print("Select an appliance:", file=sys.stderr)
    for i, a in enumerate(appliances, 1):
        print(
            f"  [{i}] {a.model}  {_type_name(a.applianceType)}  "
            f"({_brand_name(a.brand)})  applianceId={a.applianceId}",
            file=sys.stderr,
        )
    while True:
        raw = input(f"Number [1-{len(appliances)}]: ").strip()
        if raw.isdigit() and 1 <= int(raw) <= len(appliances):
            return appliances[int(raw) - 1].applianceId
        print("Invalid choice, try again.", file=sys.stderr)


def _print_appliances(appliances: list[api.ApplianceInfo]) -> None:
    print(f"\nDiscovered {len(appliances)} appliance(s):", file=sys.stderr)
    for a in appliances:
        bt = "BT" if a.is_bt() else a.connectivity
        print(
            f"  - {a.model:20} {_type_name(a.applianceType):16} "
            f"{_brand_name(a.brand):9} [{bt}]  applianceId={a.applianceId}",
            file=sys.stderr,
        )
    print("", file=sys.stderr)


async def _run(args: argparse.Namespace) -> None:
    dotenv_path = find_dotenv(usecwd=True)  # "" when there is no .env
    if dotenv_path:
        load_dotenv(dotenv_path)  # real env still wins over .env
    username, password = _resolve_credentials(dotenv_path)
    # CLI flags win; otherwise fall back to optional .env selectors.
    model = args.model or os.environ.get("HOMEWHIZ_MODEL")
    appliance_id = args.appliance_id or os.environ.get("HOMEWHIZ_APPLIANCE_ID")
    language = args.language or os.environ.get("HOMEWHIZ_LANGUAGE") or "en-GB"

    print("Logging in…", file=sys.stderr)
    try:
        credentials = await api.login(username, password)
    except api.LoginError:
        sys.exit("Login failed — check HOMEWHIZ_USER / HOMEWHIZ_PASS.")

    print("Fetching appliances…", file=sys.stderr)
    try:
        appliances = await api.fetch_appliance_infos(credentials)
    except api.RequestError as e:
        sys.exit(f"Failed to fetch appliances from HomeWhiz: {e}")
    if not appliances:
        sys.exit("No appliances found on this account.")
    _print_appliances(appliances)

    # Several appliances, nothing pre-selected, and we're at a terminal: offer a
    # menu instead of exiting. Non-interactive runs keep the explicit-flag path.
    if not model and not appliance_id and len(appliances) > 1 and _interactive():
        appliance_id = _choose_interactively(appliances)

    target = _select(appliances, model=model, appliance_id=appliance_id)
    print(
        f"Selected: {target.model} ({_type_name(target.applianceType)}) "
        f"applianceId={target.applianceId}",
        file=sys.stderr,
    )
    if not target.is_bt():
        print(
            f"WARNING: connectivity is {target.connectivity!r}; this bridge is "
            f"BLE-only.",
            file=sys.stderr,
        )

    try:
        contents = await api.fetch_appliance_contents(
            credentials, target.applianceId, language=language
        )
    except api.RequestError as e:
        sys.exit(f"Failed to fetch appliance config: {e}")

    config_dict = dataclasses.asdict(contents.config)
    # Attach provenance so the generator can stamp the header (no secrets here).
    config_dict["_meta"] = {
        "model": target.model,
        "applianceType": target.applianceType,
        "applianceTypeName": _type_name(target.applianceType),
        "brand": _brand_name(target.brand),
    }

    # Optional debug dump (off by default — no config.json intermediate).
    if args.dump_config:
        with open(args.dump_config, "w", encoding="utf-8") as f:
            json.dump(config_dict, f, indent=2, ensure_ascii=False)
        print(f"Wrote {args.dump_config}", file=sys.stderr)
    if args.dump_localization:
        with open(args.dump_localization, "w", encoding="utf-8") as f:
            json.dump(contents.localization, f, indent=2, ensure_ascii=False)
        print(f"Wrote {args.dump_localization}", file=sys.stderr)

    # Entities include first (generate() pops "_meta", so pass a copy).
    entities = generate_mapping.generate_entities(
        dict(config_dict), contents.localization
    )
    with open(args.entities_out, "w", encoding="utf-8") as f:
        f.write(entities)
    print(f"Wrote {args.entities_out}", file=sys.stderr)

    # Generate mapping.h directly. generate() pops "_meta" for the header comment.
    header = generate_mapping.generate(config_dict, contents.localization)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(header)
    print(
        f"Wrote {args.out}\nNext: cd ../esphome && esphome run bridge.yaml",
        file=sys.stderr,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model", help="Select appliance by model (else HOMEWHIZ_MODEL)"
    )
    parser.add_argument(
        "--appliance-id",
        help="Select appliance by applianceId (else HOMEWHIZ_APPLIANCE_ID)",
    )
    parser.add_argument(
        "--language", help="Localization language (else HOMEWHIZ_LANGUAGE, en-GB)"
    )
    parser.add_argument(
        "--out", default=DEFAULT_MAPPING_OUT, help="mapping.h output path"
    )
    parser.add_argument(
        "--entities-out",
        default=DEFAULT_ENTITIES_OUT,
        help="ESPHome entities include output path",
    )
    parser.add_argument(
        "--dump-config", metavar="PATH", help="Also write raw config JSON (debug)"
    )
    parser.add_argument(
        "--dump-localization", metavar="PATH", help="Also write localization JSON"
    )
    asyncio.run(_run(parser.parse_args()))


if __name__ == "__main__":
    main()
