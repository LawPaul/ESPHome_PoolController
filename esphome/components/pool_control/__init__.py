import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID

CODEOWNERS = ["@LawPaul"]

pool_control_ns = cg.esphome_ns.namespace("pool_control")
PoolControl = pool_control_ns.class_("PoolControl", cg.Component)

CONF_OUTPUT_NUMBER = "output_number"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PoolControl),
        cv.Required(CONF_OUTPUT_NUMBER): cv.use_id(number.Number),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    num = await cg.get_variable(config[CONF_OUTPUT_NUMBER])
    cg.add(var.set_output_number(num))
