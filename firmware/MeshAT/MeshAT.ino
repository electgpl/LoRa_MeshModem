/* ============================================================================
   ELECTGPL - MeshAT v1.1 : minimal Meshtastic(R)-compatible LoRa modem over AT commands
   ----------------------------------------------------------------------------
   Board  : custom ESP32-S3 + SX1262 (AlarmSensor v1.7 pinout), XTAL, RF switch on GPIO5
   Arduino: ESP32S3 Dev Module
   Serial : 115200 8N1, line ending CR, LF or CR+LF
   Libs   : RadioLib only (>= 7.x). Own AES-128 implementation, no mbedtls, no NVS.
   Compat.: Meshtastic 2.x, default primary channel (public PSK "AQ=="),
            text broadcast + NodeInfo only. Does not rebroadcast (like CLIENT_MUTE).

   The configuration lives in RAM: it boots with the CONFIG block values and AT
   commands change it until the next reset. To make a change permanent, edit
   the CONFIG block. NODE_ID must be different on every board.

   AT COMMANDS
     AT                      Communication test
     AT+HELP                 This help
     ATI                     Current configuration (also AT+INFO?)
     AT+NODE=!aabbccdd       32-bit NodeNum in hex (8 digits, the ! is optional)
     AT+NAME=long[,short]    Long name (<=39) and short name (<=4)
     AT+PRESET=?             List presets
     AT+PRESET=MediumFast    Preset by name or number (0..8)
     AT+BAND=915,928         Region band edges in MHz (ANZ 915-928,
                             US 902-928, EU_868 869.4-869.65, BR_902 902-907.5)
     AT+FREQ=926.125         Manual frequency in MHz; AT+FREQ=0 = automatic slot
     AT+HOP=3                Max hops (0..7)
     AT+PWR=20               TX power in dBm (-9..22)
     AT+SEND=text            Broadcast text on the primary channel (<=232 bytes)
     AT+NI                   Send NodeInfo (name shown in the app)
     AT+RAW=1                Trace everything received (0 = off)
     AT&F                    Back to the CONFIG block values
     AT+RST                  Restart the ESP32

   UNSOLICITED RESPONSES
     +RX:!from,!to,rssi,snr,hops,text         text message received
     +NODE:!from,rssi,snr,short,long          NodeInfo received
     +TX:id                                   packet transmitted
     +RAW:len,status,rssi,snr,hash  /  +PKT:!from,portnum,len   (only with AT+RAW=1)

   EXAMPLE
     AT+NAME=Mobile Seba,SEBA
     AT+PRESET=LongFast
     AT+BAND=902,928
     AT+SEND=Hello mesh
   ============================================================================ */
#include <RadioLib.h>

// ======================= CONFIG (boot values) ==============================
#define NODE_ID     0xE1EC0001UL       // own NodeNum: unique per board, != 0 and != 0xFFFFFFFF
#define LONG_NAME   "ELECTGPL MeshAT"
#define SHORT_NAME  "MAT"
#define PRESET_IDX  3                  // 3 = MediumFast (see PRESETS[])
#define BAND_START  915.0f             // ANZ
#define BAND_END    928.0f
#define FREQ_MAN    0.0f               // 0 = automatic slot
#define HOP_LIMIT   3
#define TX_DBM      20

// ======================= Hardware (AlarmSensor v1.7) ========================
#define PIN_SCK   12
#define PIN_MISO  13
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_RST   18
#define PIN_BUSY   4
#define PIN_DIO1  14
#define PIN_RFSW   5                   // HIGH during TX; driven by RadioLib
#define TCXO_V   0.0f                  // 0 = XTAL. Wio-SX1262 (TCXO): 1.8
#define DIO2_RFSW false                // true if DIO2 drives the RF switch (Wio-SX1262)

// ======================= Meshtastic constants ===============================
const uint8_t PSK[16] = {0xd4,0xf1,0xbb,0x3a,0x20,0x29,0x07,0x59,      // default PSK
                         0xf0,0xbc,0xff,0xab,0xcf,0x4e,0x69,0x01};     // ("AQ==")
struct Preset { const char* name; float bw; uint8_t sf, cr; };         // name = channel name
const Preset PRESETS[] = {
  {"ShortTurbo", 500, 7, 5}, {"ShortFast", 250, 7, 5}, {"ShortSlow", 250, 8, 5},
  {"MediumFast", 250, 9, 5}, {"MediumSlow", 250, 10, 5}, {"LongTurbo", 500, 11, 8},
  {"LongFast", 250, 11, 5}, {"LongMod", 125, 11, 8}, {"LongSlow", 125, 12, 8}};
