/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare 7.5" e-Paper (GDEW075T7 / UC8179, 800x480) hello-world
 *
 * Target board : nRF9161 DK  (nrf9161dk/nrf9161)
 * Shield       : waveshare_epaper_gdew075t7
 *
 * Build:
 *   export PATH="$HOME/ncs/.venv/bin:$PATH"
 *   export ZEPHYR_BASE="$HOME/ncs/zephyr"
 *   west build -b nrf9161dk/nrf9161 -p always \
 *              -- -DSHIELD=waveshare_epaper_gdew075t7 \
 *                 -DPython3_EXECUTABLE="$HOME/ncs/.venv/bin/python3.12"
 *
 * The UC8179 driver uses PIXEL_FORMAT_MONO10 with SCREEN_INFO_MONO_MSB_FIRST
 * (horizontal row-major layout). Zephyr's CFB API only supports vertically-
 * tiled displays (OLEDs), so we use display_write() directly and access
 * Zephyr's built-in DroidSansMono 10x16 font data externally.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <string.h>

LOG_MODULE_REGISTER(epaper_hello, LOG_LEVEL_INF);

/* ---------- display geometry ---------- */
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480
#define BYTES_PER_ROW   (DISPLAY_WIDTH / 8)
#define FRAME_SIZE      (BYTES_PER_ROW * DISPLAY_HEIGHT)   /* 48 000 bytes */

/* ---------- framebuffer ----------
 * MONO10: 1 = white, 0 = black, bit 7 = leftmost pixel (MSB-first)
 */
static uint8_t framebuf[FRAME_SIZE];

/* ---------- font ----------
 * Zephyr's built-in DroidSansMono 10x16, compiled in by
 * CONFIG_CHARACTER_FRAMEBUFFER + CONFIG_CHARACTER_FRAMEBUFFER_USE_DEFAULT_FONTS.
 *
 * Layout: cfb_font_1016[char_index][col * 2 + row / 8]
 *   - char_index = ASCII code - 0x20  (0 = space, 1 = '!', ...)
 *   - col = 0..9 (left to right)
 *   - row = 0..15 (top to bottom)
 *   - within each byte: bit (row % 8) is set when the pixel is foreground
 */
#define FONT_W   10
#define FONT_H   16
#define FONT_GAP  1     /* extra horizontal pixels between characters */

extern const uint8_t cfb_font_1016[95][20];

/* ---------- pixel helpers ---------- */

static void fb_fill_white(void)
{
	memset(framebuf, 0xFF, FRAME_SIZE);  /* 0xFF = all white in MONO10 */
}

static inline void fb_set_black(int x, int y)
{
	if ((unsigned)x >= DISPLAY_WIDTH || (unsigned)y >= DISPLAY_HEIGHT) {
		return;
	}
	/* clear bit → black pixel  (MSB-first: bit 7 = leftmost) */
	framebuf[y * BYTES_PER_ROW + x / 8] &= ~(1u << (7 - x % 8));
}

/* ---------- font rendering ---------- */

static void fb_draw_char(char c, int x, int y)
{
	int idx = (unsigned char)c - 0x20;

	if (idx < 0 || idx >= 95) {
		return;   /* unprintable — skip */
	}
	for (int col = 0; col < FONT_W; col++) {
		for (int row = 0; row < FONT_H; row++) {
			uint8_t byte = cfb_font_1016[idx][col * 2 + row / 8];
			if ((byte >> (row % 8)) & 1u) {
				fb_set_black(x + col, y + row);
			}
		}
	}
}

static void fb_draw_string(const char *str, int x, int y)
{
	int cx = x;

	while (*str) {
		if (*str == '\n') {
			cx = x;
			y += FONT_H + 4;
		} else {
			fb_draw_char(*str, cx, y);
			cx += FONT_W + FONT_GAP;
		}
		str++;
	}
}

/* ---------- thick line for decorative border ---------- */
static void fb_hline(int x0, int x1, int y, int thickness)
{
	for (int t = 0; t < thickness; t++) {
		for (int x = x0; x <= x1; x++) {
			fb_set_black(x, y + t);
		}
	}
}

static void fb_vline(int x, int y0, int y1, int thickness)
{
	for (int t = 0; t < thickness; t++) {
		for (int y = y0; y <= y1; y++) {
			fb_set_black(x + t, y);
		}
	}
}

/* ---------- main ---------- */

int main(void)
{
	const struct device *display_dev;
	int rc;

	/* 1. Get display device */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display not ready");
		return -ENODEV;
	}
	LOG_INF("Display ready: %s", display_dev->name);

	/* 2. White background */
	fb_fill_white();

	/* 3. Border (4 px thick) */
	fb_hline(0, DISPLAY_WIDTH - 1, 0,                   4);
	fb_hline(0, DISPLAY_WIDTH - 1, DISPLAY_HEIGHT - 4,  4);
	fb_vline(0,                  0, DISPLAY_HEIGHT - 1, 4);
	fb_vline(DISPLAY_WIDTH - 4,  0, DISPLAY_HEIGHT - 1, 4);

	/* 4. Text */
	fb_draw_string("Hello, World!",            20, 20);
	fb_draw_string("nRF9161 DK",               20, 60);
	fb_draw_string("Waveshare 7.5\" 800x480",  20, 100);
	fb_draw_string("e-Paper test OK",          20, 140);

	/* 5. Send full frame to display (MONO10, 800x480) */
	struct display_buffer_descriptor buf_desc = {
		.buf_size = FRAME_SIZE,
		.width    = DISPLAY_WIDTH,
		.height   = DISPLAY_HEIGHT,
		.pitch    = DISPLAY_WIDTH,
	};

	/* Trigger the first refresh (clears to white) then enable writes to DTM2 */
	LOG_INF("Blanking off (first refresh, clears display)...");
	display_blanking_off(display_dev);

	/* Belt-and-suspenders: sleep long enough for a full UC8179 refresh
	 * (~4 s) in case the BUSY GPIO polarity is still off. */
	k_sleep(K_SECONDS(5));
	LOG_INF("Sending frame (%u bytes)...", FRAME_SIZE);

	rc = display_write(display_dev, 0, 0, &buf_desc, framebuf);
	if (rc != 0) {
		LOG_ERR("display_write failed: %d", rc);
		return rc;
	}

	/* Wait for the second full refresh to physically complete */
	k_sleep(K_SECONDS(5));
	LOG_INF("Display refresh complete");

	/* E-paper holds image without power — sleep forever */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
