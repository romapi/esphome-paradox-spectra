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
from esphome.components import binary_sensor, sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_VOLT,
)

CODEOWNERS = ["@romapi"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["binary_sensor", "text_sensor", "sensor"]

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
# Active poll (optional): login + RAM read for current state + voltages. Needs UART TX
# wired. Read-only by capability — no control opcodes exist in the component.
CONF_PC_PASSWORD = "pc_password"
CONF_POLL_INTERVAL = "poll_interval"
CONF_SUPPLY_VOLTAGE = "supply_voltage"      # VDC rail (~16.7 V)
CONF_BATTERY_VOLTAGE = "battery_voltage"
CONF_DC_VOLTAGE = "dc_voltage"
CONF_RF_NOISE = "rf_noise"


def _voltage_sensor():
    return sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    )

# Each zone entry is a full binary_sensor (name, device_class, id, filters, ...) plus
# the panel zone number and a type: open (PIR/contact, event major 0/1) | fire (37/39).
ZONE_SCHEMA = binary_sensor.binary_sensor_schema().extend(
    {
        cv.Required(CONF_ZONE): cv.int_range(min=1, max=32),
        cv.Optional(CONF_TYPE, default="open"): cv.one_of(
            "open", "fire", "tamper", lower=True
        ),
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
            # --- Active poll (optional). Omit poll_interval (or set 0s) = passive. ---
            cv.Optional(CONF_PC_PASSWORD, default="0000"): cv.string,
            cv.Optional(CONF_POLL_INTERVAL, default="0s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SUPPLY_VOLTAGE): _voltage_sensor(),
            cv.Optional(CONF_BATTERY_VOLTAGE): _voltage_sensor(),
            cv.Optional(CONF_DC_VOLTAGE): _voltage_sensor(),
            cv.Optional(CONF_RF_NOISE): sensor.sensor_schema(
                accuracy_decimals=0, state_class=STATE_CLASS_MEASUREMENT
            ),
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
    cg.add(var.set_pc_password(config[CONF_PC_PASSWORD]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))

    if CONF_LAST_EVENT in config:
        ts = await text_sensor.new_text_sensor(config[CONF_LAST_EVENT])
        cg.add(var.set_last_event(ts))
    for key, setter in (
        (CONF_SUPPLY_VOLTAGE, var.set_supply_voltage),
        (CONF_BATTERY_VOLTAGE, var.set_battery_voltage),
        (CONF_DC_VOLTAGE, var.set_dc_voltage),
        (CONF_RF_NOISE, var.set_rf_noise),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(setter(s))

    for z in config[CONF_ZONES]:
        bs = await binary_sensor.new_binary_sensor(z)
        if z[CONF_TYPE] == "fire":
            cg.add(var.register_zone_fire(z[CONF_ZONE], bs))
        elif z[CONF_TYPE] == "tamper":
            cg.add(var.register_zone_tamper(z[CONF_ZONE], bs))
        else:
            cg.add(var.register_zone_open(z[CONF_ZONE], bs))
