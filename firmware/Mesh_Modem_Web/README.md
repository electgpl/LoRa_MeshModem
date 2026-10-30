# Mesh_Modem_Web v3.4 — Meshtastic®-compatible LoRa node with web chat

Everything from [MeshAT](../MeshAT) plus WiFi, an IRC-style web chat, captive portal, history in flash, rebroadcasting, echo detection, battery gauge and status LEDs. The serial terminal and the web page are two windows onto the same modem: whatever happens on one is mirrored on the other.

```
            ┌───────────────── Mesh_Modem_Web (ESP32-S3) ─────────────────┐
 Serial  ───┤ UART ─┐                                                     │
 terminal   │       ├──► single AT interpreter ──► SX1262 radio ──────────┼──► Meshtastic mesh
 Browser ───┤ HTTP ─┘          │                        │                 │
            │                  └──── events (RX, TX, nodes, echoes) ◄─────┤
            └─────────────────────────────────────────────────────────────┘
```

## Setup

1. Board *ESP32S3 Dev Module*, partition **Default 4MB with spiffs** (the history uses the data partition). Install RadioLib.
2. Edit the `CONFIG` block: unique `NODE_ID`, names, preset, band, `TZ_STR` (POSIX time zone for NTP).
3. Flash. With no stored credentials the modem opens the **captive portal**: join the open network `MeshModem-XXXX`, pick your WiFi and save.
4. Browse to **`http://meshmodem.local`** (or the IP reported by `ATI`).

To change WiFi later: hold **BOOT for 5 s** (with the board already running), send `AT+PORTAL`, or set it directly with `AT+WIFI=ssid,password`. The ESP32-S3 only supports **2.4 GHz** networks.

## Web interface

- **Chat** with time, coloured short names, RSSI, SNR and hops. Your messages show *waiting for echo…*, then *✓ relayed by N: …* or *✗ no echo* after 15 s.
- **`/AT…` lines** are sent as AT commands and the answer is printed in the chat.
- **History**: the last 300 stored messages load when the page opens; *Download* gets the full JSON-lines file, *Erase history* clears it.
- **Heard nodes** with names, NodeNum, RSSI, SNR and last-heard time.
- **Configuration**: names, NodeNum, preset, region, manual frequency, rebroadcast mode, hops, power; *Apply*, *Save to flash*, *Send NodeInfo*.
- Header: preset and frequency, rebroadcast counters, battery and WiFi.

## AT commands

| Command | Function |
|---|---|
| `AT` / `AT+HELP` | Test / help |
| `ATI` / `AT+INFO?` | Full configuration and status (radio, battery, rebroadcast counters, last reset reason, history usage, WiFi) |
| `AT+NODE=!aabbccdd` | NodeNum (32-bit hex) |
| `AT+NAME=long[,short]` | Long name (≤ 39) and short name (≤ 4) |
| `AT+PRESET=?` / `=LongFast` / `=6` | Modem preset |
| `AT+BAND=915,928` | Region band edges in MHz |
| `AT+FREQ=926.125` / `=0` | Manual frequency / automatic slot |
| `AT+HOP=3` | Max hops for packets originated here (0..7) |
| `AT+RELAY=0`/`1`/`2` / `AT+RELAY?` | Rebroadcast: mute / client / router |
| `AT+PWR=20` | TX power in dBm (−9..22) |
| `AT+SEND=text` | Text broadcast (≤ 232 bytes) |
| `AT+NI` / `AT+NODES` | Send NodeInfo / list heard nodes |
| `AT+BATT?` | Battery: SOC, voltage, rate, state and power source |
| `AT+LED=0`/`1` | LEDs off / on |
| `AT+SAVE` | Store the radio configuration in flash (NVS) |
| `AT+WIFI=ssid,password` / `AT+WIFI?` | WiFi credentials / status |
| `AT+PORTAL` | Open the WiFi setup portal |
| `AT+LOG=20` / `AT+LOGCLR` | Last N history entries / erase history |
| `AT+RAW=1` | Trace everything received (and rebroadcasts) |
| `AT&F` / `AT+RST` | Factory values (erases stored config) / restart |

Unsolicited responses:

