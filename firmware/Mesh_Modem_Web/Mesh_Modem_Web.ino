/* ============================================================================
   ELECTGPL - Mesh_Modem_Web v3.4
   LoRa node compatible with the Meshtastic(R) network: rebroadcasting (hops),
   web chat, AT commands, battery gauge, status LEDs and message echoes.
   ----------------------------------------------------------------------------
   Board  : custom ESP32-S3 + SX1262 (AlarmSensor v1.7 pinout), XTAL, RF switch on GPIO5
   Arduino: ESP32S3 Dev Module, partition "Default 4MB with spiffs"
   Serial : 115200 8N1, line ending CR, LF or CR+LF
   Libs   : RadioLib (only external one) + WiFi, WebServer, DNSServer, ESPmDNS,
            Preferences, LittleFS and Wire (all part of the ESP32 core).
            Own AES-128 implementation (no mbedtls).
   Compat.: Meshtastic 2.x, default primary channel (public PSK "AQ=="), text
            broadcast + NodeInfo. Rebroadcasts mesh packets using the official
            firmware rules (managed flooding) in 3 modes. No direct messages.

   WIFI
     With no stored credentials, or holding BOOT (GPIO0) for 5 s, it starts a
     captive portal: open network "MeshModem-XXXX", page at http://192.168.4.1
     Once on your home network: http://meshmodem.local or the IP shown by ATI.
     Everything done on the web is mirrored on the serial terminal and vice versa.

   BATTERY (MAX17048, I2C SDA=8 SCL=9, 0x36)
     State of charge (SOC %), cell voltage and charge/discharge rate (%/h).
     Read every 10 s; AT+BATT? reports it, ATI includes it and the web header
     shows it. Chat warning when it drops below 15 %.

   LEDS
     Red   (GPIO47): solid = powered from USB; short blink every 3 s = on battery.
     Green (GPIO48): 3 fast blinks when a text message arrives, then one blink
                     every 2 s while there are unread messages. Messages are marked
                     read when the web page is visible or the serial terminal is used.
     USB detection: BQ24075 /PGOOD pin if wired to a GPIO (PIN_PGOOD); otherwise it
     is estimated from the MAX17048 rate (reacts within minutes).
     AT+LED=0 turns the LEDs off (e.g. at night); AT+LED=1 turns them back on.

   ECHOES (implicit ACK)
     When another node rebroadcasts one of our messages, the modem hears the echo
     (same from and id, with the relay_node of whoever repeated it). Reported as
     +ECO:id,relay=XX,name,rssi,snr,hop_limit and on the web next to the message:
     "relayed by N" (or "no echo" if nobody repeated it within 15 s).
     hop_limit = HOP-1 is a direct neighbour; lower means one hop further.

   REBROADCAST (AT+RELAY)
     0 = mute   (CLIENT_MUTE): never rebroadcasts.
     1 = client (CLIENT): rebroadcasts after an SNR-based delay and cancels if
         another node repeats first. Default.
     2 = router (ROUTER): always rebroadcasts, with priority. Only for a well
         placed node (height, good antenna).
     Any packet with a valid CRC and hop_limit > 0 is rebroadcast, even if it
     cannot be decrypted (other channels, other people's DMs), like the
     firmware's ALL mode.

   HISTORY
     Received and sent messages are stored in flash (LittleFS) with date and time
     (NTP). They survive resets and power cuts. Shown on the web, with AT+LOG, and
     erased with AT+LOGCLR. Every boot is logged together with its reset reason.

   CONFIGURATION
     Changes (AT or web) apply immediately. AT+SAVE stores them in flash (NVS);
     without AT+SAVE they are lost on reset. AT&F restores the CONFIG block.

   AT COMMANDS
     AT                      Communication test
     AT+HELP                 This help
     ATI / AT+INFO?          Full configuration and current status
     AT+NODE=!aabbccdd       32-bit NodeNum in hex (8 digits)
     AT+NAME=long[,short]    Long name (<=39) and short name (<=4)
     AT+PRESET=?             List presets
     AT+PRESET=MediumFast    Preset by name or number (0..8)
     AT+BAND=915,928         Region band edges in MHz (ANZ 915,928 / US 902,928 /
                             EU_868 869.4,869.65 / BR_902 902,907.5)
     AT+FREQ=926.125         Manual frequency in MHz; AT+FREQ=0 = automatic slot
     AT+HOP=3                Max hops for packets originated by this node (0..7)
     AT+RELAY=1              Rebroadcast: 0 mute, 1 client, 2 router
     AT+PWR=20               TX power in dBm (-9..22)
     AT+SEND=text            Broadcast text on the primary channel (<=232 bytes)
     AT+NI                   Send NodeInfo
     AT+BATT?                Battery: SOC, voltage, rate (%/h) and source (USB/BATTERY)
     AT+LED=0|1              LEDs off / on
     AT+NODES                Heard nodes
     AT+SAVE                 Store the radio configuration in flash
     AT+WIFI=ssid,password   Store WiFi credentials and connect
     AT+WIFI?                WiFi status
     AT+PORTAL               Start the WiFi setup portal
     AT+LOG=20               Last N history entries (AT+LOG = 20)
     AT+LOGCLR               Erase the history
     AT+RAW=1                Trace everything received (0 = off)
     AT&F                    CONFIG block values (erases stored config)
     AT+RST                  Restart the ESP32

   UNSOLICITED RESPONSES
     +RX:!from,!to,rssi,snr,hops,text         +NODE:!from,rssi,snr,short,long
     +TX:id     +ECO:id,relay=XX,name,rssi,snr,hop_limit
     +RELAY:!from,id,hop_limit (with AT+RAW=1)     +WIFI:UP,ssid,url     +WIFI:DOWN     +WIFI:PORTAL,ssid,ip
     [web] command                            command received from the web
   ============================================================================ */
#include <RadioLib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <vector>
#include <time.h>
#include <Wire.h>

// ======================= CONFIG (factory values) ============================
#define NODE_ID     0xE1EC0001UL       // own NodeNum: unique per board, != 0 and != 0xFFFFFFFF
#define LONG_NAME   "ELECTGPL MeshWeb"
#define SHORT_NAME  "MMW"
#define PRESET_IDX  3                  // 3 = MediumFast (see PRESETS[])
#define BAND_START  915.0f             // ANZ
#define BAND_END    928.0f
#define FREQ_MAN    0.0f               // 0 = automatic slot
#define HOP_LIMIT   3
#define TX_DBM      20
#define RELAY_MODE  1                  // 0 mute, 1 client, 2 router
#define HOSTNAME    "meshmodem"        // http://meshmodem.local
#define TZ_STR      "<-03>3"           // POSIX TZ string (here Argentina, UTC-3, no DST)
#define PORTAL_HOLD_MS 5000            // hold BOOT this long = WiFi portal
#define LOG_MAX     (384UL * 1024UL)   // size per history file (rotated to .old)

