"""Paradox Spectra (SP-series) passive serial decoder for ESPHome.

Decodes the panel's unsolicited 37-byte LiveEvent frames (command nibble 0xE) on
the serial/PRG header and exposes zones to Home Assistant over the native API — no
PAI container and no MQTT needed. Passive and read-only: it never transmits, so it
is safe to tap a bus that another client (e.g. PAI) already drives. Frame layout
derived from the open-source Paradox Alarm Interface (ParadoxAlarmInterface/pai),
spectra_magellan. See the README for the protocol notes.

Config is samsung_ac-style: zones are a nested list under the hub (not a separate
binary_sensor platform), so the whole panel lives in one block.
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, text_sensor, uart
from esphome.const import CONF_ID

CODEOWNERS = ["@romapi"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "text_sensor"]

paradox_spectra_ns = cg.esphome_ns.namespace("paradox_spectra")
ParadoxSpectra = paradox_spectra_ns.class_(
    "ParadoxSpectra", cg.Component, uart.UARTDevice
)

CONF_DEBUG_FRAMES = "debug_frames"
CONF_LOG_ALL_EVENTS = "log_all_events"
CONF_LAST_EVENT = "last_event"
CONF_ZONES = "zones"
CONF_ZONE = "zone"
CONF_TYPE = "type"

# Each zone entry is a full binary_sensor (name, device_class, id, filters, ...) plus
# the panel zone number and a type: open (PIR/contact, event major 0/1) | fire (37/39).
ZONE_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=32),
        cv.Optional(CONF_TYPE, default="open"): cv.one_of("open", "fire", lower=True),
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ParadoxSpectra),
            # Log every validated frame as hex — handy for first flash / zone discovery.
            cv.Optional(CONF_DEBUG_FRAMES, default=False): cv.boolean,
            # Log EVERY decoded event (time + name + label) at INFO, not just mapped ones.
            cv.Optional(CONF_LOG_ALL_EVENTS, default=False): cv.boolean,
            # Optional text_sensor publishing the last decoded event (zone discovery).
            cv.Optional(CONF_LAST_EVENT): text_sensor.text_sensor_schema(),
            cv.Optional(CONF_ZONES, default=[]): cv.ensure_list(ZONE_SCHEMA),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

# Validate the UART at config time (replaces the deprecated C++ check_uart_settings):
# 9600 baud, RX required, TX not needed (passive, read-only tap).
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "paradox_spectra", baud_rate=9600, require_rx=True, require_tx=False
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add(var.set_debug_frames(config[CONF_DEBUG_FRAMES]))
    cg.add(var.set_log_all_events(config[CONF_LOG_ALL_EVENTS]))

    if CONF_LAST_EVENT in config:
        ts = await text_sensor.new_text_sensor(config[CONF_LAST_EVENT])
        cg.add(var.set_last_event(ts))

    for z in config[CONF_ZONES]:
        bs = await binary_sensor.new_binary_sensor(z)
        if z[CONF_TYPE] == "fire":
            cg.add(var.register_zone_fire(z[CONF_ZONE], bs))
        else:
            cg.add(var.register_zone_open(z[CONF_ZONE], bs))
