# ============================================================================
#  ELECTGPL - stress_tx.py : statistical test for resets during LoRa TX
#  For Mesh_Modem_Web / MeshAT. Runs from Thonny or any Python 3 (needs pyserial).
#  Moves the modem off the mesh slot (AT+FREQ) so it does not disturb the network,
#  transmits N times and counts resets by watching for the +READY banner.
# ============================================================================
import serial, time

# ------------------------------- SETTINGS -----------------------------------
PORT        = "COM5"        # modem serial port (e.g. "/dev/ttyUSB0" on Linux)
BAUD        = 115200
REPETITIONS = 100           # number of transmissions
INTERVAL_S  = 5.0           # pause between transmissions (low duty cycle)
POWER_DBM   = 20            # TX power (AT+PWR)
TEST_FREQ   = 920.0         # MHz, outside the mesh slot; irrelevant with a dummy load
TEXT        = "x" * 100     # test payload (100 bytes, ~1.1 s in LongFast, ~0.3 s in MediumFast)
# ----------------------------------------------------------------------------

def send(s, cmd, wait=1.5):
    s.write((cmd + "\r\n").encode())
    t, lines = time.time(), []
    while time.time() - t < wait:
        l = s.readline().decode(errors="replace").strip()
        if l:
            lines.append(l)
            if l in ("OK", "ERROR"):
                break
    return lines

def configure(s):
    send(s, "AT+FREQ=%.4f" % TEST_FREQ)
    send(s, "AT+PWR=%d" % POWER_DBM)

s = serial.Serial()
s.port, s.baudrate, s.timeout = PORT, BAUD, 0.2
s.dtr = False               # avoid resetting the ESP32 when the port opens
s.rts = False
s.open()
time.sleep(0.5)
s.reset_input_buffer()

configure(s)
resets, failures = [], 0
print("Test: %d TX at %d dBm on %.3f MHz" % (REPETITIONS, POWER_DBM, TEST_FREQ))

for i in range(1, REPETITIONS + 1):
    r = send(s, "AT+SEND=" + TEXT, wait=8)
    if "OK" not in r:
        failures += 1
    # listen during the interval to catch a +READY (reset)
    t = time.time()
    while time.time() - t < INTERVAL_S:
        l = s.readline().decode(errors="replace").strip()
        if l.startswith("+READY"):
            resets.append((i, l))
            print("  RESET at TX %d: %s" % (i, l))
            time.sleep(4)               # wait for boot and WiFi
            s.reset_input_buffer()
            configure(s)                # after a reset it is back to the saved config
    if i % 10 == 0:
        print("  %d/%d TX, %d resets" % (i, REPETITIONS, len(resets)))

send(s, "AT+FREQ=0")                    # back to the automatic mesh slot
print("\nResult: %d resets and %d TX without OK in %d transmissions at %d dBm"
      % (len(resets), failures, REPETITIONS, POWER_DBM))
s.close()
