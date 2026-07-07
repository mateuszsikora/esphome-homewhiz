"""Enum/flag HomeWhiz entities (state, program, warnings, …).

Each text_sensor names a `key` from the generated mapping.h; resolved at runtime
against the table (plan §6-T-B3).
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor

from . import CONF_HOMEWHIZ_ID, HomeWhiz, homewhiz_ns

DEPENDENCIES = ["homewhiz"]

CONF_KEY = "key"

CONFIG_SCHEMA = text_sensor.text_sensor_schema().extend(
    {
        cv.GenerateID(CONF_HOMEWHIZ_ID): cv.use_id(HomeWhiz),
        cv.Required(CONF_KEY): cv.string,
    }
)


async def to_code(config):
    parent = await cg.get_variable(config[CONF_HOMEWHIZ_ID])
    sens = await text_sensor.new_text_sensor(config)
    cg.add(parent.register_text_sensor(config[CONF_KEY], sens))
