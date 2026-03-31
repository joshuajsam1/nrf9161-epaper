/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF9161 DK — e-Paper SMS Display
 *
 * Connects to LTE, listens for incoming SMS messages, and renders each
 * message on the Waveshare 7.5" 800×480 e-ink display (UC8179/GDEW075T7).
 *
 * Wire the HAT per the README. Insert a nano SIM that supports SMS.
 * Send an SMS to the SIM's number → the message appears on the display.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <modem/sms.h>
#include <modem/nrf_modem_lib.h>
#include <modem/lte_lc.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(sms_display, LOG_LEVEL_INF);

/* ---- display constants -------------------------------------------------- */
#define DISPLAY_WIDTH   800
#define DISPLAY_HEIGHT  480
#define BYTES_PER_ROW   (DISPLAY_WIDTH / 8)
#define FRAME_SIZE      (BYTES_PER_ROW * DISPLAY_HEIGHT)

/* ---- font (column-major, 2 bytes/column, LSB = top pixel row) ----------- */
#define FONT_W  10
#define FONT_H  16
#define LINE_H  (FONT_H + 4)          /* vertical step between text lines   */
#define CHARS_PER_LINE ((DISPLAY_WIDTH - 40) / (FONT_W + 1)) /* ~68 chars */

extern const uint8_t cfb_font_1016[95][20];

/* ---- display state ------------------------------------------------------- */
static const struct device *display_dev;
static uint8_t framebuf[FRAME_SIZE];
static K_MUTEX_DEFINE(disp_mutex);
static bool display_initialised;

/* ---- pixel helpers ------------------------------------------------------- */
static inline void fb_set_black(int x, int y)
{
	if ((unsigned)x >= DISPLAY_WIDTH || (unsigned)y >= DISPLAY_HEIGHT) return;
	framebuf[y * BYTES_PER_ROW + x / 8] &= ~(1u << (7 - x % 8));
}

/* Draw character c at pixel (x, y). Returns x after last pixel column. */
static int draw_char(char c, int x, int y)
{
	int idx = (unsigned char)c - 0x20;

	if (idx < 0 || idx >= 95) return x;
	for (int col = 0; col < FONT_W; col++) {
		for (int row = 0; row < FONT_H; row++) {
			uint8_t b = cfb_font_1016[idx][col * 2 + row / 8];

			if ((b >> (row % 8)) & 1u) fb_set_black(x + col, y + row);
		}
	}
	return x + FONT_W + 1;
}

/*
 * Draw a NUL-terminated string with word-wrap.
 * Starts at (x0, y), wraps at max_x, stops at max_y.
 * Returns the y coordinate after the last line written.
 */
static int draw_str_wrapped(const char *s, int x0, int y, int max_x, int max_y)
{
	int cx = x0;

	while (*s && y + FONT_H <= max_y) {
		if (*s == '\n') {
			cx = x0;
			y += LINE_H;
			s++;
			continue;
		}
		/* Wrap at character boundary when next char won't fit */
		if (cx + FONT_W + 1 > max_x) {
			cx = x0;
			y += LINE_H;
		}
		if (y + FONT_H > max_y) break;
		cx = draw_char(*s++, cx, y);
	}
	return y;
}

/* Horizontal rule at y, thickness t */
static void fb_hrule(int y, int t)
{
	for (int row = 0; row < t && y + row < DISPLAY_HEIGHT; row++) {
		for (int x = 0; x < DISPLAY_WIDTH; x++) fb_set_black(x, y + row);
	}
}

/* ---- display update ------------------------------------------------------ */
static void display_commit(void)
{
	struct display_buffer_descriptor desc = {
		.buf_size = FRAME_SIZE,
		.width    = DISPLAY_WIDTH,
		.height   = DISPLAY_HEIGHT,
		.pitch    = DISPLAY_WIDTH,
	};

	int rc = display_write(display_dev, 0, 0, &desc, framebuf);

	if (rc) {
		LOG_ERR("display_write failed: %d", rc);
	}
	/*
	 * Hard wait to cover refresh time regardless of BUSY polarity.
	 * The driver's busy_wait (ACTIVE_HIGH overlay) handles real sync;
	 * this sleep provides belt-and-braces safety.
	 */
	k_sleep(K_SECONDS(8));
}

