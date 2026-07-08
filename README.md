# esphome-homewhiz

A **local, cloud-free bridge** from HomeWhiz appliances (Beko / Grundig /
Arçelik) to Home Assistant, running as an [ESPHome](https://esphome.io) external
component on an ESP32 over Bluetooth LE.

No HomeWhiz cloud or phone app at runtime. You log into the cloud **once**, on
your own machine, to download the appliance's data model; after that the ESP32
talks only to the appliance over BLE and only to Home Assistant over your LAN.

> Validated end-to-end on a Beko EWUE7636XAW washing machine: the ESP32 connects,
> handshakes, and publishes 24 entities (state, program, temperature, remaining
> time, warning flags, …) to Home Assistant.

## Highlights

- **Fully local** — after provisioning, zero internet dependency.
- **Config-driven / appliance-agnostic** — the decoder is a table generated from
  the appliance's own configuration. Point the provisioning tool at a washer,
  dryer, dishwasher, oven, … and you get a working entity set with **no code or
  YAML changes** — just regenerate and reflash.
- **Credentials never leave your machine** — used once during provisioning, never
  written to any artifact, never placed on the ESP.
- **Self-discovering** — the firmware logs nearby BLE devices until it connects,
  so you can find your appliance's MAC without a phone.

## How it works

```
┌────────────────────┐        ┌──────────────────────────┐        ┌───────────────┐
│  Provisioning       │  once  │  ESP32 (ESPHome)          │  BLE   │  HomeWhiz      │
│  (your computer)    │        │  homewhiz component       │◄──────►│  appliance     │
│                     │        │                           │        └───────────────┘
│  login → fetch      │        │  handshake, reassemble,   │
│  config → generate: │  ───▶  │  decode via mapping.h,    │  API   ┌───────────────┐
│  · mapping.h        │  files │  publish entities         │◄──────►│ Home Assistant│
│  · entities.yaml    │        │                           │  LAN   └───────────────┘
└────────────────────┘        └──────────────────────────┘
```

1. **Provisioning (once, on your computer).** `provisioning/` logs into the
   HomeWhiz cloud, downloads the appliance's `ApplianceConfiguration` (byte
   offsets, enum tables, factors — all appliance data comes from the cloud,
   nothing is hardcoded), and generates two artifacts:
   - `esphome/components/homewhiz/mapping.h` — the decode table
   - `esphome/homewhiz_entities.yaml` — one Home Assistant entity per field
2. **Bridge (on the ESP32).** The `homewhiz` component connects over BLE,
   performs the handshake, reassembles the state frame, and decodes it by
   **iterating the generated table** — there are no appliance field names or byte
   offsets in the C++. It publishes the decoded values as ESPHome entities.

## Repository layout

```
provisioning/   host-side: fetch config -> mapping.h + homewhiz_entities.yaml
esphome/        ESPHome external component + generic bridge.yaml
tests/          host-side C++ decode test (no hardware needed)
```

## Requirements

- An **ESP32** board (tested on `esp32dev`). BLE + Wi-Fi coexistence on a single
  radio can be marginal through a metal appliance enclosure — a board with an
  external antenna, placed close to the appliance, is recommended.
