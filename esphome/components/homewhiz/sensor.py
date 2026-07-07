"""Numeric HomeWhiz entities (temperature, spin, remaining, delay, …).

Each sensor names a `key` from the generated mapping.h; the C++ resolves it
against the table at runtime, so the field set can differ per appliance with no
code changes (plan §6-T-B3).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor

from . import CONF_HOMEWHIZ_ID, HomeWhiz, homewhiz_ns

DEPENDENCIES = ["homewhiz"]

CONF_KEY = "key"

CONFIG_SCHEMA = sensor.sensor_schema().extend(
    {
        cv.GenerateID(CONF_HOMEWHIZ_ID): cv.use_id(HomeWhiz),
        cv.Required(CONF_KEY): cv.string,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HOMEWHIZ_ID])
    sens = await sensor.new_sensor(config)
    cg.add(parent.register_sensor(config[CONF_KEY], sens))