// ======================= Hardware (AlarmSensor v1.7) ========================
#define PIN_SCK   12
#define PIN_MISO  13
#define PIN_MOSI  11
#define PIN_CS    10
#define PIN_RST   18
#define PIN_BUSY   4
#define PIN_DIO1  14
#define PIN_RFSW   5                   // HIGH during TX; driven by RadioLib
#define PIN_BOOT   0                   // BOOT button
#define PIN_SDA    8                   // MAX17048 I2C (bus shared with the ADXL343)
#define PIN_SCL    9
#define MAX17048_ADDR 0x36
#define BATT_PERIOD_MS 10000           // fuel gauge read period
#define BATT_LOW_PCT   15              // low battery warning
#define PIN_LED_RED   47               // board LEDs, anode to GPIO: HIGH = on
#define PIN_LED_GREEN 48
#define PIN_PGOOD     -1               // BQ24075 /PGOOD (open-drain, LOW = USB present) wired to a
                                       // free GPIO (1, 2, 6 or 7); -1 = estimate from the MAX17048
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
  "AT  AT+HELP  ATI (configuration and status)\r\n"
  "AT+NODE=!aabbccdd     NodeNum (8 hex digits)\r\n"
  "AT+NAME=long[,short] long name (<=39) and short name (<=4)\r\n"
  "AT+PRESET=?|name|n   modem preset\r\n"
  "AT+BAND=f0,f1         region band in MHz (ANZ 915,928 / US 902,928)\r\n"
  "AT+FREQ=MHz|0         manual frequency / 0 = automatic slot\r\n"
  "AT+HOP=0..7           max hops\r\n"
  "AT+RELAY=0|1|2        rebroadcast: mute / client / router\r\n"
  "AT+PWR=-9..22         TX power in dBm\r\n"
  "AT+SEND=text          text broadcast\r\n"
  "AT+NI  AT+NODES       send NodeInfo / heard nodes\r\n"
  "AT+BATT?              battery (SOC, voltage, rate, source)\r\n"
  "AT+LED=0|1            LEDs off / on\r\n"
  "AT+SAVE               store radio configuration in flash\r\n"
  "AT+WIFI=ssid,pass     WiFi credentials   AT+WIFI?  status\r\n"
  "AT+PORTAL             WiFi portal (or hold BOOT 5 s)\r\n"
  "AT+LOG=n  AT+LOGCLR   history / erase history\r\n"
  "AT+RAW=0|1            RX trace\r\n"
  "AT&F  AT+RST          factory values / restart\r\n";

// ======================= State ==============================================
struct Cfg { uint32_t magic, node; char ln[40], sn[5]; uint8_t preset, hop; int8_t pwr; float f0, f1, fov; uint8_t relay; };
const Cfg DEF = {0x4D4D5733, NODE_ID, LONG_NAME, SHORT_NAME, PRESET_IDX, HOP_LIMIT, TX_DBM, BAND_START, BAND_END, FREQ_MAN, RELAY_MODE};
Cfg cfg = DEF;
struct NodeE { uint32_t id; char sn[5]; char ln[40]; float rssi, snr; uint32_t seen; };   // heard node
NodeE nodes[32];

SPIClass spiLora(FSPI);
SX1262 radio = new Module(PIN_CS, PIN_DIO1, PIN_RST, PIN_BUSY, spiLora);
uint32_t nCh, slot; uint8_t chHash; float freq;
uint64_t seen[32]; uint8_t seenIdx; bool rawMode;                  // duplicates: from<<32 | id
float slotMs;                                                        // contention slot (ms)

// Rebroadcast queue: raw packets waiting for their turn
struct Pend { bool used; uint32_t from, id, due; uint8_t len, buf[255]; };
Pend pend[6];
uint32_t stRelayed, stCanceled, stDropped;                           // rebroadcast statistics
const char* RELAY_NAME[] = {"MUTE", "CLIENT", "ROUTER"};

Preferences nvs;
WebServer server(80);
DNSServer dns;
String wSsid, wPass;
bool portal = false, wifiUp = false;
uint32_t tWifiPending = 0;                                           // deferred reconnection (web)

// Single output: everything goes to the UART and, during a web request, also to the HTTP response
class Tee : public Print {
public:
  String* cap = nullptr;
  size_t write(uint8_t c) override { Serial.write(c); if (cap) *cap += (char)c; return 1; }
  size_t write(const uint8_t* b, size_t n) override { Serial.write(b, n); if (cap) cap->concat((const char*)b, n); return n; }
} io;

// ======================= Web events (ring buffer) ===========================
#define NEV 48
String evBuf[NEV]; uint32_t evSeq = 0;

uint32_t nowTs() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }

String jStr(const char* s, size_t n) {                               // escaped JSON string
  String o = "\""; o.reserve(n + 8);
  for (size_t i = 0; i < n; i++) {
    uint8_t c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
String jStr(const char* s) { return jStr(s, strlen(s)); }
String hexId(uint32_t id) { char b[10]; snprintf(b, sizeof(b), "!%08x", (unsigned)id); return b; }

void pushEv(const String& body) {
  evBuf[evSeq % NEV] = "{\"s\":" + String(evSeq) + ",\"ts\":" + String(nowTs()) +
                       ",\"up\":" + String(millis() / 1000) + "," + body + "}";
  evSeq++;
}
void sysEv(const String& txt) { pushEv("\"t\":\"sys\",\"txt\":" + jStr(txt.c_str())); }

// ======================= Flash history (LittleFS) ===========================
#define LOG_FILE "/log.jsonl"                                        // one JSON line per event
#define LOG_OLD  "/log.old"
bool fsOk = false;

void logWrite(const String& line) {
  if (!fsOk) return;
  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;
  f.print(line); f.print('\n');
  size_t sz = f.size(); f.close();
  if (sz > LOG_MAX) { LittleFS.remove(LOG_OLD); LittleFS.rename(LOG_FILE, LOG_OLD); }   // rotation
}

void logEv(const String& body) { pushEv(body); logWrite(evBuf[(evSeq - 1) % NEV]); }   // event + flash

// Last valid lines of a file (reads at most the last 'win' bytes)
void readTail(const char* path, size_t win, std::vector<String>& out) {
  File f = LittleFS.open(path, "r");
  if (!f) return;
  size_t sz = f.size(), start = sz > win ? sz - win : 0;
  f.seek(start);
  String buf = f.readString(); f.close();
  int a = 0;
  if (start) { a = buf.indexOf('\n') + 1; if (a <= 0) return; }        // drop the truncated line
  while (a < (int)buf.length()) {
    int b = buf.indexOf('\n', a); if (b < 0) b = buf.length();
    String l = buf.substring(a, b); l.trim();
    if (l.startsWith("{") && l.endsWith("}")) out.push_back(l);      // skip lines damaged by a power cut
    a = b + 1;
  }
}

void logTail(size_t n, std::vector<String>& out) {
  readTail(LOG_FILE, 24 * 1024, out);
  if (out.size() < n) { std::vector<String> old; readTail(LOG_OLD, 24 * 1024, old); out.insert(out.begin(), old.begin(), old.end()); }
  if (out.size() > n) out.erase(out.begin(), out.end() - n);
}

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_SW:       return "software restart";
    case ESP_RST_PANIC:    return "exception (panic)";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout (supply dip)";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_USB:      return "USB";
    case ESP_RST_DEEPSLEEP:return "deep sleep";
    default:               return "unknown";
  }
}

// ======================= Heard nodes table ==================================
NodeE* nodeGet(uint32_t id) {
  NodeE* old = &nodes[0];
  for (auto& n : nodes) { if (n.id == id) return &n; if (n.seen < old->seen) old = &n; }
  memset(old, 0, sizeof(NodeE)); old->id = id;                       // replace the oldest
  return old;
}

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
  // Slot = CAD (2 symbols + 0.5) + propagation, turnaround and MAC (7.6 ms): computeSlotTimeMsec()
  slotMs = 2.5f * (float)(1UL << p.sf) / p.bw + 7.6f;
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

// ======================= Battery (MAX17048) =================================
bool battOk = false;
float battSoc = 0, battV = 0, battRate = 0;                          // %, V, %/h

