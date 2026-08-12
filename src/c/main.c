/*
 * Watchface for Pebble Emery
 * - Background image
 * - Typewriter gimmick: wrist-flick triggers word-by-word text, then clears
 * - Clock (12/24h), centered ~2/3 down
 * - Date line  Ddd DD Mon, follows watch language
 * - Bottom bar: 3 sections, each split 20px icon + value (FONT_CONFIDENTIAL_18)
 *
 * Emery display: 200x228 px
 */

#include <pebble.h>

// ── Persistent settings ───────────────────────────────────────────────────────
#define SETTINGS_KEY    42

typedef struct {
  GColor color_main;        // clock, date, bar text
  GColor color_typewriter;  // typewriter text
} Settings;

static Settings s_settings;

// ── Layout constants ──────────────────────────────────────────────────────────
#define SCREEN_W        200
#define SCREEN_H        228

// Typewriter box: just above the clock, with a 4px gap
#define TYPE_H          58    // room for 2 lines at 24px + leading
#define TYPE_Y          (CLOCK_Y - TYPE_H - 4)
#define TYPE_WORD_MS    300   // delay between each word appearing
#define TYPE_CLEAR_MS   800   // pause after last word before clearing

// Clock: centered around 2/3 down
#define CLOCK_FONT_H    52
#define CLOCK_Y         (SCREEN_H * 2 / 3 - CLOCK_FONT_H / 2 - 20)  // 106
#define CLOCK_H         58

// AM/PM label: just right of the centered clock text.
// At 52px font, "00:00" is ~110px wide, centered in 200px → right edge ~155px
#define AMPM_W          30
#define AMPM_H          20
#define AMPM_X          155
#define AMPM_Y          (CLOCK_Y + CLOCK_H - AMPM_H)

// Date: directly below clock
#define DATE_Y          (CLOCK_Y + CLOCK_H)                           // 164
#define DATE_H          28

// Bottom bar: 24px tall at very bottom
#define BAR_H           24
#define BAR_Y           (SCREEN_H - BAR_H)                            // 204

// Three equal sections: 200 / 3
#define SEC_W_0         66
#define SEC_W_1         66
#define SEC_W_2         68
#define SEC_X_0         0
#define SEC_X_1         (SEC_X_0 + SEC_W_0)
#define SEC_X_2         (SEC_X_1 + SEC_W_1)

// Within each section: 20px icon slot, rest is value slot
#define ICON_W          20
#define VAL_W_0         (SEC_W_0 - ICON_W)
#define VAL_W_1         (SEC_W_1 - ICON_W)
#define VAL_W_2         (SEC_W_2 - ICON_W)

// ── Typewriter data ───────────────────────────────────────────────────────────
// Words of the two-line message; \n marks the line break position
static const char * const WORDS[] = {
  "Mess", "with", "the", "best\n", "die", "like", "the", "rest!"
};
#define WORD_COUNT      8

// ── Globals ───────────────────────────────────────────────────────────────────
static Window       *s_window;

static BitmapLayer  *s_bg_layer;
static GBitmap      *s_bg_bitmap;

static TextLayer    *s_type_layer;
static TextLayer    *s_clock_layer;
static TextLayer    *s_ampm_layer;
static TextLayer    *s_date_layer;

static TextLayer    *s_steps_icon_layer;
static TextLayer    *s_steps_val_layer;
static TextLayer    *s_sleep_icon_layer;
static TextLayer    *s_sleep_val_layer;
static TextLayer    *s_battery_icon_layer;
static TextLayer    *s_battery_val_layer;

static GFont        s_font_clock;
static GFont        s_font_date;
static GFont        s_font_val;
static GFont        s_font_icons;

static char s_clock_buf[8];
static char s_ampm_buf[4];
static char s_date_buf[16];
static char s_steps_buf[10];
static char s_sleep_buf[10];
static char s_battery_buf[8];

// Typewriter state
static char         s_type_buf[64];   // accumulates words as they appear
static int          s_type_word_idx;  // next word to add (0 = not started)
static AppTimer    *s_type_timer;     // active timer handle

// Light poll: rising-edge detection for backlight on (wrist flick, button, tap)
// Identical approach to MetroWP8 — catches all causes of backlight turning on,
// including emulator button presses where accel_tap never fires.
#define LIGHT_POLL_MS     250
static AppTimer *s_light_poll_timer = NULL;
static bool      s_light_was_on     = false;

// MDI icon characters (from PebbleMDIcons18.ttf)
#define ICON_BATTERY     "!"
#define ICON_WALK        "\""
#define ICON_HEART_PULSE "#"
#define ICON_CHAT_SLEEP  "$"
#define ICON_SHOE        "%"