/*
 * Show a two-line status message centred on the display.
 * line2 may be NULL.
 */
static void display_show_status(const char *line1, const char *line2)
{
	k_mutex_lock(&disp_mutex, K_FOREVER);

	memset(framebuf, 0xFF, FRAME_SIZE);   /* all white */

	/* Light border */
	fb_hrule(0, 3);
	fb_hrule(DISPLAY_HEIGHT - 3, 3);
	for (int y = 0; y < DISPLAY_HEIGHT; y++) {
		fb_set_black(0, y); fb_set_black(1, y); fb_set_black(2, y);
		fb_set_black(DISPLAY_WIDTH - 3, y);
		fb_set_black(DISPLAY_WIDTH - 2, y);
		fb_set_black(DISPLAY_WIDTH - 1, y);
	}

	/* Header */
	draw_str_wrapped("nRF9161 SMS Display", 20, 12, DISPLAY_WIDTH - 20, 60);
	fb_hrule(34, 2);

	/* Status lines */
	int y = DISPLAY_HEIGHT / 2 - LINE_H;

	if (line1) {
		draw_str_wrapped(line1, 20, y, DISPLAY_WIDTH - 20,
				 y + LINE_H * 3);
		y += LINE_H + 4;
	}
	if (line2) {
		draw_str_wrapped(line2, 20, y, DISPLAY_WIDTH - 20,
				 y + LINE_H * 3);
	}

	display_commit();
	k_mutex_unlock(&disp_mutex);
}

/*
 * Render an SMS on the full display: sender number + wrapped body text.
 *
 * Layout (all black text on white background):
 *   y=10:  "=== SMS RECEIVED ==="
 *   y=36:  ─── separator ──────────
 *   y=44:  "From: +1234567890"
 *   y=68:  ─── separator ──────────
 *   y=76:  message body (word-wrapped)
 *   y=448: ─── separator ──────────
 *   y=456: footer
 */
static void display_show_sms(const char *from, const char *text)
{
	k_mutex_lock(&disp_mutex, K_FOREVER);

	memset(framebuf, 0xFF, FRAME_SIZE);   /* all white */

	/* Header */
	draw_str_wrapped("=== SMS RECEIVED ===",
			 20, 10, DISPLAY_WIDTH - 20, 36);
	fb_hrule(36, 3);

	/* From line */
	char from_buf[80];

	snprintf(from_buf, sizeof(from_buf), "From: %s", from);
	draw_str_wrapped(from_buf, 20, 46, DISPLAY_WIDTH - 20, 46 + LINE_H + 4);
	fb_hrule(46 + LINE_H + 4, 2);

	/* Message body */
	draw_str_wrapped(text, 20, 46 + LINE_H + 10,
			 DISPLAY_WIDTH - 20, DISPLAY_HEIGHT - LINE_H - 16);

	/* Footer */
	fb_hrule(DISPLAY_HEIGHT - LINE_H - 12, 2);
	draw_str_wrapped("nRF9161 DK  |  UC8179  |  800x480",
			 20, DISPLAY_HEIGHT - LINE_H - 6,
			 DISPLAY_WIDTH - 20, DISPLAY_HEIGHT);

	display_commit();
	k_mutex_unlock(&disp_mutex);
}

/* ---- display work thread ------------------------------------------------- */
/*
 * We offload display_show_sms() to a dedicated thread so the SMS callback
 * returns quickly, avoiding any library timeout issues.
 */
struct sms_msg {
	char from[SMS_MAX_ADDRESS_LEN_CHARS + 1];
	char text[SMS_MAX_PAYLOAD_LEN_CHARS + 1];
};

K_MSGQ_DEFINE(sms_msgq, sizeof(struct sms_msg), 4, 4);

#define DISPLAY_THREAD_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(display_thread_stack, DISPLAY_THREAD_STACK_SIZE);
static struct k_thread display_thread_data;

static void display_thread(void *a, void *b, void *c)
{
	struct sms_msg msg;

	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	while (1) {
		k_msgq_get(&sms_msgq, &msg, K_FOREVER);
		LOG_INF("Rendering SMS from %s", msg.from);
		display_show_sms(msg.from, msg.text);
		LOG_INF("Display updated");
	}
}