uint16_t maxRead16(uint8_t reg) {                                    // 16-bit big-endian registers
  Wire.beginTransmission(MAX17048_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFFFF;
  if (Wire.requestFrom((uint8_t)MAX17048_ADDR, (uint8_t)2) != 2) return 0xFFFF;
  uint16_t hi = Wire.read();
  return (hi << 8) | Wire.read();
}

void battRead() {
  if (!battOk) return;
  uint16_t v = maxRead16(0x02), s = maxRead16(0x04), c = maxRead16(0x16);
  if (v == 0xFFFF || s == 0xFFFF) return;                            // failed read: keep the previous value
  float prev = battSoc;
  battV    = v * 78.125e-6f;                                         // VCELL: 78.125 uV/LSB
  battSoc  = s / 256.0f;                                             // SOC: 1/256 %/LSB
  battRate = (int16_t)c * 0.208f;                                    // CRATE: 0.208 %/h per LSB, signed
  if (prev >= BATT_LOW_PCT && battSoc < BATT_LOW_PCT) sysEv("Low battery: " + String(battSoc, 0) + " %");
}

void battInit() {
  Wire.begin(PIN_SDA, PIN_SCL, 400000);
  Wire.beginTransmission(MAX17048_ADDR);
  battOk = Wire.endTransmission() == 0;
  if (battOk) { battSoc = 100; battRead(); }                         // 100 avoids a false warning at boot
}

// Power source. With /PGOOD it is immediate. Without /PGOOD the MAX17048 rate is used:
// on battery there is always discharge (about -2 to -5 %/h with the modem active); on USB the
// cell charges or stays idle. The chip averages the rate, so the change takes minutes.
bool usbPower() {
  static bool usb = true;
  if (PIN_PGOOD >= 0) return digitalRead(PIN_PGOOD) == LOW;
  if (!battOk) return true;                                          // no fuel gauge: assume USB
  if (usb && battRate < -1.5f) usb = false;                          // hysteresis between -1.5 and -0.8 %/h
  else if (!usb && battRate > -0.8f) usb = true;
  return usb;
}

void battStatus() {
  if (!battOk) { io.printf("+BATT:NOT_DETECTED,%s\r\n", usbPower() ? "USB" : "BATTERY"); return; }
  io.printf("+BATT:%.1f%%,%.3fV,%+.1f%%/h,%s,%s\r\n", min(battSoc, 100.0f), battV, battRate,
            battRate > 0.5f ? "CHARGING" : battRate < -0.5f ? "DISCHARGING" : "IDLE",
            usbPower() ? "USB" : "BATTERY");
}

// ======================= LEDs ===============================================
bool ledsOn = true;
uint16_t unread = 0;                                                 // unread text messages

// ======================= Echoes of own messages (implicit ACK) ==============
struct OwnTx { uint32_t id, t; uint8_t n, rb[8]; };                  // distinct relays that repeated it
OwnTx own[8]; uint8_t ownIdx; uint32_t lastTxId;
uint32_t tMsgLed = 0;                                                // arrival of the last message

void ledsLoop() {
  uint32_t t = millis();
  bool red = false, green = false;
  if (ledsOn) {
    red = usbPower() ? true : (t % 3000) < 60;                       // solid on USB, blink on battery
    if (tMsgLed && t - tMsgLed < 600) green = ((t - tMsgLed) / 100) % 2 == 0;   // 3 fast blinks
    else if (unread) green = (t % 2000) < 80;                        // reminder: unread messages
  }
  digitalWrite(PIN_LED_RED, red ? HIGH : LOW);
  digitalWrite(PIN_LED_GREEN, green ? HIGH : LOW);
}

// ======================= TX =================================================
bool txRaw(uint8_t* pkt, size_t n) {                                 // CAD + transmission of an already built frame
  for (int i = 0; i < 5 && radio.scanChannel() == RADIOLIB_LORA_DETECTED; i++)
    delay(100 + esp_random() % 400);
  int e = radio.transmit(pkt, n);
  radio.startReceive();
  return e == RADIOLIB_ERR_NONE;
}

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
  seen[seenIdx++ & 31] = ((uint64_t)cfg.node << 32) | id;           // ignore relays' echoes in the duplicate filter
  bool ok = txRaw(pkt, 16 + k);
  if (ok) {
    io.printf("+TX:%08x\r\n", (unsigned)id);
    lastTxId = id;
    OwnTx& o = own[ownIdx++ & 7]; o.id = id; o.t = millis(); o.n = 0;   // to recognise its echoes
  }
  return ok;
}

bool sendNodeInfo() {                                               // protobuf User
  String id = hexId(cfg.node);
  uint8_t u[64]; size_t n = 0;
  auto str = [&](uint8_t tag, const char* s) { u[n++] = tag; u[n++] = strlen(s); memcpy(u + n, s, strlen(s)); n += strlen(s); };
  str(0x0A, id.c_str()); str(0x12, cfg.ln); str(0x1A, cfg.sn);      // id, long_name, short_name
  u[n++] = 0x28; u[n++] = 0xFF; u[n++] = 0x01;                      // hw_model = PRIVATE_HW (255)
  const uint8_t ROLE[] = {1, 0, 2};                                  // CLIENT_MUTE, CLIENT, ROUTER
  if (ROLE[cfg.relay]) { u[n++] = 0x38; u[n++] = ROLE[cfg.relay]; }  // role (field 7; CLIENT = 0 is omitted)
  u[n++] = 0x48; u[n++] = 0x01;                                      // is_unmessagable = true: no DMs
  return sendData(PORT_NODEINFO, u, n);
}

// ======================= Rebroadcast (managed flooding) =====================
// Same contention window as RadioInterface::getTxDelayMsecWeighted():
//   CW = map(SNR, -20..10 dB, 3..8)
//   client: 2*CWmax*slot + random(0, 2^CW)*slot    (low SNR = short wait)
//   router: random(0, 2*CW)*slot                   (always before clients)
void scheduleRelay(const uint8_t* p, size_t n, float snr, uint32_t from, uint32_t id) {
  Pend* q = nullptr;
  for (auto& x : pend) if (!x.used) { q = &x; break; }
  if (!q) { stDropped++; return; }                                   // queue full
  memcpy(q->buf, p, n); q->len = n; q->from = from; q->id = id;
  q->buf[12] = (p[12] & 0xF8) | ((p[12] & 0x07) - 1);                // hop_limit - 1 (hop_start and flags unchanged)
  q->buf[14] = 0;                                                    // next_hop: no preference
  q->buf[15] = cfg.node & 0xFF;                                      // relay_node: this node
  long cw = constrain(map((long)snr, -20, 10, 3, 8), 3, 9);
  uint32_t d = cfg.relay == 2 ? random(0, 2 * cw) * slotMs
                              : 2 * 8 * slotMs + random(0, 1L << cw) * slotMs;
  q->due = millis() + d; q->used = true;
}

void cancelRelay(uint32_t from, uint32_t id) {                       // another node already repeated it
  if (cfg.relay != 1) return;                                        // routers never cancel
  for (auto& x : pend) if (x.used && x.from == from && x.id == id) { x.used = false; stCanceled++; }
}

void serviceRelay() {
  for (auto& x : pend) {
    if (!x.used || (int32_t)(millis() - x.due) < 0) continue;
    x.used = false;
    if (txRaw(x.buf, x.len)) {
      stRelayed++;
      if (rawMode) io.printf("+RELAY:!%08x,%08x,%u\r\n", (unsigned)x.from, (unsigned)x.id, x.buf[12] & 7);
    }
    return;                                                          // one per loop iteration
  }
}

