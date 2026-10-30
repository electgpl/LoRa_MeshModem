# tools

## stress_tx.py

Statistical test for resets during LoRa transmission (for example to detect power-supply brownouts when TX and WiFi peaks coincide).

1. `pip install pyserial`
2. Edit the settings block at the top (`PORT`, `REPETITIONS`, `POWER_DBM`, `TEST_FREQ`…).
3. Run it from Thonny or `python stress_tx.py`.

It moves the modem to `TEST_FREQ` (off the mesh slot, so it does not disturb the network), transmits `REPETITIONS` messages and counts resets by watching for the `+READY` banner. At the end it returns to the automatic slot. The test messages are stored in the history; clear them with `AT+LOGCLR` if needed.
