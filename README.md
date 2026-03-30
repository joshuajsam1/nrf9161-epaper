# nRF9161 DK + Waveshare 7.5" e-Paper Test

A minimal [nRF Connect SDK](https://developer.nordicsemi.com/nRF_Connect_SDK/doc/latest/) (Zephyr RTOS) application that prints text to a Waveshare 7.5" 800×480 e-ink display connected to an nRF9161 Development Kit.

**Hardware:**
- [nRF9161 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF9161-DK)
- [Waveshare Universal e-Paper Driver HAT (SKU 13512)](https://www.waveshare.com/e-paper-driver-hat.htm)
- [Waveshare 7.5" e-Paper Display (GDEW075T7, 800×480)](https://www.waveshare.com/7.5inch-e-paper-hat.htm)

---

## Hardware Wiring

Connect the Waveshare HAT ribbon cable to the nRF9161 DK Arduino header using jumper wires:

| HAT Pin | Signal | nRF9161 DK Arduino Pin | nRF GPIO | Notes |
|---------|--------|------------------------|----------|-------|
| 1 | VCC | 3.3V | — | **3.3V only — do NOT use 5V** |
| 2 | GND | GND | — | |
| 3 | DIN | D11 (MOSI) | P0.11 | SPI3 data in |
| 4 | CLK | D13 (SCK) | P0.13 | SPI3 clock |
| 5 | CS | D10 | P0.10 | SPI3 chip select (active low) |
| 6 | DC | D9 | P0.09 | Data / Command select |
| 7 | RST | D8 | P0.08 | Reset (active low) |
| 8 | BUSY | D7 | P0.07 | Busy flag (driver polls this) |

> MISO (D12) is part of the SPI3 pinctrl but unused by e-paper — leave it unconnected.

The HAT's 40-pin Raspberry Pi header is not used; connect via the ribbon cable connector only.

---

## Prerequisites

1. **nRF Connect SDK** v2.7.0 or later installed.
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
├── CMakeLists.txt       # Build definition
├── prj.conf             # Zephyr Kconfig options
└── src/
    └── main.c           # Application — init display, print text
```

No custom device tree overlay is needed. The Zephyr built-in shield
`waveshare_epaper_gdew075t7` provides the full device tree configuration
(SPI node, GPIO pin assignments, UC8179 driver binding).

---

## Build

```bash
cd ~/gh/nrf9161-epaper

# Activate the venv first (required — system Python lacks pykwalify)
export PATH="$HOME/ncs/.venv/bin:$PATH"
export ZEPHYR_BASE="$HOME/ncs/zephyr"

west build \
    -b nrf9161dk/nrf9161 \
    -p always \
    -- -DSHIELD=waveshare_epaper_gdew075t7 \
       -DPython3_EXECUTABLE="$HOME/ncs/.venv/bin/python3.12"
```

| Flag | Purpose |
|------|---------|
| `-b nrf9161dk/nrf9161` | Target board (nRF9161 DK, nRF9161 core) |
| `-p always` | Pristine/clean build (recommended when changing shields) |
| `-DSHIELD=waveshare_epaper_gdew075t7` | Apply the upstream Waveshare 7.5" shield overlay |

---

## Flash

```bash
west flash
```

Or specify the J-Link runner explicitly:

```bash
west flash --runner jlink
```

---

## Serial Monitor

Open the USB serial port at **115200 baud** to see log output:

```bash
# macOS
screen /dev/tty.usbmodem* 115200

# Linux
screen /dev/ttyACM0 115200

# Or use minicom
minicom -D /dev/ttyACM0 -b 115200
```

### Expected output

```
[00:00:00.200] <inf> epaper_hello: Display device ready: uc8179@0
[00:00:00.201] <inf> epaper_hello: CFB framebuffer initialised  (800 x 480, 1bpp)
[00:00:00.202] <inf> epaper_hello: Available CFB fonts: 1
[00:00:00.203] <inf> epaper_hello: Using font 0: 10 x 16 px
[00:00:00.204] <inf> epaper_hello: Sending framebuffer to display (full refresh ~4s) ...
[00:00:04.300] <inf> epaper_hello: Display refresh complete
```

The display will show three lines of text after a ~4 second full refresh cycle and then retain the image indefinitely with no power.

---

## How It Works

1. **Device acquisition** — `DEVICE_DT_GET(DT_CHOSEN(zephyr_display))` resolves the display node set in the shield overlay's `chosen` block.
2. **CFB init** — `cfb_framebuffer_init()` allocates a 48 000-byte RAM pixel buffer (800×480 ÷ 8 bits).
3. **Draw** — `cfb_draw_text()` renders text into the RAM buffer at pixel coordinates.
4. **Flush** — `cfb_framebuffer_finalize()` sends the buffer over SPI to the UC8179 controller and triggers the e-paper waveform refresh. The BUSY GPIO is polled inside the driver.

---

## Adapting for a Different Panel

Change only the `-DSHIELD` flag. No code changes are needed:

| Panel | Controller | Shield Flag |
|-------|-----------|-------------|
| 7.5" GDEW075T7 (800×480) | UC8179 | `waveshare_epaper_gdew075t7` ← **this project** |
| 2.13" GDEH0213B1 (250×122) | SSD1673 | `waveshare_epaper_gdeh0213b1` |
| 2.13" GDEY0213B74 (250×122) | SSD1680 | `waveshare_epaper_gdey0213b74` |
| 2.9" GDEH029A1 (296×128) | SSD1608 | `waveshare_epaper_gdeh029a1` |
| 1.54" GDEH0154A07 (200×200) | SSD1681 | `waveshare_epaper_gdeh0154a07` |
| 4.2" GDEW042T2 (400×300) | UC8176 | `waveshare_epaper_gdew042t2` |

For smaller panels also reduce `CONFIG_HEAP_MEM_POOL_SIZE` in `prj.conf`
(minimum = `width × height / 8` bytes, e.g. 4096 for 2.13").

---

## Troubleshooting

| Symptom | Likely cause | Fix |
|---------|-------------|-----|
| `Display device not ready` | Shield not applied or wrong shield name | Verify `-DSHIELD=waveshare_epaper_gdew075t7` in build command |
| `cfb_framebuffer_init failed: -12` (-ENOMEM) | Heap too small | Increase `CONFIG_HEAP_MEM_POOL_SIZE` in `prj.conf` |
| Display stays blank after refresh | BUSY pin not wired to D7 | Driver polls BUSY during init; floating BUSY causes timeout |
| Garbled / shifted output | DC and RST wires swapped | Verify D9=DC, D8=RST per wiring table |
| Build error: SPI node not found | Outdated nRF Connect SDK | Use nRF Connect SDK v2.7.0 or later |