// ======================= RX =================================================
void receive() {
  if (digitalRead(PIN_DIO1) != HIGH) return;                        // DIO1 high = RX done
  uint8_t p[256]; size_t n = radio.getPacketLength(); if (n > 255) n = 255;
  int st = radio.readData(p, n); float rssi = radio.getRSSI(), snr = radio.getSNR();
  radio.startReceive();
  if (rawMode) io.printf("+RAW:%u,%d,%.0f,%.1f,%02X\r\n", (unsigned)n, st, rssi, snr, n > 13 ? p[13] : 0);
  if (st != RADIOLIB_ERR_NONE || n < 16) return;
  uint32_t to, from, id; memcpy(&to, p, 4); memcpy(&from, p + 4, 4); memcpy(&id, p + 8, 4);
  if (from == cfg.node) {                                            // echo: another node repeated one of ours
    uint8_t rb = p[15], hlE = p[12] & 7;
    for (auto& o : own) if (o.id == id && rb != (cfg.node & 0xFF)) {
      for (uint8_t k = 0; k < o.n; k++) if (o.rb[k] == rb) return;   // this relay was already counted
      if (o.n < sizeof(o.rb)) o.rb[o.n++] = rb;
      const char* who = "";                                          // name of the node whose NodeNum ends in rb
      for (auto& ne : nodes) if (ne.id && (ne.id & 0xFF) == rb && ne.sn[0]) { who = ne.sn; break; }
      io.printf("+ECO:%08x,relay=%02X,%s,%.0f,%.1f,%u\r\n", (unsigned)id, rb, who, rssi, snr, hlE);
      char b[8]; snprintf(b, sizeof(b), "%02X", rb);
      pushEv("\"t\":\"eco\",\"id\":\"" + String(id, HEX) + "\",\"rb\":\"" + b + "\",\"sn\":" + jStr(who) +
             ",\"n\":" + String(o.n) + ",\"hl\":" + String(hlE) + ",\"rssi\":" + String(rssi, 0));
    }
    return;
  }
  uint64_t key = ((uint64_t)from << 32) | id;
  for (uint64_t s : seen) if (s == key) { cancelRelay(from, id); return; }   // duplicate: someone already repeated it
  seen[seenIdx++ & 31] = key;
  int hl = p[12] & 7, hs = p[12] >> 5, hops = hs ? hs - hl : -1;
  // Rebroadcast: like NextHopRouter::perhapsRebroadcast(). Works for any channel (no decryption needed)
  if (cfg.relay && to != cfg.node && hl > 0 && id && (p[14] == 0 || p[14] == (cfg.node & 0xFF)))
    scheduleRelay(p, n, snr, from, id);
  if (n < 20 || p[13] != chHash) return;                             // other channel: rebroadcast only
  uint8_t* d = p + 16; size_t L = n - 16; ctr(d, L, id, from);
  uint32_t port = 0, f, v; const uint8_t *s, *pl = nullptr; size_t pn = 0, i = 0;
  while (i < L) {
    if (!pbNext(d, L, i, f, v, s)) return;                          // different PSK or corrupted frame
    if (f == 1 && !s) port = v; else if (f == 2 && s) { pl = s; pn = v; }
  }
  if (!pl) return;
  NodeE* ne = nodeGet(from); ne->rssi = rssi; ne->snr = snr; ne->seen = millis() | 1;
  String meta = "\"from\":\"" + hexId(from) + "\",\"rssi\":" + String(rssi, 0) + ",\"snr\":" + String(snr, 1);
  if (port == PORT_TEXT) {
    unread++; tMsgLed = millis() | 1;                                // green LED
    io.printf("+RX:!%08x,!%08x,%.0f,%.1f,%d,", (unsigned)from, (unsigned)to, rssi, snr, hops);
    io.write(pl, pn); io.print("\r\n");
    logEv("\"t\":\"rx\"," + meta + ",\"to\":\"" + hexId(to) + "\",\"hops\":" + String(hops) +
           ",\"sn\":" + jStr(ne->sn) + ",\"txt\":" + jStr((const char*)pl, pn));
  } else if (port == PORT_NODEINFO) {
    size_t j = 0;
    while (j < pn && pbNext(pl, pn, j, f, v, s)) {
      if (f == 2 && s) { size_t c = min((size_t)v, sizeof(ne->ln) - 1); memcpy(ne->ln, s, c); ne->ln[c] = 0; }
      if (f == 3 && s) { size_t c = min((size_t)v, sizeof(ne->sn) - 1); memcpy(ne->sn, s, c); ne->sn[c] = 0; }
    }
    io.printf("+NODE:!%08x,%.0f,%.1f,%s,%s\r\n", (unsigned)from, rssi, snr, ne->sn, ne->ln);
    pushEv("\"t\":\"node\"," + meta + ",\"sn\":" + jStr(ne->sn) + ",\"ln\":" + jStr(ne->ln));
  } else if (rawMode) io.printf("+PKT:!%08x,%u,%u\r\n", (unsigned)from, (unsigned)port, (unsigned)pn);
}

// ======================= WiFi ===============================================
void saveCfg() { nvs.putBytes("cfg", &cfg, sizeof(cfg)); }

void startPortal() {
  portal = true; wifiUp = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);                                            // STA active to scan networks
  char ap[24]; snprintf(ap, sizeof(ap), "MeshModem-%04X", (unsigned)(cfg.node & 0xFFFF));
  WiFi.softAP(ap);
  dns.start(53, "*", WiFi.softAPIP());                               // every name resolves to the portal
  server.close(); server.begin();
  io.printf("+WIFI:PORTAL,%s,%s\r\n", ap, WiFi.softAPIP().toString().c_str());
  sysEv(String("WiFi portal active: network ") + ap);
}

void wifiStart() {
  if (portal) { dns.stop(); WiFi.softAPdisconnect(true); portal = false; }
  if (!wSsid.length()) { startPortal(); return; }
  wifiUp = false;
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wSsid.c_str(), wPass.c_str());
  server.close(); server.begin();
  io.printf("+WIFI:CONNECTING,%s\r\n", wSsid.c_str());
}

void wifiLoop() {
  if (tWifiPending && millis() - tWifiPending > 1500) { tWifiPending = 0; wifiStart(); }
  if (portal) { dns.processNextRequest(); return; }
  bool c = WiFi.status() == WL_CONNECTED;
  if (c && !wifiUp) {
    wifiUp = true;
    MDNS.begin(HOSTNAME); MDNS.addService("http", "tcp", 80);
    configTzTime(TZ_STR, "pool.ntp.org", "time.google.com");
    String ip = WiFi.localIP().toString();
    io.printf("+WIFI:UP,%s,http://%s/ (http://%s.local/)\r\n", wSsid.c_str(), ip.c_str(), HOSTNAME);
    sysEv("WiFi connected to " + wSsid + ", IP " + ip);
  } else if (!c && wifiUp) {
    wifiUp = false; MDNS.end();
    io.print("+WIFI:DOWN\r\n");
  }
}

void bootButton() {
  static uint32_t tDown = 0;
  if (digitalRead(PIN_BOOT) == LOW) {
    if (!tDown) tDown = millis() | 1;
    else if (!portal && millis() - tDown >= PORTAL_HOLD_MS) startPortal();
  } else tDown = 0;
}

void wifiStatus() {
  if (portal) io.printf("+WIFI:PORTAL,%s,%s\r\n", WiFi.softAPSSID().c_str(), WiFi.softAPIP().toString().c_str());
  else if (wifiUp) io.printf("+WIFI:UP,%s,%s,%d dBm,http://%s.local/\r\n", wSsid.c_str(),
                             WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(), HOSTNAME);
  else io.printf("+WIFI:%s,%s\r\n", wSsid.length() ? "CONNECTING" : "NOT_CONFIGURED", wSsid.c_str());
}

