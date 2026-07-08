"""HomeWhiz BLE bridge — ESPHome external component (hub).

Table-driven: all appliance semantics live in the generated ``mapping.h`` next to
this file. Regenerate that header (provisioning/) to support a different
appliance; no code here changes.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, binary_sensor
from esphome.const import CONF_ID, DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

CODEOWNERS = ["@you"]
DEPENDENCIES = ["ble_client", "esp32"]
# The optional `connected:` status entity is a binary_sensor, so make sure the
# core binary_sensor component is compiled in even when the config has no
# `binary_sensor:` platform block of its own.
AUTO_LOAD = ["binary_sensor"]
MULTI_CONF = True

CONF_SERVICE_UUID = "service_uuid"
CONF_HOMEWHIZ_ID = "homewhiz_id"
CONF_CONNECTED = "connected"

homewhiz_ns = cg.esphome_ns.namespace("homewhiz")
HomeWhiz = homewhiz_ns.class_("HomeWhiz", ble_client.BLEClientNode, cg.Component)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeWhiz),
            # Model-specific; the only runtime unknown (plan §3.1 / T-B2).
            # Discover it once from the ble_client GATT dump or nRF Connect.
            cv.Required(CONF_SERVICE_UUID): cv.string,
            # Optional hub-level connectivity status entity. Reflects the BLE
            # link (connected + handshaken), so HA can tell "appliance off / out
            # of range" from live data.
            cv.Optional(CONF_CONNECTED): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_service_uuid(config[CONF_SERVICE_UUID]))
    if CONF_CONNECTED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_CONNECTED])
        cg.add(var.set_connected_binary_sensor(sens))
