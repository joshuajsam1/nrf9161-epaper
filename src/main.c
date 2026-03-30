/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Waveshare 7.5" e-Paper (GDEW075T7 / UC8179, 800x480) hello-world
 *
 * Target board : nRF9161 DK  (nrf9161dk/nrf9161)
 * Shield       : waveshare_epaper_gdew075t7
 *
 * Build:
 *   west build -b nrf9161dk/nrf9161 -p always \
 *              -- -DSHIELD=waveshare_epaper_gdew075t7
 *
 * Flash:
 *   west flash
 *
 * Uses the Zephyr Character Framebuffer (CFB) API for text rendering.
 * No LVGL, no external graphics library.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(epaper_hello, LOG_LEVEL_INF);

int main(void)
{
	const struct device *display_dev;
	uint8_t font_width, font_height;
	int rc;

	/* -----------------------------------------------------------------
	 * 1. Obtain the display device.
	 *    DT_CHOSEN(zephyr_display) resolves to the node set by:
	 *      chosen { zephyr,display = &gdew075t7; }
	 *    which the shield overlay writes automatically.
	 * ----------------------------------------------------------------- */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display device '%s' not ready", display_dev->name);
		return -ENODEV;
	}
	LOG_INF("Display device ready: %s", display_dev->name);

	/* -----------------------------------------------------------------
	 * 2. Disable blanking (activate the display).
	 *    E-paper drivers return -ENOTSUP here — that is expected and safe.
	 * ----------------------------------------------------------------- */
	rc = display_blanking_off(display_dev);
	if (rc != 0 && rc != -ENOTSUP) {
		LOG_ERR("display_blanking_off failed: %d", rc);
		return rc;
	}

	/* -----------------------------------------------------------------
	 * 3. Initialise the Character Framebuffer.
	 *    Allocates a RAM pixel buffer (k_malloc) sized to the full frame:
	 *    800 * 480 / 8 = 48 000 bytes for 1bpp monochrome.
	 *    CONFIG_HEAP_MEM_POOL_SIZE must be at least 56000.
	 * ----------------------------------------------------------------- */
	rc = cfb_framebuffer_init(display_dev);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_init failed: %d  "
			"(try increasing CONFIG_HEAP_MEM_POOL_SIZE)", rc);
		return rc;
	}
	LOG_INF("CFB framebuffer initialised  (800 x 480, 1bpp)");

	/* -----------------------------------------------------------------
	 * 4. Clear the RAM framebuffer (does NOT send data to the panel yet).
	 * ----------------------------------------------------------------- */
	rc = cfb_framebuffer_clear(display_dev, false);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_clear failed: %d", rc);
		return rc;
	}

	/* -----------------------------------------------------------------
	 * 5. Select a built-in font.
	 *    Requires CONFIG_CHARACTER_FRAMEBUFFER_USE_DEFAULT_FONTS=y.
	 *    Font index 0 is the smallest default font (~10x16 px).
	 * ----------------------------------------------------------------- */
	int num_fonts = cfb_get_numof_fonts(display_dev);
	LOG_INF("Available CFB fonts: %d", num_fonts);

	if (num_fonts > 0) {
		cfb_framebuffer_set_font(display_dev, 0);
		cfb_get_font_size(display_dev, 0, &font_width, &font_height);
		LOG_INF("Using font 0: %u x %u px", font_width, font_height);
	}

	/* -----------------------------------------------------------------
	 * 6. Draw text into the RAM framebuffer.
	 *    cfb_draw_text(dev, string, x_pixels, y_pixels)
	 *    Text is white-on-black by default on e-paper.
	 * ----------------------------------------------------------------- */
	rc = cfb_draw_text(display_dev, "Hello, World!", 10, 10);
	if (rc != 0) {
		LOG_ERR("cfb_draw_text failed: %d", rc);
	}

	rc = cfb_draw_text(display_dev, "nRF9161 DK", 10, 40);
	if (rc != 0) {
		LOG_WRN("cfb_draw_text line 2 failed: %d", rc);
	}

	rc = cfb_draw_text(display_dev, "Waveshare 7.5\" 800x480", 10, 70);
	if (rc != 0) {
		LOG_WRN("cfb_draw_text line 3 failed: %d", rc);
	}

	/* -----------------------------------------------------------------
	 * 7. Flush the RAM framebuffer to the panel and trigger refresh.
	 *    On 7.5" e-paper this full refresh takes ~3-4 seconds.
	 *    The BUSY GPIO is polled inside the UC8179 driver — you do not
	 *    need to poll it yourself.
	 * ----------------------------------------------------------------- */
	LOG_INF("Sending framebuffer to display (full refresh ~4s) ...");
	rc = cfb_framebuffer_finalize(display_dev);
	if (rc != 0) {
		LOG_ERR("cfb_framebuffer_finalize failed: %d", rc);
		return rc;
	}
	LOG_INF("Display refresh complete");

	/* E-paper retains the image with no power — sleep forever. */
	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