// ======================= AT commands ========================================
void info() {
  const Preset& p = PRESETS[cfg.preset];
  io.printf("+NODE:!%08x\r\n+NAME:%s,%s\r\n", (unsigned)cfg.node, cfg.ln, cfg.sn);
  io.printf("+PRESET:%s,BW=%.1f,SF=%u,CR=4/%u,HASH=%02X\r\n", p.name, p.bw, p.sf, p.cr, chHash);
  io.printf("+BAND:%.3f,%.3f\r\n+FREQ:%.4f,%s,SLOT=%u/%u\r\n", cfg.f0, cfg.f1, freq,
            cfg.fov > 0 ? "MANUAL" : "AUTO", (unsigned)slot + 1, (unsigned)nCh);
  io.printf("+HOP:%u\r\n+PWR:%d\r\n+RAW:%d\r\n", cfg.hop, cfg.pwr, rawMode);
  battStatus();
  io.printf("+RELAY:%s,RELAYED=%lu,CANCELED=%lu,DROPPED=%lu,SLOT=%.1fms\r\n", RELAY_NAME[cfg.relay],
            (unsigned long)stRelayed, (unsigned long)stCanceled, (unsigned long)stDropped, slotMs);
  io.printf("+BOOT:%s,UPTIME=%lus\r\n", resetReason(), (unsigned long)(millis() / 1000));
  if (fsOk) io.printf("+LOG:%u bytes used of %u\r\n", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else io.print("+LOG:NO_FLASH\r\n");
  wifiStatus();
}

void listNodes() {
  for (auto& n : nodes) if (n.id)
    io.printf("+NODES:!%08x,%s,%s,%.0f,%.1f,%lus\r\n", (unsigned)n.id, n.sn, n.ln, n.rssi, n.snr,
              (unsigned long)((millis() - n.seen) / 1000));
}

void at(String s) {
  s.trim(); if (!s.length()) return;
  String u = s; u.toUpperCase();
  int eq = s.indexOf('='); String a = eq >= 0 ? s.substring(eq + 1) : ""; a.trim();
  bool ok = true;

  if (u == "AT") {}
  else if (u == "AT+HELP" || u == "AT+HELP?") io.print(HELP);
  else if (u == "ATI" || u == "AT+INFO?") info();
  else if (u.startsWith("AT+SEND=")) {
    ok = a.length() > 0 && sendData(PORT_TEXT, (const uint8_t*)a.c_str(), a.length());
    if (ok) logEv("\"t\":\"tx\",\"id\":\"" + String(lastTxId, HEX) + "\",\"sn\":" + jStr(cfg.sn) + ",\"txt\":" + jStr(a.c_str()));
  }
  else if (u == "AT+NI") ok = sendNodeInfo();
  else if (u == "AT+NODES") listNodes();
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
  else if (u == "AT+PRESET=?") { for (int i = 0; i < NPRE; i++) io.printf("+PRESET:%d,%s\r\n", i, PRESETS[i].name); }
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
  else if (u == "AT+RELAY?") io.printf("+RELAY:%s\r\n", RELAY_NAME[cfg.relay]);
  else if (u == "AT+BATT?" || u == "AT+BATT") battStatus();
  else if (u.startsWith("AT+LED=")) { ok = a == "0" || a == "1"; if (ok) ledsOn = a == "1"; }
  else if (u.startsWith("AT+RELAY=")) {
    int r = a.toInt(); ok = isDigit(a[0]) && r <= 2;
    if (ok) { cfg.relay = r; if (!r) for (auto& x : pend) x.used = false; sendNodeInfo(); }   // announce the role
  }
  else if (u == "AT+SAVE") { saveCfg(); sysEv("Configuration saved to flash"); }
  else if (u == "AT+WIFI?") wifiStatus();
  else if (u.startsWith("AT+WIFI=")) {
    int c = a.indexOf(',');
    ok = c > 0;
    if (ok) {
      wSsid = a.substring(0, c); wPass = a.substring(c + 1);
      nvs.putString("ssid", wSsid); nvs.putString("pass", wPass);
      tWifiPending = millis() | 1;                                   // connect after replying
    }
  }
  else if (u == "AT+PORTAL") startPortal();
  else if (u == "AT+LOG" || u.startsWith("AT+LOG=")) {
    int n = u == "AT+LOG" ? 20 : a.toInt();
    ok = fsOk && n > 0 && n <= 500;
    if (ok) { std::vector<String> v; logTail(n, v); for (auto& l : v) io.printf("+LOG:%s\r\n", l.c_str()); }
  }
  else if (u == "AT+LOGCLR") {
    ok = fsOk;
    if (ok) { LittleFS.remove(LOG_FILE); LittleFS.remove(LOG_OLD); sysEv("History erased"); }
  }
  else if (u == "AT&F") { cfg = DEF; nvs.remove("cfg"); applyRadio(); sendNodeInfo(); }
  else if (u == "AT+RST") { io.print("OK\r\n"); Serial.flush(); delay(100); ESP.restart(); }
  else ok = false;

  io.print(ok ? "OK\r\n" : "ERROR\r\n");
}

// ======================= Embedded web pages =================================
const char PAGE_PORTAL[] = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><title>Mesh_Modem_Web: WiFi</title>
<style>
:root{--bg:#0f1626;--card:#172138;--ink:#e7ecf5;--mut:#a3aec4;--line:#2b3753;--acc:#3cc3c4;color-scheme:dark}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.5 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:420px;margin:0 auto;padding:28px 18px}
h1{font-size:1.4rem;margin:0 0 4px}p{color:var(--mut);margin:0 0 18px}
form{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px;display:grid;gap:12px}
label{font-size:.85rem;font-weight:600;color:var(--mut);display:grid;gap:5px}
input{font:inherit;padding:10px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--ink)}
button{font:inherit;font-weight:600;padding:11px;border:0;border-radius:8px;background:var(--acc);color:#0d1322;cursor:pointer}
#nets{display:grid;gap:4px;max-height:220px;overflow:auto}
#nets button{background:transparent;color:var(--ink);border:1px solid var(--line);text-align:left;display:flex;justify-content:space-between;font-weight:500}
small{color:var(--mut)}
</style></head><body><main>
<h1>Connect to your WiFi</h1><p>Pick your home network. The modem keeps working over LoRa and the serial terminal in the meantime.</p>
<form method="post" action="/wifi">
<div id="nets"><small>Scanning networks…</small></div>
<label>Network (SSID)<input name="ssid" id="ssid" required maxlength="32" autocomplete="off"></label>
<label>Password<input name="pass" type="password" maxlength="63"></label>
<button type="submit">Save and connect</button>
</form></main>
<script>
fetch('/scan').then(r=>r.json()).then(l=>{const d=document.getElementById('nets');d.innerHTML='';
const seen=new Set();l.sort((a,b)=>b.r-a.r).forEach(n=>{if(!n.s||seen.has(n.s))return;seen.add(n.s);
const b=document.createElement('button');b.type='button';b.innerHTML='<span></span><small></small>';
b.firstChild.textContent=n.s;b.lastChild.textContent=n.r+' dBm';b.onclick=()=>{document.getElementById('ssid').value=n.s};d.appendChild(b)});
if(!seen.size)d.innerHTML='<small>No networks found. Type the name manually.</small>'}).catch(()=>{});
</script></body></html>)HTML";

const char PAGE_MAIN[] = R"HTML(<!DOCTYPE html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover"><title>Mesh_Modem_Web</title>
<style>
:root{--bg:#0d1322;--card:#151e33;--ink:#e7ecf5;--mut:#a3aec4;--line:#283450;--acc:#3cc3c4;--me:#a893ff;--sys:#76819a;--warn:#f2b53c;
--bad:#ff6b7a;--log:#10182a;--mono:ui-monospace,"Cascadia Mono",Consolas,"DejaVu Sans Mono",monospace;color-scheme:dark}
.bat{display:inline-block;width:22px;height:11px;border:1.5px solid var(--mut);border-radius:2px;position:relative;margin-right:6px;vertical-align:-1px}
.bat::after{content:"";position:absolute;right:-4px;top:2.5px;width:2px;height:3px;background:var(--mut);border-radius:0 1px 1px 0}
.bat i{position:absolute;left:1px;top:1px;bottom:1px;border-radius:1px}
*{box-sizing:border-box}
html,body{height:100%}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
padding:env(safe-area-inset-top,0) 0 env(safe-area-inset-bottom,0)}
.app{height:100%;display:grid;grid-template-rows:auto 1fr;max-width:1200px;margin:0 auto}
header{display:flex;flex-wrap:wrap;align-items:center;gap:8px 14px;padding:12px 16px;border-bottom:1px solid var(--line)}
header h1{font-size:1.05rem;margin:0;margin-right:auto}
.pill{font-size:.8rem;padding:3px 10px;border-radius:999px;background:var(--card);border:1px solid var(--line);color:var(--mut);white-space:nowrap}
.pill b{color:var(--ink);font-weight:600}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--acc);margin-right:6px;vertical-align:1px}
.dot.off{background:var(--warn)}
.body{display:grid;grid-template-columns:1fr 300px;min-height:0}
.chat{display:grid;grid-template-rows:1fr auto;min-height:0;border-right:1px solid var(--line)}
#log{overflow-y:auto;padding:12px 16px;background:var(--log);font:14px/1.55 var(--mono);word-wrap:break-word}
.l{white-space:pre-wrap}
.t{color:var(--sys)}
.n{font-weight:700}
.me{color:var(--me)}
.s{color:var(--sys)}
.m{color:var(--sys);font-size:.78em;margin-left:6px}
form.in{display:flex;gap:8px;padding:10px 12px;border-top:1px solid var(--line);background:var(--card)}
form.in input{flex:1;min-width:0;font:15px var(--mono);padding:10px 12px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--ink)}
button{font:inherit;font-weight:600;border:0;border-radius:8px;padding:9px 14px;background:var(--acc);color:#0d1322;cursor:pointer}
button.sec{background:transparent;color:var(--ink);border:1px solid var(--line)}
button:disabled{opacity:.5;cursor:default}
#cnt{align-self:center;font:12px var(--mono);color:var(--sys);min-width:34px;text-align:right}
aside{overflow-y:auto;padding:14px 16px;display:grid;gap:18px;align-content:start}
aside h2{font-size:.95rem;margin:0 0 8px}
.nodes{display:grid;gap:6px}
.node{display:grid;grid-template-columns:auto 1fr;gap:0 8px;font-size:.85rem}
.node .sn{font:700 13px var(--mono);grid-row:span 2;align-self:center;min-width:44px}
.node .d{color:var(--mut);font-size:.78rem}
.cfg{display:grid;gap:10px}
.cfg label{display:grid;gap:4px;font-size:.78rem;font-weight:600;color:var(--mut)}
.cfg input,.cfg select{font:inherit;font-size:.9rem;padding:7px 9px;border:1px solid var(--line);border-radius:7px;background:var(--bg);color:var(--ink);width:100%}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.btns{display:flex;flex-wrap:wrap;gap:8px}
.hint{font-size:.78rem;color:var(--mut);margin:0}
.lnk{font-weight:600;font-size:.95rem;padding:9px 14px;border:1px solid var(--line);border-radius:8px;color:var(--ink);text-decoration:none}
button:focus-visible,input:focus-visible,select:focus-visible{outline:3px solid var(--me);outline-offset:1px}
[hidden]{display:none!important}
.cfg input,.cfg select{font-weight:400}
@media(max-width:760px){.app{height:auto;min-height:100%}.body{display:block}
.chat{height:72vh;height:72dvh;border-right:0}aside{border-top:1px solid var(--line);overflow:visible}}
</style></head><body><div class="app">
<header><h1>Mesh_Modem_Web</h1>
<span class="pill" id="p-me">—</span><span class="pill" id="p-rf">—</span><span class="pill" id="p-rly">—</span><span class="pill" id="p-bat" hidden>—</span><span class="pill" id="p-net"><span class="dot"></span>—</span></header>
<div class="body">
<section class="chat">
<div id="log" aria-live="polite"></div>
<form class="in" id="f-in" autocomplete="off">
<input id="msg" maxlength="232" placeholder="Message to the channel, or /AT+... for a command">
<span id="cnt">0</span><button id="b-send">Send</button></form>
</section>
<aside>
<div><h2>History</h2><div class="btns"><a class="lnk" href="/log.jsonl">Download</a><button type="button" class="sec" id="b-clr">Erase history</button></div>
<p class="hint" style="margin-top:6px">Stored in the modem flash; survives resets and power cuts.</p></div>
<div><h2>Heard nodes</h2><div class="nodes" id="nodes"><p class="hint">No nodes heard yet.</p></div></div>
<div><h2>Configuration</h2>
<form class="cfg" id="f-cfg">
<div class="row2"><label>Short name<input id="c-sn" maxlength="4"></label><label>NodeNum<input id="c-node" maxlength="9"></label></div>
<label>Long name<input id="c-ln" maxlength="39"></label>
<label>Preset<select id="c-preset"></select></label>
<label>Region<select id="c-reg">
<option value="915,928">ANZ (915 to 928 MHz)</option><option value="902,928">US (902 to 928 MHz)</option>
<option value="902,907.5">BR_902 (902 to 907.5 MHz)</option><option value="869.4,869.65">EU_868 (869.4 to 869.65 MHz)</option>
<option value="custom">Other band</option></select></label>
<div class="row2" id="r-band" hidden><label>Start [MHz]<input id="c-f0" type="number" step="0.001"></label><label>End [MHz]<input id="c-f1" type="number" step="0.001"></label></div>
<label>Manual frequency [MHz], 0 = automatic<input id="c-fov" type="number" step="0.0001" min="0"></label>
<label>Rebroadcast<select id="c-relay"><option value="0">Mute (never repeats)</option><option value="1">Client (default)</option><option value="2">Router (always repeats)</option></select></label>
<div class="row2"><label>Hops (0 to 7)<input id="c-hop" type="number" min="0" max="7"></label><label>Power [dBm]<input id="c-pwr" type="number" min="-9" max="22"></label></div>
<div class="btns"><button type="submit">Apply</button><button type="button" class="sec" id="b-save">Save to flash</button><button type="button" class="sec" id="b-ni">Send NodeInfo</button></div>
<p class="hint">“Apply” changes the configuration immediately. Use “Save to flash” to keep it after a restart.</p>
</form></div>
</aside></div></div>
<script>
const $=id=>document.getElementById(id);
let since=0,info=null,myId='';
const names={};
function hue(s){let h=0;for(const c of s)h=(h*31+c.charCodeAt(0))%360;return h}
function hhmm(ts,up){if(!ts&&up!==undefined){const h=Math.floor(up/3600),m=Math.floor(up/60)%60;return '+'+h+'h'+String(m).padStart(2,'0')}
const d=ts?new Date(ts*1000):new Date(),n=new Date(),hm=d.toTimeString().slice(0,5);
return d.toDateString()===n.toDateString()?hm:d.toLocaleDateString('en',{month:'short',day:'numeric'})+' '+hm}
function line(parts){const log=$('log'),stick=log.scrollTop+log.clientHeight>=log.scrollHeight-30;
const div=document.createElement('div');div.className='l';
for(const [txt,cls,style] of parts){const sp=document.createElement('span');sp.textContent=txt;if(cls)sp.className=cls;if(style)sp.style.cssText=style;div.appendChild(sp)}
log.appendChild(div);while(log.children.length>500)log.removeChild(log.firstChild);if(stick)log.scrollTop=log.scrollHeight;return div}
const txEl={};
function nick(id,sn){const n=sn||names[id]||id;return['<'+n+'>','n','color:hsl('+hue(id)+' 60% 50%)']}
function addEv(e,hist){const t=['['+hhmm(e.ts,e.up)+'] ','t'];
if(e.t==='rx'){if(e.sn)names[e.from]=e.sn;
const meta=e.rssi+' dBm, '+e.snr+' dB'+(e.hops>=0?', '+e.hops+(e.hops===1?' hop':' hops'):'')+(e.to!=='!ffffffff'?', to '+e.to:'');
line([t,nick(e.from,e.sn),[' '+e.txt],[meta,'m']])}
else if(e.t==='tx'){const d=line([t,['<'+e.sn+'>','n me'],[' '+e.txt],[hist?'':'waiting for echo…','m eco']]);
if(e.id&&!hist){const x=txEl[e.id]={sp:d.lastChild,who:[]};
setTimeout(()=>{if(!x.who.length){x.sp.textContent='✗ no echo: nobody in range repeated it';x.sp.style.color='var(--warn)'}},15000)}}
else if(e.t==='eco'){const x=txEl[e.id];const w=(e.rb==='00'?'a node with firmware older than 2.6':(e.sn||e.rb))+(info&&e.hl<info.hop-1?' (2nd hop)':'');
if(x){if(x.who.includes(w))return;x.who.push(w);x.sp.textContent='✓ relayed by '+x.who.length+': '+x.who.join(', ');x.sp.style.color='var(--acc)'}
else line([t,['-- echo of your message '+e.id+' relayed by '+w,'s']])}
else if(e.t==='node'){if(e.sn)names[e.from]=e.sn;line([t,['*** '+(e.sn||e.from)+' ('+(e.ln||'no name')+') '+e.from+' announced itself, '+e.rssi+' dBm','s']])}
else line([t,['-- '+e.txt,'s']])}
async function poll(){try{const j=await(await fetch('/api/ev?since='+since+'&seen='+(document.visibilityState==='visible'?1:0),{cache:'no-store'})).json();
if(j.next<since){since=0;line([['['+hhmm(0)+'] ','t'],['-- The modem restarted; the history is still stored','s','color:var(--warn)']]);setTimeout(poll,300);return}
j.ev.forEach(addEv);since=j.next;online(true)}catch(e){online(false)}setTimeout(poll,1000)}
function online(ok){const p=$('p-net');if(!ok){p.innerHTML='<span class="dot off"></span>no connection to the modem';return}
if(info)p.innerHTML='<span class="dot"></span>'+(info.ssid||'')+(info.wrssi?' '+info.wrssi+' dBm':'')}
async function api(cmd){const r=await fetch('/api/at',{method:'POST',body:new URLSearchParams({cmd})});return r.text()}
function ago(s){return s<60?s+' s ago':s<3600?Math.round(s/60)+' min ago':Math.round(s/3600)+' h ago'}
async function loadInfo(fill){try{info=await(await fetch('/api/info',{cache:'no-store'})).json()}catch(e){return}
myId=info.node;$('p-me').innerHTML='<b></b> '+info.node;$('p-me').firstChild.textContent=info.sn;
$('p-rf').innerHTML='<b>'+info.pname+'</b> '+info.freq.toFixed(3)+' MHz, '+info.hop+' hops';
$('p-rly').innerHTML='<b>'+['Mute','Client','Router'][info.relay]+'</b> '+info.rly+' relayed, '+info.rlc+' canceled';
const pb=$('p-bat');pb.hidden=!info.batt;
if(info.batt){const s=Math.max(0,Math.min(100,info.soc)),col=s<15?'var(--bad)':s<40?'var(--warn)':'var(--acc)';
const st=info.usb?(info.crate>0.5?' USB ▲ charging':' USB'):' on battery';
pb.innerHTML='<span class="bat"><i style="width:'+(s*0.18).toFixed(1)+'px;background:'+col+'"></i></span><b>'+Math.round(s)+' %</b> '+info.vbat.toFixed(2)+' V'+st;
pb.title='Rate: '+info.crate.toFixed(1)+' %/h'}
online(true);
const nl=$('nodes');const ns=info.nodes.sort((a,b)=>a.ago-b.ago);
if(ns.length){nl.innerHTML='';ns.forEach(n=>{if(n.sn)names[n.id]=n.sn;const d=document.createElement('div');d.className='node';
d.innerHTML='<span class="sn"></span><span></span><span class="d"></span>';d.children[0].textContent=n.sn||'?';
d.children[0].style.color='hsl('+hue(n.id)+' 60% 50%)';d.children[1].textContent=n.ln||n.id;
d.children[2].textContent=n.id+', '+n.rssi+' dBm, '+n.snr+' dB, '+ago(n.ago);nl.appendChild(d)})}
if(fill){$('c-sn').value=info.sn;$('c-ln').value=info.ln;$('c-node').value=info.node;
const sel=$('c-preset');if(!sel.options.length)info.presets.forEach((p,i)=>sel.add(new Option(p,i)));sel.value=info.preset;
const band=info.f0.toFixed(3).replace(/\.?0+$/,'')+','+info.f1.toFixed(3).replace(/\.?0+$/,'');
const opt=[...$('c-reg').options].find(o=>o.value===band);$('c-reg').value=opt?band:'custom';
$('c-f0').value=info.f0;$('c-f1').value=info.f1;$('r-band').hidden=!!opt;
$('c-fov').value=info.fov;$('c-hop').value=info.hop;$('c-pwr').value=info.pwr;$('c-relay').value=info.relay}}
$('c-reg').onchange=()=>{const v=$('c-reg').value;$('r-band').hidden=v!=='custom';if(v!=='custom'){const[a,b]=v.split(',');$('c-f0').value=a;$('c-f1').value=b}};
async function run(cmd){const r=(await api(cmd)).trim();const ok=/OK$/.test(r);
line([['['+hhmm(0)+'] ','t'],['-- '+cmd+(ok?'':' → ERROR'),'s',ok?'':'color:var(--warn)']]);return r}
$('f-cfg').onsubmit=async e=>{e.preventDefault();const b=e.submitter;b.disabled=true;const c=[];
const sn=$('c-sn').value.trim(),ln=$('c-ln').value.trim();
if(sn!==info.sn||ln!==info.ln)c.push('AT+NAME='+ln+','+sn);
if($('c-node').value.trim().toLowerCase()!==info.node)c.push('AT+NODE='+$('c-node').value.trim());
if(+$('c-preset').value!==info.preset)c.push('AT+PRESET='+$('c-preset').value);
const f0=+$('c-f0').value,f1=+$('c-f1').value;if(Math.abs(f0-info.f0)>1e-4||Math.abs(f1-info.f1)>1e-4)c.push('AT+BAND='+f0+','+f1);
if(Math.abs(+$('c-fov').value-info.fov)>1e-5)c.push('AT+FREQ='+(+$('c-fov').value));
if(+$('c-hop').value!==info.hop)c.push('AT+HOP='+$('c-hop').value);
if(+$('c-pwr').value!==info.pwr)c.push('AT+PWR='+$('c-pwr').value);
if(+$('c-relay').value!==info.relay)c.push('AT+RELAY='+$('c-relay').value);
if(!c.length)line([['['+hhmm(0)+'] ','t'],['-- Nothing to apply','s']]);
for(const x of c)await run(x);await loadInfo(true);b.disabled=false};
$('b-save').onclick=()=>run('AT+SAVE');
$('b-ni').onclick=()=>run('AT+NI');
$('msg').oninput=()=>$('cnt').textContent=new TextEncoder().encode($('msg').value).length;
$('f-in').onsubmit=async e=>{e.preventDefault();const v=$('msg').value.trim();if(!v)return;
const b=$('b-send');b.disabled=true;
if(v[0]==='/'){const cmd=v.slice(1);const r=await api(cmd);line([['['+hhmm(0)+'] ','t'],['> '+cmd,'s']]);
r.trim().split(/\r?\n/).forEach(x=>line([['        ','t'],[x,'s']]));loadInfo(/^AT\+(NAME|NODE|PRESET|BAND|FREQ|HOP|PWR)|^AT&F/i.test(cmd))}
else if(new TextEncoder().encode(v).length>232)line([['['+hhmm(0)+'] ','t'],['-- Message exceeds 232 bytes','s','color:var(--warn)']]);
else{const r=await api('AT+SEND='+v);if(!/OK\s*$/.test(r))line([['['+hhmm(0)+'] ','t'],['-- Transmission failed','s','color:var(--warn)']])}
$('msg').value='';$('cnt').textContent='0';b.disabled=false;$('msg').focus()};
$('b-clr').onclick=async()=>{if(!confirm('Erase the whole history stored in the modem?'))return;await run('AT+LOGCLR');$('log').textContent=''};
async function loadLog(){try{const j=await(await fetch('/api/log?n=300',{cache:'no-store'})).json();
j.log.forEach(e=>{if(e.t==='sys')line([['['+hhmm(e.ts,e.up)+'] ','t'],['-- '+e.txt,'s']]);else addEv(e,true)});
if(j.log.length)line([['','t'],['---------- live ----------','s']]);since=j.next}catch(e){}}
loadInfo(true).then(loadLog).then(poll);setInterval(()=>loadInfo(false),10000);
</script></body></html>)HTML";

