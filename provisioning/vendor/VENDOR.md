# Vendored upstream

`api.py` and `appliance_config.py` are copied verbatim from the
[home-assistant-HomeWhiz](https://github.com/home-assistant-HomeWhiz/home-assistant-HomeWhiz)
project (MIT — see `LICENSE.upstream`).

- Source path: `custom_components/homewhiz/{api.py,appliance_config.py}`
- Copied from commit: `225186711b1779939fdd15e6f57f53cf46d0c719` (2026-11-19)

## Updating

```sh
git clone --depth 1 https://github.com/home-assistant-HomeWhiz/home-assistant-HomeWhiz.git /tmp/hw
cp /tmp/hw/custom_components/homewhiz/api.py provisioning/vendor/api.py
cp /tmp/hw/custom_components/homewhiz/appliance_config.py provisioning/vendor/appliance_config.py
# then bump the commit hash above and re-run the tests
```

The files are used unmodified; `api.py` imports only stdlib + `aiohttp` +
`dacite` + the sibling `appliance_config`, with no Home Assistant dependency.