// ── Typewriter logic ──────────────────────────────────────────────────────────

// Called after the clear delay: hide the layer and reset state
static void typewriter_clear_cb(void *context) {
  s_type_timer = NULL;
  layer_set_hidden(text_layer_get_layer(s_type_layer), true);
  s_type_buf[0]    = '\0';
  s_type_word_idx  = 0;
}

// Called each TYPE_WORD_MS to append the next word
static void typewriter_step_cb(void *context) {
  s_type_timer = NULL;

  if (s_type_word_idx >= WORD_COUNT) {
    // All words shown — wait then clear
    s_type_timer = app_timer_register(TYPE_CLEAR_MS, typewriter_clear_cb, NULL);
    return;
  }

  // Append next word (with a space separator after the first)
  if (s_type_word_idx > 0) {
    strncat(s_type_buf, " ", sizeof(s_type_buf) - strlen(s_type_buf) - 1);
  }
  strncat(s_type_buf, WORDS[s_type_word_idx], sizeof(s_type_buf) - strlen(s_type_buf) - 1);
  s_type_word_idx++;

  text_layer_set_text(s_type_layer, s_type_buf);
  layer_set_hidden(text_layer_get_layer(s_type_layer), false);

  // Schedule next word
  s_type_timer = app_timer_register(TYPE_WORD_MS, typewriter_step_cb, NULL);
}

// Kick off or restart the typewriter sequence
static void typewriter_start() {
  // Cancel any running sequence
  if (s_type_timer) {
    app_timer_cancel(s_type_timer);
    s_type_timer = NULL;
  }
  s_type_buf[0]   = '\0';
  s_type_word_idx = 0;
  layer_set_hidden(text_layer_get_layer(s_type_layer), true);

  // First word fires immediately
  s_type_timer = app_timer_register(1, typewriter_step_cb, NULL);
}

// ── Button handler (select = typewriter trigger, useful for emulator/GIF) ─────
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  typewriter_start();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, select_click_handler);
}

// ── Light poll: rising-edge trigger (works in emulator too) ─────────────────
static void light_poll_callback(void *context) {
  bool now_on = light_is_on();
  if (now_on && !s_light_was_on) {
    typewriter_start();
  }
  s_light_was_on = now_on;
  s_light_poll_timer = app_timer_register(LIGHT_POLL_MS, light_poll_callback, NULL);
}

// ── Tap / flick handler ───────────────────────────────────────────────────────
static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  // Fire on any tap/flick — remove axis filter to ensure it triggers
  (void)axis;
  (void)direction;
  typewriter_start();
}

// ── Settings helpers ────────────────────────────────────────────────────────
static void apply_colors() {
  text_layer_set_text_color(s_clock_layer,        s_settings.color_main);
  text_layer_set_text_color(s_ampm_layer,         s_settings.color_main);
  text_layer_set_text_color(s_date_layer,         s_settings.color_main);
  text_layer_set_text_color(s_steps_icon_layer,   s_settings.color_main);
  text_layer_set_text_color(s_steps_val_layer,    s_settings.color_main);
  text_layer_set_text_color(s_sleep_icon_layer,   s_settings.color_main);
  text_layer_set_text_color(s_sleep_val_layer,    s_settings.color_main);
  text_layer_set_text_color(s_battery_icon_layer, s_settings.color_main);
  text_layer_set_text_color(s_battery_val_layer,  s_settings.color_main);
  text_layer_set_text_color(s_type_layer,         s_settings.color_typewriter);
}

static void load_settings() {
  // Defaults: white for everything
  s_settings.color_main       = GColorWhite;
  s_settings.color_typewriter = GColorWhite;
  if (persist_exists(SETTINGS_KEY)) {
    persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  }
}

// ── Inbox: receive settings from Clay ────────────────────────────────────────
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;

  t = dict_find(iter, MESSAGE_KEY_ColorMain);
  if (t) s_settings.color_main = GColorFromHEX(t->value->int32);

  t = dict_find(iter, MESSAGE_KEY_ColorTypewriter);
  if (t) s_settings.color_typewriter = GColorFromHEX(t->value->int32);

  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  apply_colors();
}

// ── Clock / date / health update helpers ─────────────────────────────────────
static void update_clock(struct tm *tick_time) {
  if (clock_is_24h_style()) {
    strftime(s_clock_buf, sizeof(s_clock_buf), "%H:%M", tick_time);
    layer_set_hidden(text_layer_get_layer(s_ampm_layer), true);
  } else {
    strftime(s_clock_buf, sizeof(s_clock_buf), "%I:%M", tick_time);
    strftime(s_ampm_buf, sizeof(s_ampm_buf), "%p", tick_time);
    text_layer_set_text(s_ampm_layer, s_ampm_buf);
    layer_set_hidden(text_layer_get_layer(s_ampm_layer), false);
  }
  text_layer_set_text(s_clock_layer, s_clock_buf);
}