// ======================= Web server =========================================
void hRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", portal ? PAGE_PORTAL : PAGE_MAIN);
}

void hAt() {                                                         // POST /api/at  cmd=...
  String cmd = server.arg("cmd"); cmd.replace("\r", " "); cmd.replace("\n", " ");
  String out;
  io.printf("[web] %s\r\n", cmd.c_str());
  io.cap = &out; at(cmd); io.cap = nullptr;
  server.send(200, "text/plain; charset=utf-8", out);
}

void hInfo() {                                                       // GET /api/info
  const Preset& p = PRESETS[cfg.preset];
  String j = "{\"node\":\"" + hexId(cfg.node) + "\",\"ln\":" + jStr(cfg.ln) + ",\"sn\":" + jStr(cfg.sn) +
             ",\"preset\":" + String(cfg.preset) + ",\"pname\":\"" + p.name + "\",\"f0\":" + String(cfg.f0, 3) +
             ",\"f1\":" + String(cfg.f1, 3) + ",\"fov\":" + String(cfg.fov, 4) + ",\"freq\":" + String(freq, 4) +
             ",\"slot\":" + String(slot + 1) + ",\"nch\":" + String(nCh) + ",\"hash\":" + String(chHash) +
             ",\"hop\":" + String(cfg.hop) + ",\"pwr\":" + String(cfg.pwr) + ",\"relay\":" + String(cfg.relay) +
             ",\"rly\":" + String(stRelayed) + ",\"rlc\":" + String(stCanceled) + ",\"slotms\":" + String(slotMs, 1) +
             ",\"ssid\":" + jStr(wSsid.c_str()) + ",\"ip\":\"" + WiFi.localIP().toString() +
             "\",\"wrssi\":" + String(wifiUp ? WiFi.RSSI() : 0) + ",\"up\":" + String(millis() / 1000) +
             ",\"batt\":" + String(battOk ? 1 : 0) + ",\"soc\":" + String(min(battSoc, 100.0f), 1) +
             ",\"vbat\":" + String(battV, 3) + ",\"crate\":" + String(battRate, 1) +
             ",\"usb\":" + String(usbPower() ? 1 : 0) + ",\"unread\":" + String(unread) + ",\"presets\":[";
  for (int i = 0; i < NPRE; i++) j += String(i ? "," : "") + "\"" + PRESETS[i].name + "\"";
  j += "],\"nodes\":[";
  bool first = true;
  for (auto& n : nodes) if (n.id) {
    j += String(first ? "" : ",") + "{\"id\":\"" + hexId(n.id) + "\",\"sn\":" + jStr(n.sn) + ",\"ln\":" + jStr(n.ln) +
         ",\"rssi\":" + String(n.rssi, 0) + ",\"snr\":" + String(n.snr, 1) + ",\"ago\":" + String((millis() - n.seen) / 1000) + "}";
    first = false;
  }
  server.send(200, "application/json", j + "]}");
}

