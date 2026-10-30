# MeshAT v1.1 — minimal Meshtastic®-compatible LoRa modem over AT commands

About 370 lines, one external library (RadioLib), own AES-128. The configuration lives in RAM: it boots with the values of the `CONFIG` block and AT commands change it until the next reset.

It sends and receives text on the default primary channel and announces itself with NodeInfo. It does **not** rebroadcast (behaves like `CLIENT_MUTE`); see [Mesh_Modem_Web](../Mesh_Modem_Web) for a full node.

## Commands

| Command | Function |
|---|---|
| `AT` / `AT+HELP` / `ATI` | Test, help, current configuration |
| `AT+NODE=!aabbccdd` | NodeNum (32-bit hex) |
| `AT+NAME=long[,short]` | Long name (≤ 39) and short name (≤ 4) |
| `AT+PRESET=?` / `=LongFast` / `=6` | Modem preset |
| `AT+BAND=915,928` | Region band edges in MHz |
| `AT+FREQ=926.125` / `=0` | Manual frequency / automatic slot |
| `AT+HOP=3` | Max hops (0..7) |
| `AT+PWR=20` | TX power in dBm (−9..22) |
| `AT+SEND=text` | Text broadcast (≤ 232 bytes) |
| `AT+NI` | Send NodeInfo |
| `AT+RAW=1` | Trace everything received |
| `AT&F` / `AT+RST` | Boot values / restart |

Unsolicited: `+RX:!from,!to,rssi,snr,hops,text` · `+NODE:!from,rssi,snr,short,long` · `+TX:id`

Common bands: **ANZ** `915,928` · **US** `902,928` · **EU_868** `869.4,869.65` · **BR_902** `902,907.5`

## Code walkthrough

| Block | What it does |
|---|---|
| `CONFIG` / hardware | Boot values and pinout (SPI, BUSY, DIO1, RF switch, TCXO) |
| `PSK`, `PRESETS[]` | Default key and the 9 presets; the preset name is the channel name |
| AES-128 | S-box, key expansion (once at boot), block encryption, CTR mode |
| `derive()` | Channel hash and frequency slot (djb2), same math as the official firmware |
| `applyRadio()` | Loads frequency, BW, SF, CR and power into the SX1262 |
| `pbVarint` / `pbNext` | Minimal protobuf encoder and field walker |
| `sendData()` | Builds header + `Data`, encrypts, CAD, transmits |
| `sendNodeInfo()` | Builds the `User` message (portnum 4) |
| `receive()` | Polls DIO1, filters CRC/hash/duplicates, decrypts, prints `+RX` / `+NODE` |
| `at()` | AT command interpreter |
| `setup()` / `loop()` | Radio init (sync word 0x2B, preamble 16, 140 mA OCP, boosted RX gain); RX, serial lines, periodic NodeInfo |

See [`docs/PROTOCOL.md`](../../docs/PROTOCOL.md) for the frame details.