static void update_date(struct tm *tick_time) {
  strftime(s_date_buf, sizeof(s_date_buf), "%a %d %b", tick_time);
  text_layer_set_text(s_date_layer, s_date_buf);
}

static void update_steps() {
  HealthMetric metric = HealthMetricStepCount;
  HealthServiceAccessibilityMask mask =
    health_service_metric_accessible(metric, time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    int steps = (int)health_service_sum_today(metric);
    snprintf(s_steps_buf, sizeof(s_steps_buf), "%d", steps);
  } else {
    snprintf(s_steps_buf, sizeof(s_steps_buf), "--");
  }
  text_layer_set_text(s_steps_val_layer, s_steps_buf);
}

static void update_sleep() {
  HealthMetric metric = HealthMetricSleepSeconds;
  HealthServiceAccessibilityMask mask =
    health_service_metric_accessible(metric, time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    int secs  = (int)health_service_sum_today(metric);
    int hours = secs / 3600;
    int mins  = (secs % 3600) / 60;
    snprintf(s_sleep_buf, sizeof(s_sleep_buf), "%dh%02d", hours, mins);
  } else {
    snprintf(s_sleep_buf, sizeof(s_sleep_buf), "--");
  }
  text_layer_set_text(s_sleep_val_layer, s_sleep_buf);
}

static void update_battery(BatteryChargeState state) {
  snprintf(s_battery_buf, sizeof(s_battery_buf), "%d%%", state.charge_percent);
  text_layer_set_text(s_battery_val_layer, s_battery_buf);
}

// ── Service callbacks ─────────────────────────────────────────────────────────
static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  update_clock(tick_time);
  update_date(tick_time);
  update_steps();
  update_sleep();
}

static void battery_handler(BatteryChargeState state) {
  update_battery(state);
}

static void health_handler(HealthEventType event, void *context) {
  if (event == HealthEventMovementUpdate || event == HealthEventSleepUpdate) {
    update_steps();
    update_sleep();
  }
}

// ── Layer helpers ─────────────────────────────────────────────────────────────
static TextLayer *make_icon_layer(GRect frame, const char *icon_char, GFont font) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, GColorWhite);
  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, GTextAlignmentCenter);
  text_layer_set_text(layer, icon_char);
  return layer;
}

static TextLayer *make_val_layer(GRect frame, GFont font) {
  TextLayer *layer = text_layer_create(frame);
  text_layer_set_background_color(layer, GColorClear);
  text_layer_set_text_color(layer, GColorWhite);
  text_layer_set_font(layer, font);
  text_layer_set_text_alignment(layer, GTextAlignmentLeft);
  return layer;
}

