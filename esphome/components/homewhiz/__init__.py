"""HomeWhiz BLE bridge — ESPHome external component (hub).

Table-driven: all appliance semantics live in the generated ``mapping.h`` next to
this file. Regenerate that header (provisioning/) to support a different
appliance; no code here changes.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

CODEOWNERS = ["@you"]
DEPENDENCIES = ["ble_client", "esp32"]
MULTI_CONF = True

CONF_SERVICE_UUID = "service_uuid"
CONF_HOMEWHIZ_ID = "homewhiz_id"

homewhiz_ns = cg.esphome_ns.namespace("homewhiz")
HomeWhiz = homewhiz_ns.class_("HomeWhiz", ble_client.BLEClientNode, cg.Component)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HomeWhiz),
            # Model-specific; the only runtime unknown (plan §3.1 / T-B2).
            # Discover it once from the ble_client GATT dump or nRF Connect.
            cv.Required(CONF_SERVICE_UUID): cv.string,
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
