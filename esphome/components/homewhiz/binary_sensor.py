"""Flag/warning HomeWhiz entities (door open, no water, safety, …).

Each binary_sensor names a `key` from the generated mapping.h whose field is a
bit flag; resolved at runtime against the table (plan §6-T-B3).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import CONF_HOMEWHIZ_ID, HomeWhiz, homewhiz_ns

DEPENDENCIES = ["homewhiz"]

CONF_KEY = "key"

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.GenerateID(CONF_HOMEWHIZ_ID): cv.use_id(HomeWhiz),
        cv.Required(CONF_KEY): cv.string,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HOMEWHIZ_ID])
    sens = await binary_sensor.new_binary_sensor(config)
    cg.add(parent.register_binary_sensor(config[CONF_KEY], sens))
