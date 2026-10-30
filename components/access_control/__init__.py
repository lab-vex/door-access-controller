"""Access control: user profiles (cards, PIN, face, palm) for a door panel.

Drives an FP008 / DFRobot FP-series face + palm vein module over UART
(EF AA protocol, 115200 baud) and serves a profile management page at
http://<device>/ .
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import socket, uart, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@cody"]
DEPENDENCIES = ["uart", "network"]
AUTO_LOAD = ["web_server_base"]

CONF_PRESENCE_PIN = "presence_pin"
CONF_IDENTIFY_TIMEOUT = "identify_timeout"
CONF_LED_FEEDBACK = "led_feedback"
CONF_ENROL_TIMEOUT = "enrol_timeout"
CONF_ON_ACCESS = "on_access"

access_control_ns = cg.esphome_ns.namespace("access_control")
AccessControl = access_control_ns.class_(
    "AccessControl", cg.Component, uart.UARTDevice
)
AccessTrigger = access_control_ns.class_(
    "AccessTrigger",
    automation.Trigger.template(cg.std_string, cg.std_string, cg.bool_),
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AccessControl),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.Optional(CONF_PRESENCE_PIN): pins.gpio_input_pin_schema,
            cv.Optional(CONF_IDENTIFY_TIMEOUT, default="5s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=1), max=cv.TimePeriod(seconds=20)),
            ),
            cv.Optional(CONF_ENROL_TIMEOUT, default="20s"): cv.All(
                cv.positive_time_period_seconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(seconds=60)),
            ),
            cv.Optional(CONF_LED_FEEDBACK, default=True): cv.boolean,
            cv.Optional(CONF_ON_ACCESS): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(AccessTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA),
    # Browser connections to the profile page
    socket.consume_sockets(3, "access_control"),
)

FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "access_control", baud_rate=115200, require_tx=True, require_rx=True
)


async def to_code(config):
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    if CONF_PRESENCE_PIN in config:
        pin = await cg.gpio_pin_expression(config[CONF_PRESENCE_PIN])
        cg.add(var.set_presence_pin(pin))
    cg.add(var.set_identify_timeout(config[CONF_IDENTIFY_TIMEOUT].total_seconds))
    cg.add(var.set_enrol_timeout(config[CONF_ENROL_TIMEOUT].total_seconds))
    cg.add(var.set_led_feedback(config[CONF_LED_FEEDBACK]))

    for conf in config.get(CONF_ON_ACCESS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.std_string, "method"), (cg.std_string, "name"), (cg.bool_, "granted")],
            conf,
        )