/* ---- SMS callback -------------------------------------------------------- */
static void sms_callback(struct sms_data *const data, void *context)
{
	ARG_UNUSED(context);

	if (!data) return;

	if (data->type == SMS_TYPE_DELIVER) {
		struct sms_msg msg = {0};
		const char *addr =
			data->header.deliver.originating_address.address_str;

		strncpy(msg.from, addr, sizeof(msg.from) - 1);
		strncpy(msg.text, data->payload, sizeof(msg.text) - 1);

		LOG_INF("SMS from %s (%d chars): %.*s",
			msg.from, data->payload_len,
			MIN(data->payload_len, 40), msg.text);

		/* Non-blocking: drop if queue is full (shouldn't happen) */
		if (k_msgq_put(&sms_msgq, &msg, K_NO_WAIT)) {
			LOG_WRN("SMS queue full, message dropped");
		}
	} else if (data->type == SMS_TYPE_STATUS_REPORT) {
		LOG_INF("SMS status report received");
	}
}

/* ---- LTE event handler --------------------------------------------------- */
static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("LTE reg status: %d", evt->nw_reg_status);
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("LTE cell update: TAC %d, ID %d",
			evt->cell.tac, evt->cell.id);
		break;
	case LTE_LC_EVT_LTE_MODE_UPDATE:
		LOG_INF("LTE mode: %d", evt->lte_mode);
		break;
	default:
		break;
	}
}

/* ======================================================================== */
int main(void)
{
	int rc;

	LOG_INF("=== nRF9161 SMS Display starting ===");

	/* ---- Step 1: Init display ---------------------------------------- */
	display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Display not ready — check SPI wiring");
		return -ENODEV;
	}
	LOG_INF("Display ready: %s", display_dev->name);

	/* Trigger initial clear refresh (blanking_off → sends PON+DRF).
	 * The driver's busy_wait (ACTIVE_HIGH overlay) blocks until done. */
	display_blanking_off(display_dev);
	k_sleep(K_SECONDS(8));   /* belt-and-braces wait for first refresh */
	display_initialised = true;

	/* Show startup status */
	display_show_status("Initialising modem...", NULL);
	LOG_INF("Display initialised");

	/* ---- Step 2: Start display thread -------------------------------- */
	k_thread_create(&display_thread_data, display_thread_stack,
			DISPLAY_THREAD_STACK_SIZE,
			display_thread, NULL, NULL, NULL,
			K_PRIO_PREEMPT(7), 0, K_NO_WAIT);
	k_thread_name_set(&display_thread_data, "disp_worker");

	/* ---- Step 3: Init modem ------------------------------------------ */
	rc = nrf_modem_lib_init();
	if (rc) {
		LOG_ERR("Modem init failed: %d", rc);
		display_show_status("Modem init failed!", NULL);
		return rc;
	}
	LOG_INF("Modem library initialised");

	/* ---- Step 4: Connect to LTE -------------------------------------- */
	display_show_status("Connecting to LTE...",
			    "This may take 30-120 seconds");

	/* Register event handler, then blocking connect */
	lte_lc_register_handler(lte_handler);
	rc = lte_lc_connect();
	if (rc) {
		LOG_ERR("LTE connect failed: %d", rc);
		display_show_status("LTE connect failed",
				    "Check SIM card and antenna");
		return rc;
	}
	LOG_INF("Connected to LTE");

	/* ---- Step 5: Register SMS listener ------------------------------- */
	int sms_handle = sms_register_listener(sms_callback, NULL);

	if (sms_handle < 0) {
		LOG_ERR("SMS register failed: %d", sms_handle);
		display_show_status("SMS init failed",
				    "Check modem AT+CNMI support");
		return sms_handle;
	}
	LOG_INF("SMS listener registered (handle %d)", sms_handle);

	/* ---- Step 6: Show ready screen ----------------------------------- */
	display_show_status(
		"Ready! Send an SMS to this",
		"device to display it here.");
	LOG_INF("=== Waiting for SMS messages ===");

	/*
	 * Main thread exits; Zephyr kernel keeps running.
	 * SMS callbacks arrive via the modem ISR → sms_callback() queues
	 * messages → display_thread renders them.
	 */
	return 0;
}
