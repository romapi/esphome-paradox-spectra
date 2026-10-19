# esphome-paradox-spectra

A passive, read-only [ESPHome](https://esphome.io) external component for **Paradox
SP-series (Spectra)** alarm panels. An ESP32 wired to the panel's serial/PRG header
decodes the panel's event stream and exposes zones to Home Assistant over the native
API — **no PAI container, no MQTT**.

It is **passive**: it only listens (ESP RX ← panel TX), never transmits. So it is safe
to add to a bus another client (e.g. [PAI](https://github.com/ParadoxAlarmInterface/pai))
already drives, and it can never arm/disarm or actuate the panel.

## What it decodes

The panel streams unsolicited **37-byte LiveEvent** frames (command nibble `0xE`) on
every state change. The component frames on length + checksum (sum of bytes mod 256)
and decodes the event `major`/`minor`, partition, and the 16-byte ASCII zone label:

| event `major` | meaning | exposed as |
| --- | --- | --- |
| 0 / 1 | zone closed / **open** | `binary_sensor` `type: open` |
| 37 / 39 | **fire alarm** / restore | `binary_sensor` `type: fire` |
| 36 / 38 | zone in alarm / restore | logged |
| 42 / 43 | zone tamper / restore | logged |
| 29 / 31 | arm / disarm by user | logged |

`minor` is the zone number. A `text_sensor` publishes the last decoded event.

Every LiveEvent carries a **timestamp** and an event group. With `log_all_events: true`
the component logs the panel's full chatter — decoded to names — e.g.:

```
[I][paradox_spectra]: [2026-10-19 18:14] zone open (maj=1 min=5 p1) 'SAUN'
[I][paradox_spectra]: [2026-10-19 18:20] ARM (user) (maj=29 min=1 p1) 'JOHN'
[W][paradox_spectra]: [2026-10-19 18:33] NEW TROUBLE (maj=44 min=2 p1) ''
```
Arm/disarm, troubles, bus modules, tampers, system status, etc. are all named (alarms
and troubles log at WARN).

### Discovering zone numbers

You don't need to know the zone numbers up front. Any zone event for a zone **not in
your `zones:` list** is logged once at WARN with its number + panel label and a
ready-to-paste config line, e.g.:

```
[W][paradox_spectra]: DISCOVERED zone 5 'SAUN' (open closed) — not in config; add:  - { zone: 5, name: "SAUN" }
```

So: flash with an empty (or partial) `zones:` list, walk your PIRs / open doors, read
the device log, and paste the lines it prints. `debug_frames: true` additionally dumps
every validated frame as hex.

> Frame layout was derived from the open-source
> [Paradox Alarm Interface](https://github.com/ParadoxAlarmInterface/pai)
> (`spectra_magellan`). This component contains original code; PAI's own license
> applies to PAI.

## Hardware

- Any ESP32 + one UART to the panel **serial/PRG header**, **9600 8N1**.
- Panel serial is **5 V TTL**; ESP32 GPIO are 3.3 V and **not** 5 V-tolerant →
  **level-shift** the panel-TX → ESP-RX line (shifter or divider).
- Passive wiring: panel **TX → ESP RX** + common **GND** only. Leave ESP TX
  unconnected.

## Usage

```yaml
external_components:
  - source: github://romapi/esphome-paradox-spectra@main
    components: [paradox_spectra]

uart:
  id: uart_paradox
  rx_pin: GPIO1        # panel TX -> here (via level shift)
  baud_rate: 9600
  rx_buffer_size: 512

# Everything lives under the hub (samsung_ac-style nested lists):
paradox_spectra:
  uart_id: uart_paradox
  debug_frames: true     # log every validated frame as hex; off once zones are mapped
  log_all_events: true   # log EVERY decoded event (time + name + label) at INFO
  last_event:
    name: "Last Event"   # optional text_sensor — shows the last decoded event
  zones:
    - { zone: 1, name: "Living Room", device_class: motion }
    - { zone: 2, name: "Front Door",  device_class: door }
    - { zone: 7, type: fire, name: "Smoke Hall", device_class: smoke }
```

Each `zones:` entry is a full `binary_sensor` (so `id`, `filters`, `device_class`,
etc. all work) plus `zone:` (panel zone number) and `type:`:
- `open` (default) — PIR/contact, event major 0/1, or `zone_open` bitmap when polling.
- `fire` — smoke/fire, event major 37/39, or `zone_fire` bitmap.
- `tamper` — zone tamper, event major 42/43, or `zone_tamper` bitmap (use
  `device_class: tamper`).

## Passive vs active

By default the component is **passive** (RX only): zones update only when the panel
emits a change event — no current-state snapshot, no voltages. Great for presence +
smoke, and it never transmits.

Set `poll_interval` (and wire **UART TX**) to go **active**: the component logs in with
`pc_password` and reads panel RAM, giving authoritative current zone state on every
poll plus supply voltages and partition arm state. Active mode makes the ESP the **sole
bus master** — stop any other client (e.g. PAI) first.

```yaml
uart:
  id: uart_paradox
  tx_pin: GPIO2        # REQUIRED for active poll
  rx_pin: GPIO1
  baud_rate: 9600

paradox_spectra:
  uart_id: uart_paradox
  pc_password: "0000"  # panel PC password (section programming)
  poll_interval: 3s    # 0s / omitted = passive
  supply_voltage:  { name: "Panel VDC" }
  battery_voltage: { name: "Panel Battery" }
  dc_voltage:      { name: "Panel DC" }
  rf_noise:        { name: "RF Noise" }
  zones: [...]
```

> **Read-only by capability:** even in active mode the component sends **only** login +
> RAM-read requests — there are no arm/disarm/PGM opcodes anywhere in the code, so it
> cannot actuate the panel.

See [`example.yaml`](example.yaml) for a fuller config.

## ⚠ Safety

This is a **read-only, best-effort** convenience integration. It is **not** a
life-safety system. Keep the alarm panel's own siren and monitoring as your real fire
and intrusion protection — if the ESP or HA fails silently, the panel still works.

## License

MIT — see [`LICENSE`](LICENSE).