const int NPRE = sizeof(PRESETS) / sizeof(*PRESETS);
#define PORT_TEXT     1
#define PORT_NODEINFO 4
#define NI_PERIOD_MS  (3UL * 3600UL * 1000UL)                          // NodeInfo every 3 h

const char HELP[] =
  "AT  AT+HELP  ATI\r\n"
  "AT+NODE=!aabbccdd     NodeNum (8 hex digits)\r\n"
  "AT+NAME=long[,short] long name (<=39) and short name (<=4)\r\n"
  "AT+PRESET=?|name|n   modem preset\r\n"
  "AT+BAND=f0,f1         region band in MHz (ANZ 915,928 / US 902,928)\r\n"
  "AT+FREQ=MHz|0         manual frequency / 0 = automatic slot\r\n"
  "AT+HOP=0..7           max hops\r\n"
  "AT+PWR=-9..22         TX power in dBm\r\n"
  "AT+SEND=text          text broadcast\r\n"
  "AT+NI                 send NodeInfo\r\n"
  "AT+RAW=0|1            RX trace\r\n"
  "AT&F  AT+RST          boot values / restart\r\n";

// ======================= State ==============================================
struct Cfg { uint32_t node; char ln[40], sn[5]; uint8_t preset, hop; int8_t pwr; float f0, f1, fov; };
const Cfg DEF = {NODE_ID, LONG_NAME, SHORT_NAME, PRESET_IDX, HOP_LIMIT, TX_DBM, BAND_START, BAND_END, FREQ_MAN};
Cfg cfg = DEF;

SPIClass spiLora(FSPI);
SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spiLora);
uint32_t nCh, slot; uint8_t chHash; float freq;
uint64_t seen[16]; uint8_t seenIdx; bool rawMode;