// ── Window load / unload ──────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  window_set_click_config_provider(window, click_config_provider);

  // Background image
  s_bg_bitmap = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);
  s_bg_layer  = bitmap_layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
  bitmap_layer_set_compositing_mode(s_bg_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_bg_layer));

  // Load fonts
  s_font_clock  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_52));
  s_font_date   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_24));
  s_font_val    = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_18));
  s_font_icons  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_MDI_ICONS_18));

  // Typewriter layer — hidden until flick triggers it
  s_type_layer = text_layer_create(GRect(0, TYPE_Y, SCREEN_W, TYPE_H));
  text_layer_set_background_color(s_type_layer, GColorClear);
  text_layer_set_text_color(s_type_layer, GColorWhite);
  text_layer_set_font(s_type_layer, s_font_date);
  text_layer_set_text_alignment(s_type_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_type_layer, GTextOverflowModeWordWrap);
  layer_set_hidden(text_layer_get_layer(s_type_layer), true);
  layer_add_child(root, text_layer_get_layer(s_type_layer));

  // Clock layer
  s_clock_layer = text_layer_create(GRect(0, CLOCK_Y, SCREEN_W, CLOCK_H));
  text_layer_set_background_color(s_clock_layer, GColorClear);
  text_layer_set_text_color(s_clock_layer, GColorWhite);
  text_layer_set_font(s_clock_layer, s_font_clock);
  text_layer_set_text_alignment(s_clock_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_clock_layer));

  // AM/PM layer — bottom-right of clock, hidden in 24h mode
  s_ampm_layer = text_layer_create(GRect(AMPM_X, AMPM_Y, AMPM_W, AMPM_H));
  text_layer_set_background_color(s_ampm_layer, GColorClear);
  text_layer_set_text_color(s_ampm_layer, GColorWhite);
  text_layer_set_font(s_ampm_layer, s_font_val);
  text_layer_set_text_alignment(s_ampm_layer, GTextAlignmentCenter);
  layer_set_hidden(text_layer_get_layer(s_ampm_layer), !clock_is_24h_style() ? false : true);
  layer_add_child(root, text_layer_get_layer(s_ampm_layer));

  // Date layer
  s_date_layer = text_layer_create(GRect(0, DATE_Y, SCREEN_W, DATE_H));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, s_font_date);
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);
  layer_add_child(root, text_layer_get_layer(s_date_layer));

  // ── Bottom bar ─────────────────────────────────────────────────────────────
  // Section 0: Steps  (x=0, w=66)
  s_steps_icon_layer = make_icon_layer(
    GRect(SEC_X_0, BAR_Y, ICON_W, BAR_H), ICON_SHOE, s_font_icons);
  layer_add_child(root, text_layer_get_layer(s_steps_icon_layer));

  s_steps_val_layer = make_val_layer(
    GRect(SEC_X_0 + ICON_W, BAR_Y, VAL_W_0, BAR_H), s_font_val);
  layer_add_child(root, text_layer_get_layer(s_steps_val_layer));

  // Section 1: Sleep  (x=66, w=66)
  s_sleep_icon_layer = make_icon_layer(
    GRect(SEC_X_1, BAR_Y, ICON_W, BAR_H), ICON_CHAT_SLEEP, s_font_icons);
  layer_add_child(root, text_layer_get_layer(s_sleep_icon_layer));

  s_sleep_val_layer = make_val_layer(
    GRect(SEC_X_1 + ICON_W, BAR_Y, VAL_W_1, BAR_H), s_font_val);
  layer_add_child(root, text_layer_get_layer(s_sleep_val_layer));

  // Section 2: Battery  (x=132, w=68)
  s_battery_icon_layer = make_icon_layer(
    GRect(SEC_X_2, BAR_Y, ICON_W, BAR_H), ICON_BATTERY, s_font_icons);
  layer_add_child(root, text_layer_get_layer(s_battery_icon_layer));

  s_battery_val_layer = make_val_layer(
    GRect(SEC_X_2 + ICON_W, BAR_Y, VAL_W_2, BAR_H), s_font_val);
  layer_add_child(root, text_layer_get_layer(s_battery_val_layer));

  // Initial values
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  update_date(t);
  update_steps();
  update_sleep();
  update_battery(battery_state_service_peek());
  apply_colors();
}

static void window_unload(Window *window) {
  // Cancel any running typewriter timer
  if (s_type_timer) {
    app_timer_cancel(s_type_timer);
    s_type_timer = NULL;
  }

  text_layer_destroy(s_type_layer);
  text_layer_destroy(s_clock_layer);
  text_layer_destroy(s_ampm_layer);
  text_layer_destroy(s_date_layer);

  text_layer_destroy(s_steps_icon_layer);
  text_layer_destroy(s_steps_val_layer);
  text_layer_destroy(s_sleep_icon_layer);
  text_layer_destroy(s_sleep_val_layer);
  text_layer_destroy(s_battery_icon_layer);
  text_layer_destroy(s_battery_val_layer);

  bitmap_layer_destroy(s_bg_layer);
  gbitmap_destroy(s_bg_bitmap);

  fonts_unload_custom_font(s_font_clock);
  fonts_unload_custom_font(s_font_date);
  fonts_unload_custom_font(s_font_val);
  fonts_unload_custom_font(s_font_icons);
}

// ── App init / deinit ─────────────────────────────────────────────────────────
static void init() {
  s_type_timer    = NULL;
  s_type_buf[0]   = '\0';
  s_type_word_idx = 0;

  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load   = window_load,
    .unload = window_unload,
  });

  load_settings();

  window_stack_push(s_window, true);

  app_message_register_inbox_received(inbox_received_handler);
  app_message_open(128, 128);

  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  battery_state_service_subscribe(battery_handler);
  health_service_events_subscribe(health_handler, NULL);
  accel_tap_service_subscribe(accel_tap_handler);

  s_light_was_on    = light_is_on();
  s_light_poll_timer = app_timer_register(LIGHT_POLL_MS, light_poll_callback, NULL);
}

static void deinit() {
  tick_timer_service_unsubscribe();
  battery_state_service_unsubscribe();
  health_service_events_unsubscribe();
  accel_tap_service_unsubscribe();
  if (s_light_poll_timer) {
    app_timer_cancel(s_light_poll_timer);
    s_light_poll_timer = NULL;
  }
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}