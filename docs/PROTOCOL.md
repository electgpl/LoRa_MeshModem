# Protocol notes

How MeshAT / Mesh_Modem_Web build and parse Meshtastic® frames without the Meshtastic firmware. Values were cross-checked against the official firmware sources (`RadioInterface.cpp`, `CryptoEngine.cpp`, `Router.cpp`, `FloodingRouter.cpp`, `NextHopRouter.cpp`, `MeshRadio.h`) and the `meshtastic` protobufs.

## 1. Physical layer

Same LoRa modulation as any point-to-point link, with two fixed parameters:

| Parameter | Value |
|---|---|
| Sync word | `0x2B` (RadioLib writes it as `0x24B4`) |
| Preamble | 16 symbols |
| Header / CRC | explicit header, payload CRC on |
| Max size | 255 bytes (16 header + 239 payload) |

### Presets

The preset name is also the default channel name, and it feeds both the channel hash and the frequency slot. It must match exactly.

| # | Preset | BW [kHz] | SF | CR | Channel hash |
|---|---|---|---|---|---|
| 0 | ShortTurbo | 500 | 7 | 4/5 | 0x0E |
| 1 | ShortFast | 250 | 7 | 4/5 | 0x70 |
| 2 | ShortSlow | 250 | 8 | 4/5 | 0x77 |
| 3 | **MediumFast** | 250 | 9 | 4/5 | 0x1F |
| 4 | MediumSlow | 250 | 10 | 4/5 | 0x18 |
| 5 | LongTurbo | 500 | 11 | 4/8 | 0x76 |
| 6 | LongFast | 250 | 11 | 4/5 | 0x08 |
| 7 | LongMod | 125 | 11 | 4/8 | 0x6E |
| 8 | LongSlow | 125 | 12 | 4/8 | 0x0F |

LDRO is enabled automatically by RadioLib when the symbol time is ≥ 16 ms (LongMod, LongSlow), as in the official firmware.

## 2. Frequency slot

```
numChannels = floor((freqEnd − freqStart) / BW)
slot        = djb2(channelName) mod numChannels        // djb2: h = 5381; h = h*33 + c (uint32)
freq [MHz]  = freqStart + BW/2 + slot · BW
```

Only the band edges are needed (`AT+BAND=start,end`), so no region table is required. Verified values: US/LongFast = 906.875 MHz (slot 20), ANZ/MediumFast = 926.125 MHz (slot 45).

## 3. Header (16 bytes, cleartext, little-endian)

| Offset | Size | Field | Sent by the modem |
|---|---|---|---|
| 0 | 4 | `to` | `0xFFFFFFFF` (broadcast) |
| 4 | 4 | `from` | own NodeNum |
| 8 | 4 | `id` | random, never 0 |
| 12 | 1 | flags | `hop_start << 5 \| hop_limit` |
| 13 | 1 | channel hash | `xor(name) ^ xor(PSK)` |
| 14 | 1 | `next_hop` | 0 (no preference) |
| 15 | 1 | `relay_node` | low byte of own NodeNum |

Flags byte: bits 0–2 `hop_limit`, bit 3 `want_ack`, bit 4 `via_mqtt`, bits 5–7 `hop_start`. Hops travelled = `hop_start − hop_limit`.

## 4. Encryption

- AES-128-CTR with the default key (channel PSK `AQ==` expands to `d4 f1 bb 3a 20 29 07 59 f0 bc ff ab cf 4e 69 01`). The key is public, so the default channel offers no confidentiality.
- Counter block: `id` as uint64 LE (8 bytes) | `from` (4 bytes LE) | 4 zero bytes, incremented as a 128-bit big-endian integer.
- Only the bytes after the 16-byte header are encrypted. CTR is symmetric, so the firmware implements only the AES block *encryption* (validated against FIPS-197 vector C.1 and a reference AES-CTR).

## 5. Payload (protobuf)

`Data` message, hand-encoded:

```
08 <portnum>          field 1, varint   (1 = TEXT_MESSAGE_APP, 4 = NODEINFO_APP)
12 <len> <payload>    field 2, bytes    (length as varint, 2 bytes when > 127)
```

NodeInfo (`User`) sent by Mesh_Modem_Web:

```
0A len "!e1ec0001"    id
12 len "long name"    long_name
1A len "SHRT"         short_name
28 FF 01              hw_model = PRIVATE_HW (255)
38 <role>             role (omitted for CLIENT = 0)
48 01                 is_unmessagable = true
```

The parser skips any field it does not need (`bitfield`, `dest`, `request_id`…), so it tolerates future additions.

### Example frame

`!a1b2c3d4` sends "Hola", MediumFast, 3 hops, `id = 0x12345678`:

```
FF FF FF FF  D4 C3 B2 A1  78 56 34 12  63  1F  00  D4  | FF F0 53 98 A8 20 AA FD
└── to ────┘ └── from ──┘ └── id ────┘ flg hash nh rly  └ 08 01 12 04 "Hola" encrypted ┘
```

## 6. Receive path

1. LoRa CRC OK and length ≥ 16.
2. `from == own NodeNum` → it is an **echo** of one of our packets: record `relay_node` as a repeater (implicit ACK).
3. `(from, id)` already seen → duplicate: cancel our pending rebroadcast (client mode).
4. Schedule a rebroadcast if allowed (section 7). This works for any channel, no decryption needed.
5. Channel hash mismatch → stop (other channel).
6. Decrypt, walk the protobuf, dispatch by `portnum`.

## 7. Rebroadcast (managed flooding)

A packet is rebroadcast when `to ≠ us`, `from ≠ us`, `hop_limit > 0`, `id ≠ 0` and `next_hop` is 0 or our low byte (same conditions as `NextHopRouter::perhapsRebroadcast()`). The copy goes out with `hop_limit − 1`, `relay_node` = our low byte, `next_hop` = 0; `hop_start` is unchanged.

![Fields changed on rebroadcast](images/rebroadcast-fields.png)

Delay (same as `RadioInterface::getTxDelayMsecWeighted()`):

```
slot   = 2.5 · T_symbol + 7.6 ms
CW     = map(SNR, −20…10 dB → 3…8)
client = 16·slot + random(0, 2^CW)·slot     cancels if another node repeats first
router = random(0, 2·CW)·slot               never cancels
```

| Preset | slot | Client, SNR −15 dB | Client, SNR +10 dB | Router |
|---|---|---|---|---|
| MediumFast | 12.7 ms | 204–293 ms | 204 ms – 3.4 s | 0–191 ms |
| LongFast | 28.1 ms | 449–646 ms | 449 ms – 7.6 s | 0–421 ms |

Low SNR means a short wait: far nodes repeat first because they add the most coverage.

## 8. Direct messages

Firmware ≥ 2.5 refuses to send a DM to a node without a known public key and rejects channel-encrypted DMs it receives (unless in licensed/ham mode). Proper DMs would require X25519 key exchange, SHA-256 and AES-256-CCM with an 8-byte tag and extra nonce. This project intentionally stays with channel broadcast.

## 9. MQTT

Packets are sent without `Data.bitfield`, so current gateways do not uplink them to public MQTT servers (`OK_TO_MQTT` not set). Over the air they are delivered normally.