```
+RX:!9e7563b8,!ffffffff,-87,9.5,1,hello everyone    text: from,to,RSSI,SNR,hops,text
+NODE:!9e7563b8,-87,9.5,SB01,Base Station          NodeInfo: from,RSSI,SNR,short,long
+TX:1a2b3c4d                                       packet sent
+ECO:1a2b3c4d,relay=B8,SB01,-80,7.5,2              echo: id,relay byte,name,RSSI,SNR,hop_limit
+RELAY:!9e7563b8,5c1e09a3,2                        rebroadcast done (AT+RAW=1)
+WIFI:UP,HomeNetwork,http://192.168.1.48/          also CONNECTING, DOWN, PORTAL, NOT_CONFIGURED
[web] AT+SEND=hi                                   command received from the web
```

## Rebroadcast

| `AT+RELAY` | Announced role | Behaviour |
|---|---|---|
| 0 | CLIENT_MUTE | never repeats |
| 1 (default) | CLIENT | repeats after an SNR-weighted wait, cancels if another node repeats first |
| 2 | ROUTER | always repeats, with priority; only for well-placed nodes |

Any packet with a valid CRC and `hop_limit > 0` is repeated, even if it cannot be decrypted. Details in [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md).

## Echoes (implicit ACK)

When a neighbour repeats one of your messages, the modem hears it back with your `from` and `id` and the neighbour's byte in `relay_node`. Each distinct repeater is counted once.

- Echo `hop_limit` = `AT+HOP − 1` means a direct neighbour; lower means a later hop (shown as *2nd hop*).
- Firmware older than 2.6 sends `relay_node = 0`; those repeaters are counted as one.
- No echo does not prove the message was lost (you only hear repeaters in range); an echo proves it left your area.

## Persistence

| What | Where | When |
|---|---|---|
| WiFi credentials | NVS | portal or `AT+WIFI=` |
| Radio configuration | NVS | only with `AT+SAVE` |
| Message history | LittleFS (`/log.jsonl` + `/log.old`, 384 KB each) | automatically, plus every boot with its reset reason |
| Factory values | `CONFIG` block | compile time; `AT&F` |

## Battery (MAX17048)

| Register | Data | Scale |
|---|---|---|
| `0x02` VCELL | cell voltage | 78.125 µV/LSB |
| `0x04` SOC | state of charge | 1/256 %/LSB |
| `0x16` CRATE | charge (+) / discharge (−) rate | 0.208 %/h per LSB |

Read every 10 s. `+BATT:76.4%,3.987V,+2.3%/h,CHARGING,USB`; `NOT_DETECTED` if the chip does not answer. Chat warning below 15 %.

## LEDs

| LED | State | Meaning |
|---|---|---|
| Red (GPIO47) | solid | powered from USB |
| Red | short blink every 3 s | running on battery |
| Green (GPIO48) | 3 fast blinks | text message received |
| Green | blink every 2 s | unread messages |

Messages are marked read when the web page is visible or the serial terminal is used. USB detection uses the BQ24075 `/PGOOD` pin if wired (`PIN_PGOOD`); otherwise it is estimated from the MAX17048 rate, which takes a few minutes to react.

## HTTP API

| Route | Method | Returns |
|---|---|---|
| `/` | GET | chat page (or the portal when active) |
| `/api/info` | GET | JSON: configuration, WiFi, battery, nodes |
| `/api/ev?since=N&seen=0/1` | GET | JSON: live events since N |
| `/api/at` | POST `cmd=...` | text answer of the AT command |
| `/api/log?n=N` | GET | JSON: last N history entries |
| `/log.jsonl` | GET | full history download |

## Notes

- The web page has **no password**: anyone on your LAN can chat and change the configuration. Do not expose it to the Internet.
- Radio, UART, HTTP server, DNS and LEDs run in a single `loop()` (no extra tasks, no SPI sharing). While a packet is being transmitted the web pauses briefly.
- On boards with a BQ24075 power path, run with the battery connected: without it, TX current peaks can brown out the ESP32-S3 (seen as `power-on` resets in the history).
- The firmware uses ~89 % of the default app partition; use *Huge APP* or *8M with spiffs* to add features.
