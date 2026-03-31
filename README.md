# nRF9161 DK — e-Paper SMS Display

An [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/) (Zephyr RTOS) application that:

1. Displays a high-contrast test pattern on a Waveshare 7.5" 800×480 e-ink display.
2. Connects to the LTE network via the nRF9161 modem.
3. Listens for incoming SMS messages and renders each message on the e-ink display.

**Hardware:**
- [nRF9161 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9161-DK) with nano SIM card inserted
- [Waveshare Universal e-Paper Driver HAT (SKU 13512)](https://www.waveshare.com/e-paper-driver-hat.htm)
- [Waveshare 7.5" e-Paper Display (GDEW075T7, 800×480)](https://www.waveshare.com/7.5inch-e-paper-hat.htm)

---

## Hardware Wiring

Connect the Waveshare HAT ribbon cable to the nRF9161 DK Arduino header:

| HAT Pin | Signal | nRF9161 DK Arduino Pin | nRF GPIO | Notes |
|---------|--------|------------------------|----------|-------|
| 1 | VCC | 3.3V | — | **3.3V only — do NOT use 5V** |
| 2 | GND | GND | — | |
| 3 | DIN | D11 (MOSI) | P0.11 | SPI3 data in |
| 4 | CLK | D13 (SCK) | P0.13 | SPI3 clock |
| 5 | CS | D10 | P0.10 | SPI3 chip select (active low) |
| 6 | DC | D9 | P0.09 | Data / Command select |
| 7 | RST | D8 | P0.08 | Reset (active low) |
| 8 | BUSY | D7 | P0.07 | Busy flag (HIGH = busy) |

> Use **3.3V only**. The nRF9161 DK exposes 3.3V on the Arduino header VCC pin. Do not connect to 5V.

> MISO (D12) is part of the SPI3 pinctrl but unused by e-paper — leave it unconnected.

### SIM Card

Insert a nano SIM card into the J3 slot on the nRF9161 DK. The SIM must support SMS. The phone number printed on the SIM (or shown by your carrier) is the number to SMS to trigger the display.

---

## Prerequisites

1. **nRF Connect SDK** v2.7.0 or later (tested with v2.9.0).
   Follow the [official getting started guide](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/nrf/getting_started.html) or use the **nRF Connect for VS Code** extension.

2. **west** workspace initialised and dependencies fetched:
   ```bash
   west update
   ```

3. **J-Link** or **nRF9161 DK USB** connected for flashing.

---

## Project Structure

```
nrf9161-epaper/
├── CMakeLists.txt                    # Build definition
├── prj.conf                          # Zephyr Kconfig options
├── boards/
│   ├── nrf9161dk_nrf9161.overlay     # BUSY GPIO polarity fix (non-NS)
│   └── nrf9161dk_nrf9161_ns.overlay  # BUSY GPIO polarity fix (NS/TF-M)
└── src/
    └── main.c                        # SMS display application
```

The Zephyr built-in shield `waveshare_epaper_gdew075t7` provides the display device tree configuration. The board overlay overrides the `busy-gpios` polarity to `GPIO_ACTIVE_HIGH` to match the UC8179 controller's actual behaviour (HIGH = busy, confirmed by Waveshare demo code).

---

## Build

```bash
cd ~/gh/nrf9161-epaper

# Activate the ncs venv (required — system Python lacks pykwalify)
export PATH="$HOME/ncs/.venv/bin:$PATH"
export ZEPHYR_BASE="$HOME/ncs/zephyr"

west build \
    -b nrf9161dk/nrf9161/ns \
    -p always \
    -- -DSHIELD=waveshare_epaper_gdew075t7 \
       -DPython3_EXECUTABLE="$HOME/ncs/.venv/bin/python3.12"
```

| Flag | Purpose |
|------|---------|
| `-b nrf9161dk/nrf9161/ns` | Non-secure (TF-M) target — required for modem/SMS access |
| `-p always` | Pristine/clean build |
| `-DSHIELD=waveshare_epaper_gdew075t7` | Apply the upstream Waveshare 7.5" shield overlay |

---

## Flash

```bash
west flash --runner jlink
```

Or via nrfjprog directly:

```bash
nrfjprog --program build/merged.hex --sectorerase --verify --reset
```

---

## Serial Monitor

Open the USB serial port at **115200 baud**:

```bash
# macOS — use cu. device (tty. blocks on macOS)
screen /dev/cu.usbmodem* 115200
```

### Expected boot sequence

```
[00:00:00.xxx] <inf> sms_display: === nRF9161 SMS Display starting ===
[00:00:00.xxx] <inf> sms_display: Display ready: uc8179@0
[00:00:08.xxx] <inf> sms_display: Display initialised
[00:00:16.xxx] <inf> sms_display: Modem library initialised
[00:00:16.xxx] <inf> sms_display: Connecting to LTE...
... (30-120 seconds for LTE registration) ...
[00:01:xx.xxx] <inf> sms_display: Connected to LTE
[00:01:xx.xxx] <inf> sms_display: SMS listener registered (handle 0)
[00:01:xx.xxx] <inf> sms_display: === Waiting for SMS messages ===
```

After the "Waiting for SMS messages" log the display shows:

```
Ready! Send an SMS to this
device to display it here.
```

### Sending a test SMS

Send an SMS from your phone to the SIM card's number. After a few seconds the display will refresh (~5-8 s for e-ink waveform) and show:

```
SMS RECEIVED (white text on black header)
From: +1234567890
Your message text here, automatically
word-wrapped across multiple lines.
─────────────────────────
nRF9161 DK | UC8179 | 800x480
```

---

## How It Works

1. **Display init** — `DEVICE_DT_GET(DT_CHOSEN(zephyr_display))` acquires the UC8179 display. `display_blanking_off()` triggers the initial clear refresh (all-white). The board overlay fixes the BUSY GPIO polarity (`GPIO_ACTIVE_HIGH`) so the driver correctly waits during the ~5 s refresh waveform.

2. **Modem init** — `nrf_modem_lib_init()` initialises the nRF9161 baseband modem over the IPC shared-memory interface.

3. **LTE connect** — `lte_lc_register_handler()` + `lte_lc_connect()` (blocking) register an event handler and wait for LTE registration. This typically takes 30–120 seconds depending on signal strength.

4. **SMS listener** — `sms_register_listener()` registers a callback that fires whenever an SMS is received. The callback copies the sender address and text into a message queue.

5. **Display thread** — A dedicated thread (`disp_worker`) dequeues SMS messages and renders them: inverted header bar with sender number, word-wrapped body text, and a footer rule.

---

## Adapting for a Different Panel

Change the `-DSHIELD` flag and update `CONFIG_HEAP_MEM_POOL_SIZE` in `prj.conf` (minimum = `width × height / 8` bytes):

| Panel | Controller | Shield Flag | Heap (bytes) |
|-------|-----------|-------------|--------------|
| 7.5" GDEW075T7 (800×480) | UC8179 | `waveshare_epaper_gdew075t7` ← **this project** | 65000 |
| 4.2" GDEW042T2 (400×300) | UC8176 | `waveshare_epaper_gdew042t2` | 20000 |
| 2.9" GDEH029A1 (296×128) | SSD1608 | `waveshare_epaper_gdeh029a1` | 8000 |
| 2.13" GDEY0213B74 (250×122) | SSD1680 | `waveshare_epaper_gdey0213b74` | 6000 |

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `Display not ready` | Shield not applied or wrong name | Verify `-DSHIELD=waveshare_epaper_gdew075t7` |
| Display stays blank after ~30 s | BUSY GPIO polarity wrong | Confirm `boards/*.overlay` sets `GPIO_ACTIVE_HIGH` |
| Display blanks in ~200 ms (too fast) | BUSY polarity inverted — driver not waiting | Set `GPIO_ACTIVE_HIGH` in overlay |
| LTE connect times out | No SIM, no signal, or wrong APN | Check SIM is inserted and has LTE coverage |
| SMS listener returns -EBUSY | Modem not connected yet | Wait for LTE connection before registering SMS |
| Build error: `nrf_modem.h` not found | Building for non-NS target | Use `-b nrf9161dk/nrf9161/ns` (not `/nrf9161`) |
| Build error: `cfb_font_1016` undefined | CHARACTER_FRAMEBUFFER not enabled | Add `CONFIG_CHARACTER_FRAMEBUFFER=y` in `prj.conf` |
| J-Link "Cannot connect" after flash | J-Link USB out of sync | Unplug/replug USB cable to the nRF9161 DK |
