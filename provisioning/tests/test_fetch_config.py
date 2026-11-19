"""Offline tests for fetch_config's appliance-selection logic.

The network calls (login/fetch) need real credentials, so we test the pure
selection logic — the part that decides *which* appliance to provision — which
must never silently assume a washer.
"""

from __future__ import annotations

import argparse
import asyncio

import pytest

import fetch_config
from vendor import api


def _appliance(model: str, appliance_id: str, appliance_type: int = 1) -> api.ApplianceInfo:
    return api.ApplianceInfo(
        id=1,
        applianceId=appliance_id,
        brand=3,
        model=model,
        applianceType=appliance_type,
        platformType="BT",
        applianceSerialNumber=None,
        name=model,
        hsmId=None,
        connectivity="BT",
    )


WASHER = _appliance("EWUE7636XAW", "id-washer", 1)
DRYER = _appliance("DRYER123", "id-dryer", 5)
DUP = _appliance("EWUE7636XAW", "id-washer-2", 1)


def test_select_by_appliance_id_wins():
    got = fetch_config._select([WASHER, DRYER], model=None, appliance_id="id-dryer")
    assert got is DRYER


def test_select_by_model():
    got = fetch_config._select([WASHER, DRYER], model="DRYER123", appliance_id=None)
    assert got is DRYER


def test_single_appliance_auto_selected():
    got = fetch_config._select([DRYER], model=None, appliance_id=None)
    assert got is DRYER


def test_ambiguous_requires_choice():
    # Multiple appliances, nothing specified -> must not guess (no washer bias).
    with pytest.raises(SystemExit):
        fetch_config._select([WASHER, DRYER], model=None, appliance_id=None)


def test_duplicate_model_needs_appliance_id():
    with pytest.raises(SystemExit):
        fetch_config._select([WASHER, DUP], model="EWUE7636XAW", appliance_id=None)


def test_unknown_selectors_exit():
    with pytest.raises(SystemExit):
        fetch_config._select([WASHER], model=None, appliance_id="nope")
    with pytest.raises(SystemExit):
        fetch_config._select([WASHER], model="nope", appliance_id=None)


def test_choose_interactively_returns_selected_id(monkeypatch):
    answers = iter(["9", "bad", "2"])  # out-of-range, non-numeric, then valid
    monkeypatch.setattr("builtins.input", lambda _="": next(answers))
    got = fetch_config._choose_interactively([WASHER, DRYER])
    assert got == DRYER.applianceId


def test_resolve_credentials_from_env(monkeypatch):
    monkeypatch.setenv("HOMEWHIZ_USER", "u@e.co")
    monkeypatch.setenv("HOMEWHIZ_PASS", "pw")
    assert fetch_config._resolve_credentials("") == ("u@e.co", "pw")


def test_resolve_credentials_from_dotenv_announces(monkeypatch, capsys):
    monkeypatch.setenv("HOMEWHIZ_USER", "u@e.co")
    monkeypatch.setenv("HOMEWHIZ_PASS", "pw")
    assert fetch_config._resolve_credentials("/path/.env") == ("u@e.co", "pw")
    assert "Read credentials from /path/.env" in capsys.readouterr().err


def test_resolve_credentials_non_interactive_exits(monkeypatch):
    monkeypatch.delenv("HOMEWHIZ_USER", raising=False)
    monkeypatch.delenv("HOMEWHIZ_PASS", raising=False)
    monkeypatch.setattr(fetch_config, "_interactive", lambda: False)
    with pytest.raises(SystemExit):
        fetch_config._resolve_credentials("")


def test_missing_configuration_exits_cleanly(monkeypatch):
    # HomeWhiz can list an appliance but have no CONFIGURATION content for it;
    # that must be a clear exit, not a traceback.
    monkeypatch.setattr(fetch_config, "find_dotenv", lambda **_: "")
    monkeypatch.setattr(fetch_config, "_interactive", lambda: False)
    monkeypatch.setenv("HOMEWHIZ_USER", "u@e.co")
    monkeypatch.setenv("HOMEWHIZ_PASS", "pw")

    async def login(*_):
        return object()

    async def fetch_infos(*_):
        return [WASHER]

    async def fetch_contents(*_, **__):
        raise api.NoConfigurationError(WASHER.applianceId)

    monkeypatch.setattr(api, "login", login)
    monkeypatch.setattr(api, "fetch_appliance_infos", fetch_infos)
    monkeypatch.setattr(api, "fetch_appliance_contents", fetch_contents)

    args = argparse.Namespace(
        model=None,
        appliance_id=None,
        language=None,
        out="unused.h",
        entities_out="unused.yaml",
        dump_config=None,
        dump_localization=None,
    )
    with pytest.raises(SystemExit, match="no CONFIGURATION for EWUE7636XAW"):
        asyncio.run(fetch_config._run(args))
