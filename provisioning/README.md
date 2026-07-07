# provisioning

One-time, host-side provisioning for the HomeWhiz → ESPHome bridge.

It logs into the HomeWhiz cloud, downloads the appliance's `ApplianceConfiguration`
(byte offsets, enum tables, factors — all appliance-specific data comes from the
cloud, nothing is hardcoded), and emits a self-contained `esphome/mapping.h` that
the ESP32 component decodes with. **After this runs, the bridge is fully local** —
the ESP never talks to the cloud and never holds your credentials.

## Setup

```sh
cd provisioning
uv sync
cp .env.example .env      # then fill in HOMEWHIZ_USER / HOMEWHIZ_PASS
```

Credentials live in `.env` (git-ignored), are read only here, and are **never**
written to any artifact. If `.env` is absent and you run in a terminal, the
script prompts for username/password (password hidden). With several appliances
on the account it shows a numbered menu; script it with `--model` /
`--appliance-id` (or `HOMEWHIZ_MODEL` / `HOMEWHIZ_APPLIANCE_ID`) when
non-interactive.

## Fetch + generate (one step)

```sh
uv run python fetch_config.py                       # lists appliances, generates mapping.h
uv run python fetch_config.py --model EWUE7636XAW   # or set HOMEWHIZ_MODEL in .env
uv run python fetch_config.py --appliance-id <id>
```

This fetches the config and writes `../esphome/components/homewhiz/mapping.h`
directly — **no `config.json` intermediate on disk**. The generator walks the
config **generically** (never looks up washer field names), so pointing it at
any HomeWhiz BT appliance produces a valid `mapping.h` with no code changes.

Selection can also come from `.env` (`HOMEWHIZ_MODEL` / `HOMEWHIZ_APPLIANCE_ID`);
CLI flags win. With exactly one appliance on the account, nothing is needed.

### Inspect / regenerate offline (optional)

```sh
uv run python fetch_config.py --dump-config config.json --dump-localization localization.json
uv run python generate_mapping.py config.json --localization localization.json \
    --out ../esphome/components/homewhiz/mapping.h
```

`generate_mapping.py` stays a standalone tool — it's what the tests run against
saved fixtures.

## Tests

```sh
uv run pytest
```

Runs the generator against saved upstream fixtures (washer + a second appliance
type) and syntax-checks the generated header with `g++`.

---

Vendored under `vendor/`: `api.py` and `appliance_config.py` from
[home-assistant-HomeWhiz](https://github.com/home-assistant-HomeWhiz/home-assistant-HomeWhiz)
(MIT — see `vendor/LICENSE.upstream`).