void hEv() {                                                         // GET /api/ev?since=N
  uint32_t since = server.arg("since").toInt();
  if (server.arg("seen") == "1") unread = 0;                         // web page visible: mark as read
  uint32_t from = evSeq > NEV ? evSeq - NEV : 0;
  if (since > evSeq) since = 0;                                      // the device rebooted
  if (since < from) since = from;
  String j = "{\"next\":" + String(evSeq) + ",\"ev\":[";
  for (uint32_t s = since; s < evSeq; s++) j += (s > since ? "," : "") + evBuf[s % NEV];
  server.send(200, "application/json", j + "]}");
}

void hLog() {                                                        // GET /api/log?n=N
  int n = server.hasArg("n") ? server.arg("n").toInt() : 300;
  if (n < 1 || n > 500) n = 300;
  std::vector<String> v; logTail(n, v);
  String j; j.reserve(256 + v.size() * 160);
  j = "{\"next\":" + String(evSeq) + ",\"log\":[";
  for (size_t i = 0; i < v.size(); i++) { if (i) j += ","; j += v[i]; }
  server.send(200, "application/json", j + "]}");
}

void hLogDl() {                                                      // GET /log.jsonl (full download)  
  server.sendHeader("Content-Disposition", "attachment; filename=meshmodem_log.jsonl");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/x-ndjson", "");
  char buf[1024];
  for (const char* path : {LOG_OLD, LOG_FILE}) {
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    while (f.available()) { size_t n = f.read((uint8_t*)buf, sizeof(buf)); server.sendContent(buf, n); }
    f.close();
  }
  server.sendContent("");
}