// ======================= AES-128 (encrypt only; CTR never needs decrypt) ====
const uint8_t SBOX[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
uint8_t rk[176];                                                     // 11 round keys

uint8_t xt(uint8_t x) { return (x << 1) ^ ((x >> 7) * 0x1B); }       // x*2 in GF(2^8)

void aesKeyExpand(const uint8_t* key) {                              // FIPS-197 §5.2
  memcpy(rk, key, 16);
  for (int i = 16, rcon = 1; i < 176; i += 4) {
    uint8_t t[4]; memcpy(t, rk + i - 4, 4);
    if (i % 16 == 0) {                                               // RotWord + SubWord + Rcon
      uint8_t u = t[0];
      t[0] = SBOX[t[1]] ^ rcon; t[1] = SBOX[t[2]]; t[2] = SBOX[t[3]]; t[3] = SBOX[u];
      rcon = xt(rcon);
    }
    for (int j = 0; j < 4; j++) rk[i + j] = rk[i + j - 16] ^ t[j];
  }
}

void aesBlock(uint8_t* s) {                                          // encrypt 16 bytes in place
  for (int j = 0; j < 16; j++) s[j] ^= rk[j];
  for (int r = 1; r <= 10; r++) {
    uint8_t t[16];
    for (int c = 0; c < 4; c++)                                      // SubBytes + ShiftRows
      for (int w = 0; w < 4; w++) t[4 * c + w] = SBOX[s[4 * ((c + w) & 3) + w]];
    if (r < 10)
      for (int c = 0; c < 4; c++) {                                  // MixColumns
        uint8_t* a = t + 4 * c; uint8_t x = a[0] ^ a[1] ^ a[2] ^ a[3], a0 = a[0];
        a[0] ^= x ^ xt(a[0] ^ a[1]); a[1] ^= x ^ xt(a[1] ^ a[2]);
        a[2] ^= x ^ xt(a[2] ^ a[3]); a[3] ^= x ^ xt(a[3] ^ a0);
      }
    for (int j = 0; j < 16; j++) s[j] = t[j] ^ rk[16 * r + j];      // AddRoundKey
  }
}

// AES-CTR (encrypts and decrypts). Counter block = id(8 B LE) | from(4 B LE) | 0(4 B),
// incremented as a 128-bit big-endian integer (same as mbedtls_aes_crypt_ctr).
void ctr(uint8_t* d, size_t n, uint32_t id, uint32_t from) {
  uint8_t cb[16] = {0}, ks[16];
  memcpy(cb, &id, 4); memcpy(cb + 8, &from, 4);
  for (size_t i = 0; i < n; i++) {
    if ((i & 15) == 0) {
      memcpy(ks, cb, 16); aesBlock(ks);
      for (int k = 15; k >= 0 && ++cb[k] == 0; k--) {}
    }
    d[i] ^= ks[i & 15];
  }
}

// ======================= Channel and radio (same as RadioInterface.cpp) =====
uint32_t djb2(const char* s) { uint32_t h = 5381; while (*s) h = (h << 5) + h + (uint8_t)*s++; return h; }

void derive() {
  const Preset& p = PRESETS[cfg.preset];
  chHash = 0;
  for (const char* c = p.name; *c; c++) chHash ^= (uint8_t)*c;     // xorHash(name)
  for (uint8_t b : PSK) chHash ^= b;                                // ^ xorHash(PSK)
  nCh = (uint32_t)floorf((cfg.f1 - cfg.f0) / (p.bw / 1000.0f) + 1e-4f); if (!nCh) nCh = 1;
  slot = djb2(p.name) % nCh;
  freq = cfg.fov > 0 ? cfg.fov : cfg.f0 + p.bw / 2000.0f + slot * (p.bw / 1000.0f);
}

int applyRadio() {
  derive();
  const Preset& p = PRESETS[cfg.preset];
  int e = radio.standby();
  if (!e) e = radio.setFrequency(freq);
  if (!e) e = radio.setBandwidth(p.bw);
  if (!e) e = radio.setSpreadingFactor(p.sf);
  if (!e) e = radio.setCodingRate(p.cr);
  if (!e) e = radio.setOutputPower(cfg.pwr);
  radio.startReceive();
  return e;
}

// ======================= Minimal protobuf ===================================
size_t pbVarint(uint8_t* b, uint32_t v) { size_t k = 0; do { b[k] = v & 0x7F; v >>= 7; if (v) b[k] |= 0x80; k++; } while (v); return k; }

bool pbReadVarint(const uint8_t* b, size_t L, size_t& i, uint32_t& v) {
  v = 0;
  for (int sh = 0; i < L && sh < 70; sh += 7) {                    // accepts 64-bit varints (truncated)
    uint8_t c = b[i++]; if (sh < 32) v |= (uint32_t)(c & 0x7F) << sh;
    if (!(c & 0x80)) return true;
  }
  return false;
}

// Next field: f = field number, v = value (varint/fixed32) or length, s = data if wire type 2
bool pbNext(const uint8_t* b, size_t L, size_t& i, uint32_t& f, uint32_t& v, const uint8_t*& s) {
  uint32_t key; s = nullptr;
  if (i >= L || !pbReadVarint(b, L, i, key)) return false;
  f = key >> 3;
  switch (key & 7) {
    case 0: return pbReadVarint(b, L, i, v);
    case 2: if (!pbReadVarint(b, L, i, v) || i + v > L) return false; s = b + i; i += v; return true;
    case 5: if (i + 4 > L) return false; memcpy(&v, b + i, 4); i += 4; return true;
    case 1: if (i + 8 > L) return false; i += 8; return true;
    default: return false;
  }
}

// ======================= TX =================================================
bool sendData(uint8_t port, const uint8_t* pl, size_t len) {
  uint8_t pkt[256]; uint32_t to = 0xFFFFFFFF, id = esp_random() | 1;
  uint8_t* d = pkt + 16; size_t k = 0;
  d[k++] = 0x08; d[k++] = port;                                     // Data.portnum (field 1, varint)
  d[k++] = 0x12; k += pbVarint(d + k, len);                         // Data.payload (field 2, bytes)
  if (16 + k + len > 255) return false;
  memcpy(d + k, pl, len); k += len;
  memcpy(pkt, &to, 4); memcpy(pkt + 4, &cfg.node, 4); memcpy(pkt + 8, &id, 4);
  pkt[12] = (cfg.hop << 5) | cfg.hop;                               // hop_start | hop_limit, want_ack = 0
  pkt[13] = chHash; pkt[14] = 0; pkt[15] = cfg.node & 0xFF;         // channel hash, next_hop, relay_node
  ctr(d, k, id, cfg.node);
  for (int i = 0; i < 5 && radio.scanChannel() == RADIOLIB_LORA_DETECTED; i++)   // CAD before transmitting
    delay(100 + esp_random() % 400);
  int e = radio.transmit(pkt, 16 + k);
  radio.startReceive();
  if (!e) Serial.printf("+TX:%08x\r\n", (unsigned)id);
  return e == RADIOLIB_ERR_NONE;
}

bool sendNodeInfo() {                                               // protobuf User
  char id[10]; snprintf(id, sizeof(id), "!%08x", (unsigned)cfg.node);
  uint8_t u[64]; size_t n = 0;
  auto str = [&](uint8_t tag, const char* s) { u[n++] = tag; u[n++] = strlen(s); memcpy(u + n, s, strlen(s)); n += strlen(s); };
  str(0x0A, id); str(0x12, cfg.ln); str(0x1A, cfg.sn);              // id, long_name, short_name
  u[n++] = 0x28; u[n++] = 0xFF; u[n++] = 0x01;                      // hw_model = PRIVATE_HW (255)
  return sendData(PORT_NODEINFO, u, n);
}

// ======================= RX =================================================
void receive() {
  if (digitalRead(PIN_DIO1) != HIGH) return;                        // DIO1 high = RX done
  uint8_t p[256]; size_t n = radio.getPacketLength(); if (n > 255) n = 255;
  int st = radio.readData(p, n); float rssi = radio.getRSSI(), snr = radio.getSNR();
  radio.startReceive();
  if (rawMode) Serial.printf("+RAW:%u,%d,%.0f,%.1f,%02X\r\n", (unsigned)n, st, rssi, snr, n > 13 ? p[13] : 0);
  if (st != RADIOLIB_ERR_NONE || n < 20 || p[13] != chHash) return;
  uint32_t to, from, id; memcpy(&to, p, 4); memcpy(&from, p + 4, 4); memcpy(&id, p + 8, 4);
  if (from == cfg.node) return;
  uint64_t key = ((uint64_t)from << 32) | id;
  for (uint64_t s : seen) if (s == key) return;                     // duplicate (mesh rebroadcast)
  seen[seenIdx++ & 15] = key;
  int hl = p[12] & 7, hs = p[12] >> 5, hops = hs ? hs - hl : -1;
  uint8_t* d = p + 16; size_t L = n - 16; ctr(d, L, id, from);
  uint32_t port = 0, f, v; const uint8_t *s, *pl = nullptr; size_t pn = 0, i = 0;
  while (i < L) {
    if (!pbNext(d, L, i, f, v, s)) return;                          // different PSK or corrupted frame
    if (f == 1 && !s) port = v; else if (f == 2 && s) { pl = s; pn = v; }
  }
  if (!pl) return;
  if (port == PORT_TEXT) {
    Serial.printf("+RX:!%08x,!%08x,%.0f,%.1f,%d,", (unsigned)from, (unsigned)to, rssi, snr, hops);
    Serial.write(pl, pn); Serial.print("\r\n");
  } else if (port == PORT_NODEINFO) {
    const uint8_t *ln = nullptr, *sn = nullptr; uint32_t lL = 0, sL = 0; size_t j = 0;
    while (j < pn && pbNext(pl, pn, j, f, v, s)) { if (f == 2 && s) { ln = s; lL = v; } if (f == 3 && s) { sn = s; sL = v; } }
    Serial.printf("+NODE:!%08x,%.0f,%.1f,%.*s,%.*s\r\n", (unsigned)from, rssi, snr,
                  (int)sL, sn ? (const char*)sn : "", (int)lL, ln ? (const char*)ln : "");
  } else if (rawMode) Serial.printf("+PKT:!%08x,%u,%u\r\n", (unsigned)from, (unsigned)port, (unsigned)pn);
}

// ======================= AT commands ========================================
void info() {
  const Preset& p = PRESETS[cfg.preset];
  Serial.printf("+NODE:!%08x\r\n+NAME:%s,%s\r\n", (unsigned)cfg.node, cfg.ln, cfg.sn);
  Serial.printf("+PRESET:%s,BW=%.1f,SF=%u,CR=4/%u,HASH=%02X\r\n", p.name, p.bw, p.sf, p.cr, chHash);
  Serial.printf("+BAND:%.3f,%.3f\r\n+FREQ:%.4f,%s,SLOT=%u/%u\r\n", cfg.f0, cfg.f1, freq,
                cfg.fov > 0 ? "MANUAL" : "AUTO", (unsigned)slot + 1, (unsigned)nCh);
  Serial.printf("+HOP:%u\r\n+PWR:%d\r\n", cfg.hop, cfg.pwr);
}

void at(String s) {
  s.trim(); if (!s.length()) return;
  String u = s; u.toUpperCase();
  int eq = s.indexOf('='); String a = eq >= 0 ? s.substring(eq + 1) : ""; a.trim();
  bool ok = true;

  if (u == "AT") {}
  else if (u == "AT+HELP" || u == "AT+HELP?") Serial.print(HELP);
  else if (u == "ATI" || u == "AT+INFO?") info();
  else if (u.startsWith("AT+SEND=")) ok = a.length() > 0 && sendData(PORT_TEXT, (const uint8_t*)a.c_str(), a.length());
  else if (u == "AT+NI") ok = sendNodeInfo();
  else if (u.startsWith("AT+NODE=")) {
    if (a[0] == '!') a.remove(0, 1);
    uint32_t v = strtoul(a.c_str(), nullptr, 16);
    ok = a.length() == 8 && v && v != 0xFFFFFFFF;
    if (ok) { cfg.node = v; sendNodeInfo(); }
  }
  else if (u.startsWith("AT+NAME=")) {
    int c = a.indexOf(','); String ln = c >= 0 ? a.substring(0, c) : a, sn = c >= 0 ? a.substring(c + 1) : a.substring(0, 4);
    ln.trim(); sn.trim();
    ok = ln.length() > 0 && ln.length() < sizeof(cfg.ln) && sn.length() > 0 && sn.length() < sizeof(cfg.sn);
    if (ok) { strcpy(cfg.ln, ln.c_str()); strcpy(cfg.sn, sn.c_str()); sendNodeInfo(); }
  }
  else if (u == "AT+PRESET=?") { for (int i = 0; i < NPRE; i++) Serial.printf("+PRESET:%d,%s\r\n", i, PRESETS[i].name); }
  else if (u.startsWith("AT+PRESET=")) {
    int k = -1;
    for (int i = 0; i < NPRE; i++) if (a.equalsIgnoreCase(PRESETS[i].name)) k = i;
    if (k < 0 && isDigit(a[0])) k = a.toInt();
    ok = k >= 0 && k < NPRE; if (ok) { cfg.preset = k; ok = !applyRadio(); sendNodeInfo(); }
  }
  else if (u.startsWith("AT+BAND=")) {
    float f0 = a.toFloat(), f1 = a.substring(a.indexOf(',') + 1).toFloat();
    ok = a.indexOf(',') > 0 && f0 >= 150 && f1 <= 960 && f1 > f0;
    if (ok) { cfg.f0 = f0; cfg.f1 = f1; ok = !applyRadio(); }
  }
  else if (u.startsWith("AT+FREQ=")) {
    float f = a.toFloat(); ok = f == 0 || (f >= 150 && f <= 960);
    if (ok) { cfg.fov = f; ok = !applyRadio(); }
  }
  else if (u.startsWith("AT+HOP=")) { int h = a.toInt(); ok = isDigit(a[0]) && h <= 7; if (ok) cfg.hop = h; }
  else if (u.startsWith("AT+PWR=")) { int pw = a.toInt(); ok = pw >= -9 && pw <= 22; if (ok) { cfg.pwr = pw; ok = !applyRadio(); } }
  else if (u.startsWith("AT+RAW=")) rawMode = a.toInt();
  else if (u == "AT&F") { cfg = DEF; applyRadio(); sendNodeInfo(); }
  else if (u == "AT+RST") { Serial.print("OK\r\n"); Serial.flush(); ESP.restart(); }
  else ok = false;

  Serial.print(ok ? "OK\r\n" : "ERROR\r\n");
}

// ======================= Setup / loop =======================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_DIO1, INPUT);
  aesKeyExpand(PSK);
  derive();
  spiLora.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
  radio.setRfSwitchPins(RADIOLIB_NC, PIN_RFSW);
  const Preset& p = PRESETS[cfg.preset];
  int e = radio.begin(freq, p.bw, p.sf, p.cr, 0x2B, cfg.pwr, 16, TCXO_V);   // sync word 0x2B, preamble 16
  radio.setDio2AsRfSwitch(DIO2_RFSW);
  radio.setCurrentLimit(140);
  radio.setRxBoostedGainMode(true);
  radio.startReceive();
  Serial.printf("\r\n+READY:MeshAT v1.1,RADIO=%d  (AT+HELP = commands)\r\n", e);
  info();
  if (!e) sendNodeInfo();
}

void loop() {
  static String line; static uint32_t tNI = millis();
  receive();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') { at(line); line = ""; }
    else if (line.length() < 240) line += c;
  }
  if (millis() - tNI >= NI_PERIOD_MS) { tNI = millis(); sendNodeInfo(); }
}