- [ESPHome](https://esphome.io) **2025.3.1** (esp-idf framework). See
  [ESPHome version](#esphome-version).
- [uv](https://github.com/astral-sh/uv) for the provisioning Python project
  (or any Python 3.12 environment).
- A HomeWhiz account with the appliance already paired in the phone app.

## Getting started

### 1. Provision (download the appliance model)

```sh
cd provisioning
uv sync
cp .env.example .env          # then fill in HOMEWHIZ_USER / HOMEWHIZ_PASS
uv run python fetch_config.py
```

`fetch_config.py` lists every appliance on your account and, unless you pass
`--model` / `--appliance-id`, prompts you to pick one (or auto-selects if there's
exactly one). It writes both generated artifacts and prints where. Credentials
come from `.env` (or the environment); if `.env` is missing and you're in a
terminal, it prompts.

### 2. Configure secrets

```sh
cd ../esphome
cp secrets.yaml.example secrets.yaml   # then edit
```

Set `wifi_ssid` / `wifi_password`. Leave `appliance_mac` as the placeholder for
now — you'll fill it in step 3. The static-IP entries are optional (see
[Static IP](#static-ip-cross-vlan)).

### 3. Find the appliance's BLE MAC and flash

The first flash must go over **USB serial** (use a data-capable cable):

```sh
esphome run bridge.yaml --device /dev/cu.usbserial-XXXX   # or /dev/ttyUSB0 on Linux
```

While the ESP isn't connected to the appliance it logs nearby devices:

```
[scan] Nearby BLE  00:A0:50:AA:BB:CC  RSSI=-70  name='HwZ_...'
```

HomeWhiz appliances advertise a name starting **`HwZ_`**. Put that MAC into
`secrets.yaml` as `appliance_mac`, then reflash. Once the correct MAC connects,
the scan logging goes quiet automatically.

> **One BLE link only.** The appliance accepts a single BLE connection — make
> sure the HomeWhiz phone app is disconnected, or the ESP won't be able to
> connect.

### 4. Add to Home Assistant

The device exposes the ESPHome API. In Home Assistant, add the **ESPHome**
integration pointing at the device's IP (or `homewhiz-bridge.local` if mDNS
works on your network). All generated entities appear automatically.

## Configuration

`bridge.yaml` is appliance-agnostic — the entity set comes from the generated
`homewhiz_entities.yaml` (pulled in via ESPHome `packages`) and the decode from
`mapping.h`. To support a different appliance: re-run provisioning and reflash;
no edits to `bridge.yaml`.

### Service UUID

The BLE characteristics are the same across all HomeWhiz appliances (notify
`0xAC02`, write `0xAC01`), but the **service** they live under is model-specific.
`bridge.yaml` ships with `0000ac00-0000-1000-8000-00805f9b34fb`, confirmed on the
Beko washer. If yours differs, the `ble_client` GATT dump in the DEBUG logs lists
every service and its characteristics — pick the one containing `AC01` + `AC02`.

### Static IP (cross-VLAN)

If Home Assistant or your dev machine is on a **different VLAN** than the ESP,
mDNS (`.local` resolution) won't work — multicast doesn't cross VLANs. Set a
static IP so OTA and log streaming reach the device by unicast: fill `static_ip`
/ `gateway` / `subnet` in `secrets.yaml`. For a single-VLAN network you can
delete the `manual_ip:` block from `bridge.yaml` and use DHCP.

## ESPHome version

Pinned target: **ESPHome 2025.3.1** (esp-idf). `esphome compile bridge.yaml`
builds cleanly against it (~68% flash on `esp32dev`). The BLE GATT accessor names
(`get_gattc_if`, `get_conn_id`, `get_remote_bda`, `get_characteristic`) have
drifted across ESPHome releases; if the build fails on a different version, adjust
those names — the decode logic is unchanged.

## Known limitations

- **Spin/temperature are sparse enums, not raw numbers.** Some fields (notably
  `WASHER_SPIN`) are modelled in the config as an enum with only a few landmark
  values (`no_spin`, `rinse_hold`), even though the byte is really numeric. For
  an unrecognised value the text_sensor falls back to publishing the raw number
  (e.g. `"12"`) rather than going stale. To expose it as a proper numeric sensor,
  add a `sensor` for the same key with a `factor` — the reading becomes
  `(raw byte) * factor`:

  ```yaml
  sensor:
    - platform: homewhiz
      homewhiz_id: appliance
      key: WASHER_SPIN
      name: "Spin speed"
      factor: 100          # byte 12 -> 1200 rpm
      unit_of_measurement: rpm
  ```
- **Write path (start/pause) is a stretch goal.** `program.wfaWriteIndex` is null
  in every known appliance config; the writable target is the device-state index
  (`deviceStates.wifiArrayWriteIndex`, 34 on the washer). `send_command` is
  scaffolded but not wired to entities by default — validate carefully on device.
- **One appliance per ESP (by design).** The component decodes against a single
  generated table, and appliances in different rooms are out of one ESP's BLE
  range anyway — so one ESP per appliance is the intended model, not a bug.
- **Air conditioners (roadmap).** AC units map onto Home Assistant's `climate`
  domain rather than `sensor`/`text_sensor`, so they aren't covered by the generic
  entity set yet. A `climate` platform would be a natural follow-up.

## Development

```sh
# generator + provisioning logic (no hardware)
cd provisioning && uv run pytest

# C++ decode + fragment reassembly against a known-good frame (no hardware)
bash tests/run.sh
```

The decode core (`esphome/components/homewhiz/decode.h`) is deliberately free of
any ESPHome/ESP-IDF dependency so it can be unit-tested on the host.

## Security

- HomeWhiz credentials are used **only** during provisioning, on your machine,
  read from `.env` or a prompt. They are never written to any generated file,
  committed, or placed on the ESP.
- `secrets.yaml` and `.env` are git-ignored; `*.example` templates are provided.
- The ESP holds only the anonymous offset/enum table — no account data.

## Credits

- Protocol facts, auth flow, and the vendored `api.py` / `appliance_config.py`
  come from the excellent
  [home-assistant-HomeWhiz](https://github.com/home-assistant-HomeWhiz/home-assistant-HomeWhiz)
  project (MIT).

## License

[MIT](LICENSE). Vendored upstream files are MIT — see
`provisioning/vendor/LICENSE.upstream`.