void hScan() {                                                       // GET /scan (portal)
  int n = WiFi.scanNetworks();
  String j = "[";
  for (int i = 0; i < n; i++) j += String(i ? "," : "") + "{\"s\":" + jStr(WiFi.SSID(i).c_str()) + ",\"r\":" + String(WiFi.RSSI(i)) + "}";
  WiFi.scanDelete();
  server.send(200, "application/json", j + "]");
}

void hWifi() {                                                       // POST /wifi (portal)
  wSsid = server.arg("ssid"); wPass = server.arg("pass");
  String shown = wSsid; shown.replace("&", "&amp;"); shown.replace("<", "&lt;");
  nvs.putString("ssid", wSsid); nvs.putString("pass", wPass);
  io.printf("[web] WiFi saved: %s\r\n", wSsid.c_str());
  server.send(200, "text/html; charset=utf-8",
    "<meta name=viewport content='width=device-width'><body style='font-family:system-ui;padding:24px'>"
    "<h2>Saved</h2><p>The modem is connecting to <b>" + shown + "</b>. Go back to your WiFi network and open "
    "<a href='http://" HOSTNAME ".local/'>http://" HOSTNAME ".local/</a> or the IP shown by ATI on the serial terminal.</p>");
  tWifiPending = millis() | 1;
}

void hNotFound() {
  if (portal) { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); }
  else server.send(404, "text/plain", "not found");
}

// ======================= Setup / loop =======================================
void setup() {
  Serial.begin(115200);
  pinMode(PIN_DIO1, INPUT);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  nvs.begin("mmweb", false);
  Cfg c;
  if (nvs.getBytes("cfg", &c, sizeof(c)) == sizeof(c) && c.magic == DEF.magic) cfg = c;
  wSsid = nvs.getString("ssid", ""); wPass = nvs.getString("pass", "");
  fsOk = LittleFS.begin(true);                                       // formats only the first time
  battInit();
  pinMode(PIN_LED_RED, OUTPUT); pinMode(PIN_LED_GREEN, OUTPUT);
  if (PIN_PGOOD >= 0) pinMode(PIN_PGOOD, INPUT_PULLUP);              // /PGOOD is open-drain

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

  server.on("/", HTTP_GET, hRoot);
  server.on("/api/at", HTTP_POST, hAt);
  server.on("/api/info", HTTP_GET, hInfo);
  server.on("/api/ev", HTTP_GET, hEv);
  server.on("/scan", HTTP_GET, hScan);
  server.on("/api/log", HTTP_GET, hLog);
  server.on("/log.jsonl", HTTP_GET, hLogDl);
  server.on("/wifi", HTTP_POST, hWifi);
  server.onNotFound(hNotFound);

  io.printf("\r\n+READY:Mesh_Modem_Web v3.4,RADIO=%d,BOOT=%s  (AT+HELP = commands)\r\n", e, resetReason());
  wifiStart();
  info();
  logEv("\"t\":\"sys\",\"txt\":" + jStr((String("Modem started (") + resetReason() + ")").c_str()));
  if (!e) sendNodeInfo();
}

void loop() {
  static String line; static uint32_t tNI = millis();
  receive();
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r' || c == '\n') { if (line.length()) unread = 0; at(line); line = ""; }   // terminal in use: mark as read
    else if (line.length() < 240) line += c;
  }
  server.handleClient();
  wifiLoop();
  bootButton();
  ledsLoop();
  static uint32_t tBatt = millis();
  if (millis() - tBatt >= BATT_PERIOD_MS) { tBatt = millis(); battRead(); }
  serviceRelay();
  if (millis() - tNI >= NI_PERIOD_MS) { tNI = millis(); sendNodeInfo(); }
}
