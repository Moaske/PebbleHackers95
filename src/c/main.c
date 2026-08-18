/*
 * Watchface for Pebble Emery (200x228px)
 * - Background image (5 variants, swappable via settings)
 * - Clock (12/24h) + AM/PM + Date: custom LayerUpdateProc with optional drop shadow
 * - Typewriter gimmick: wrist-flick, custom LayerUpdateProc with optional drop shadow
 * - Bottom bar: steps | sleep | battery with MDI icons
 * - Clay settings: colours, typewriter text, background, shadow toggle
 */

#include <pebble.h>

// ── Persistent settings ───────────────────────────────────────────────────────
#define SETTINGS_KEY    42

typedef struct {
  GColor  color_main;        // clock, date, bar text
  GColor  color_typewriter;  // typewriter text
  uint8_t typewriter_text;   // 0 = "Mess with the best...", 1 = "Hack the planet!"
  uint8_t background;        // 0-4
  bool    shadow_on;         // drop shadow behind clock/date/typewriter
} Settings;

static Settings s_settings;

// ── Layout constants ──────────────────────────────────────────────────────────
#define SCREEN_W        200
#define SCREEN_H        228

#define SHADOW_DX       3     // shadow offset X
#define SHADOW_DY       3     // shadow offset Y

// Clock
#define CLOCK_FONT_H    52
#define CLOCK_Y         (SCREEN_H * 2 / 3 - CLOCK_FONT_H / 2 - 20)  // 106
#define CLOCK_H         58

// AM/PM: just right of centered clock digits
#define AMPM_W          30
#define AMPM_H          20
#define AMPM_X          155
#define AMPM_Y          (CLOCK_Y + CLOCK_H - AMPM_H)

// Date: directly below clock
#define DATE_Y          (CLOCK_Y + CLOCK_H)
#define DATE_H          28

// Typewriter: just above clock
#define TYPE_H          58
#define TYPE_Y          (CLOCK_Y - TYPE_H - 4)
#define TYPE_WORD_MS    300
#define TYPE_CLEAR_MS   800

// Bottom bar
#define BAR_H           24
#define BAR_Y           (SCREEN_H - BAR_H)
#define SEC_W_0         66
#define SEC_W_1         66
#define SEC_W_2         68
#define SEC_X_0         0
#define SEC_X_1         (SEC_X_0 + SEC_W_0)
#define SEC_X_2         (SEC_X_1 + SEC_W_1)
#define ICON_W          20
#define VAL_W_0         (SEC_W_0 - ICON_W)
#define VAL_W_1         (SEC_W_1 - ICON_W)
#define VAL_W_2         (SEC_W_2 - ICON_W)

// ── Typewriter data ───────────────────────────────────────────────────────────
static const char * const WORDS_A[] = {
  "Mess", "with", "the", "best\n", "die", "like", "the", "rest!"
};
#define WORD_COUNT_A    8

static const char * const WORDS_B[] = {
  "Hack", "the", "planet!"
};
#define WORD_COUNT_B    3

static const char * const *s_words     = WORDS_A;
static int                 s_word_count = WORD_COUNT_A;

// ── Globals ───────────────────────────────────────────────────────────────────
static Window      *s_window;
static BitmapLayer *s_bg_layer;
static GBitmap     *s_bg_bitmap;

// Custom layers (LayerUpdateProc — replaces TextLayer for clock/date/typewriter)
static Layer       *s_clock_layer;
static Layer       *s_date_layer;
static Layer       *s_type_layer;

// Bottom bar: custom layers for shadow support
static Layer       *s_steps_icon_layer;
static Layer       *s_steps_val_layer;
static Layer       *s_sleep_icon_layer;
static Layer       *s_sleep_val_layer;
static Layer       *s_battery_icon_layer;
static Layer       *s_battery_val_layer;

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
static bool s_show_ampm = false;

// Typewriter state
static char      s_type_buf[64];
static int       s_type_word_idx;
static bool      s_type_visible;
static AppTimer *s_type_timer;

// Light poll
#define LIGHT_POLL_MS   250
static AppTimer *s_light_poll_timer = NULL;
static bool      s_light_was_on     = false;

// MDI icon characters
#define ICON_BATTERY     "!"
#define ICON_WALK        "\""
#define ICON_HEART_PULSE "#"
#define ICON_CHAT_SLEEP  "$"
#define ICON_SHOE        "%"
#define ICON_BATTERY_V   "&"   // mdiBattery rotated 90deg CW

// ── Shadow draw helper ────────────────────────────────────────────────────────
// Draws text twice: shadow pass in black offset by (SHADOW_DX, SHADOW_DY),
// then the main pass in the chosen colour at the original position.
static void draw_text_shadowed(GContext *ctx, const char *text, GFont font,
                                GRect bounds, GTextOverflowMode overflow,
                                GTextAlignment align, GColor color, bool shadow) {
  if (shadow) {
    GRect shadow_bounds = GRect(bounds.origin.x + SHADOW_DX,
                                bounds.origin.y + SHADOW_DY,
                                bounds.size.w, bounds.size.h);
    graphics_context_set_text_color(ctx, GColorBlack);
    graphics_draw_text(ctx, text, font, shadow_bounds, overflow, align, NULL);
  }
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, font, bounds, overflow, align, NULL);
}

// ── Clock layer update proc ───────────────────────────────────────────────────
static void clock_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  bool shadow = s_settings.shadow_on;
  GColor col  = s_settings.color_main;

  // Clock digits, centered
  draw_text_shadowed(ctx, s_clock_buf, s_font_clock, bounds,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, col, shadow);

  // AM/PM, bottom-right of clock box — only in 12h mode
  if (s_show_ampm) {
    GRect ampm_bounds = GRect(AMPM_X - bounds.origin.x,
                              AMPM_Y - bounds.origin.y,
                              AMPM_W, AMPM_H);
    draw_text_shadowed(ctx, s_ampm_buf, s_font_val, ampm_bounds,
                       GTextOverflowModeWordWrap, GTextAlignmentCenter, col, shadow);
  }
}

// ── Date layer update proc ────────────────────────────────────────────────────
static void date_layer_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  draw_text_shadowed(ctx, s_date_buf, s_font_date, bounds,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter,
                     s_settings.color_main, s_settings.shadow_on);
}

// ── Typewriter layer update proc ──────────────────────────────────────────────
static void type_layer_update_proc(Layer *layer, GContext *ctx) {
  if (!s_type_visible || s_type_buf[0] == '\0') return;
  GRect bounds = layer_get_bounds(layer);
  draw_text_shadowed(ctx, s_type_buf, s_font_date, bounds,
                     GTextOverflowModeWordWrap, GTextAlignmentCenter,
                     s_settings.color_typewriter, s_settings.shadow_on);
}

// ── Typewriter logic ──────────────────────────────────────────────────────────
static void typewriter_clear_cb(void *context) {
  s_type_timer   = NULL;
  s_type_visible = false;
  layer_mark_dirty(s_type_layer);
  s_type_buf[0]   = '\0';
  s_type_word_idx = 0;
}

static void typewriter_step_cb(void *context) {
  s_type_timer = NULL;

  if (s_type_word_idx >= s_word_count) {
    s_type_timer = app_timer_register(TYPE_CLEAR_MS, typewriter_clear_cb, NULL);
    return;
  }

  if (s_type_word_idx > 0) {
    strncat(s_type_buf, " ", sizeof(s_type_buf) - strlen(s_type_buf) - 1);
  }
  strncat(s_type_buf, s_words[s_type_word_idx], sizeof(s_type_buf) - strlen(s_type_buf) - 1);
  s_type_word_idx++;

  s_type_visible = true;
  layer_mark_dirty(s_type_layer);

  s_type_timer = app_timer_register(TYPE_WORD_MS, typewriter_step_cb, NULL);
}

static void typewriter_start() {
  if (s_type_timer) {
    app_timer_cancel(s_type_timer);
    s_type_timer = NULL;
  }
  s_type_buf[0]   = '\0';
  s_type_word_idx = 0;
  s_type_visible  = false;
  layer_mark_dirty(s_type_layer);
  s_type_timer = app_timer_register(1, typewriter_step_cb, NULL);
}

// ── Button / tap / light poll ─────────────────────────────────────────────────
static void back_click_handler(ClickRecognizerRef recognizer, void *context) {
  typewriter_start();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, back_click_handler);
}

static void light_poll_callback(void *context) {
  bool now_on = light_is_on();
  if (now_on && !s_light_was_on) {
    typewriter_start();
  }
  s_light_was_on     = now_on;
  s_light_poll_timer = app_timer_register(LIGHT_POLL_MS, light_poll_callback, NULL);
}

static void accel_tap_handler(AccelAxisType axis, int32_t direction) {
  (void)axis; (void)direction;
  typewriter_start();
}

// ── Settings ──────────────────────────────────────────────────────────────────
// ── Bottom bar LayerUpdateProcs ──────────────────────────────────────────────
static void steps_icon_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, ICON_WALK, s_font_icons, b,
    GTextOverflowModeWordWrap, GTextAlignmentCenter, s_settings.color_main, s_settings.shadow_on);
}
static void steps_val_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, s_steps_buf, s_font_val, b,
    GTextOverflowModeWordWrap, GTextAlignmentLeft, s_settings.color_main, s_settings.shadow_on);
}
static void sleep_icon_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, ICON_CHAT_SLEEP, s_font_icons, b,
    GTextOverflowModeWordWrap, GTextAlignmentCenter, s_settings.color_main, s_settings.shadow_on);
}
static void sleep_val_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, s_sleep_buf, s_font_val, b,
    GTextOverflowModeWordWrap, GTextAlignmentLeft, s_settings.color_main, s_settings.shadow_on);
}
static void battery_icon_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, ICON_BATTERY_V, s_font_icons, b,
    GTextOverflowModeWordWrap, GTextAlignmentCenter, s_settings.color_main, s_settings.shadow_on);
}
static void battery_val_update_proc(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  draw_text_shadowed(ctx, s_battery_buf, s_font_val, b,
    GTextOverflowModeWordWrap, GTextAlignmentLeft, s_settings.color_main, s_settings.shadow_on);
}

static void apply_background() {
  if (s_bg_bitmap) gbitmap_destroy(s_bg_bitmap);
  s_bg_bitmap = gbitmap_create_with_resource(
    s_settings.background == 1 ? RESOURCE_ID_BACKGROUND_DARK   :
    s_settings.background == 2 ? RESOURCE_ID_BACKGROUND_PURPLE :
    s_settings.background == 3 ? RESOURCE_ID_BACKGROUND_BLUE   :
    s_settings.background == 4 ? RESOURCE_ID_BACKGROUND_DADE   :
                                  RESOURCE_ID_BACKGROUND);
  bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
}

static void apply_typewriter_text() {
  if (s_settings.typewriter_text == 1) {
    s_words      = WORDS_B;
    s_word_count = WORD_COUNT_B;
  } else {
    s_words      = WORDS_A;
    s_word_count = WORD_COUNT_A;
  }
}

static void apply_colors() {
  layer_mark_dirty(s_clock_layer);
  layer_mark_dirty(s_date_layer);
  layer_mark_dirty(s_type_layer);
  layer_mark_dirty(s_steps_icon_layer);
  layer_mark_dirty(s_steps_val_layer);
  layer_mark_dirty(s_sleep_icon_layer);
  layer_mark_dirty(s_sleep_val_layer);
  layer_mark_dirty(s_battery_icon_layer);
  layer_mark_dirty(s_battery_val_layer);
}

static void load_settings() {
  s_settings.color_main       = GColorWhite;
  s_settings.color_typewriter = GColorWhite;
  s_settings.typewriter_text  = 0;
  s_settings.background       = 0;
  s_settings.shadow_on        = false;
  if (persist_exists(SETTINGS_KEY)) {
    persist_read_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  }
  apply_typewriter_text();
}

// ── Inbox ─────────────────────────────────────────────────────────────────────
static void inbox_received_handler(DictionaryIterator *iter, void *context) {
  Tuple *t;

  t = dict_find(iter, MESSAGE_KEY_ColorMain);
  if (t) s_settings.color_main = GColorFromHEX(t->value->int32);

  t = dict_find(iter, MESSAGE_KEY_ColorTypewriter);
  if (t) s_settings.color_typewriter = GColorFromHEX(t->value->int32);

  t = dict_find(iter, MESSAGE_KEY_TypewriterText);
  if (t) {
    s_settings.typewriter_text = (t->type == TUPLE_CSTRING)
      ? (uint8_t)atoi(t->value->cstring) : (uint8_t)t->value->int32;
    apply_typewriter_text();
  }

  t = dict_find(iter, MESSAGE_KEY_ShadowOn);
  if (t) {
    s_settings.shadow_on = (bool)t->value->int32;
  }

  bool bg_changed = false;
  t = dict_find(iter, MESSAGE_KEY_Background);
  if (t) {
    uint8_t new_bg = (t->type == TUPLE_CSTRING)
      ? (uint8_t)atoi(t->value->cstring) : (uint8_t)t->value->int32;
    if (new_bg != s_settings.background) {
      s_settings.background = new_bg;
      bg_changed = true;
    }
  }

  persist_write_data(SETTINGS_KEY, &s_settings, sizeof(s_settings));
  apply_colors();
  if (bg_changed) apply_background();
}

// ── Clock / date / health update helpers ──────────────────────────────────────
static void update_clock(struct tm *tick_time) {
  if (clock_is_24h_style()) {
    strftime(s_clock_buf, sizeof(s_clock_buf), "%H:%M", tick_time);
    s_show_ampm = false;
  } else {
    strftime(s_clock_buf, sizeof(s_clock_buf), "%I:%M", tick_time);
    strftime(s_ampm_buf,  sizeof(s_ampm_buf),  "%p",    tick_time);
    s_show_ampm = true;
  }
  layer_mark_dirty(s_clock_layer);
}

static void update_date(struct tm *tick_time) {
  strftime(s_date_buf, sizeof(s_date_buf), "%a %d %b", tick_time);
  layer_mark_dirty(s_date_layer);
}

static void update_steps() {
  HealthMetric metric = HealthMetricStepCount;
  HealthServiceAccessibilityMask mask =
    health_service_metric_accessible(metric, time_start_of_today(), time(NULL));
  snprintf(s_steps_buf, sizeof(s_steps_buf),
    (mask & HealthServiceAccessibilityMaskAvailable) ? "%d" : "--",
    (mask & HealthServiceAccessibilityMaskAvailable) ? (int)health_service_sum_today(metric) : 0);
  layer_mark_dirty(s_steps_val_layer);
}

static void update_sleep() {
  HealthMetric metric = HealthMetricSleepSeconds;
  HealthServiceAccessibilityMask mask =
    health_service_metric_accessible(metric, time_start_of_today(), time(NULL));
  if (mask & HealthServiceAccessibilityMaskAvailable) {
    int secs = (int)health_service_sum_today(metric);
    snprintf(s_sleep_buf, sizeof(s_sleep_buf), "%dh%02d", secs/3600, (secs%3600)/60);
  } else {
    snprintf(s_sleep_buf, sizeof(s_sleep_buf), "--");
  }
  layer_mark_dirty(s_sleep_val_layer);
}

static void update_battery(BatteryChargeState state) {
  snprintf(s_battery_buf, sizeof(s_battery_buf), "%d%%", state.charge_percent);
  layer_mark_dirty(s_battery_val_layer);
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

// ── Layer helper (bottom bar custom layers) ──────────────────────────────────
static Layer *make_bar_layer(GRect frame, LayerUpdateProc proc) {
  Layer *layer = layer_create(frame);
  layer_set_update_proc(layer, proc);
  return layer;
}

// ── Window load / unload ──────────────────────────────────────────────────────
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);

  window_set_click_config_provider(window, click_config_provider);

  // Background
  s_bg_bitmap = gbitmap_create_with_resource(RESOURCE_ID_BACKGROUND);
  s_bg_layer  = bitmap_layer_create(GRect(0, 0, SCREEN_W, SCREEN_H));
  bitmap_layer_set_bitmap(s_bg_layer, s_bg_bitmap);
  bitmap_layer_set_compositing_mode(s_bg_layer, GCompOpSet);
  layer_add_child(root, bitmap_layer_get_layer(s_bg_layer));

  // Fonts
  s_font_clock = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_52));
  s_font_date  = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_24));
  s_font_val   = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_CONFIDENTIAL_18));
  s_font_icons = fonts_load_custom_font(resource_get_handle(RESOURCE_ID_FONT_MDI_ICONS_18));

  // Typewriter custom layer
  s_type_layer = layer_create(GRect(0, TYPE_Y, SCREEN_W, TYPE_H));
  layer_set_update_proc(s_type_layer, type_layer_update_proc);
  layer_add_child(root, s_type_layer);

  // Clock custom layer (spans full width; AM/PM drawn inside it)
  s_clock_layer = layer_create(GRect(0, CLOCK_Y, SCREEN_W, CLOCK_H));
  layer_set_update_proc(s_clock_layer, clock_layer_update_proc);
  layer_add_child(root, s_clock_layer);

  // Date custom layer
  s_date_layer = layer_create(GRect(0, DATE_Y, SCREEN_W, DATE_H));
  layer_set_update_proc(s_date_layer, date_layer_update_proc);
  layer_add_child(root, s_date_layer);

  // Bottom bar
  s_steps_icon_layer = make_bar_layer(
    GRect(SEC_X_0, BAR_Y, ICON_W, BAR_H), steps_icon_update_proc);
  layer_add_child(root, s_steps_icon_layer);

  s_steps_val_layer = make_bar_layer(
    GRect(SEC_X_0 + ICON_W, BAR_Y, VAL_W_0, BAR_H), steps_val_update_proc);
  layer_add_child(root, s_steps_val_layer);

  s_sleep_icon_layer = make_bar_layer(
    GRect(SEC_X_1, BAR_Y, ICON_W, BAR_H), sleep_icon_update_proc);
  layer_add_child(root, s_sleep_icon_layer);

  s_sleep_val_layer = make_bar_layer(
    GRect(SEC_X_1 + ICON_W, BAR_Y, VAL_W_1, BAR_H), sleep_val_update_proc);
  layer_add_child(root, s_sleep_val_layer);

  s_battery_icon_layer = make_bar_layer(
    GRect(SEC_X_2, BAR_Y, ICON_W, BAR_H), battery_icon_update_proc);
  layer_add_child(root, s_battery_icon_layer);

  s_battery_val_layer = make_bar_layer(
    GRect(SEC_X_2 + ICON_W, BAR_Y, VAL_W_2, BAR_H), battery_val_update_proc);
  layer_add_child(root, s_battery_val_layer);

  // Initial values
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  update_clock(t);
  update_date(t);
  update_steps();
  update_sleep();
  update_battery(battery_state_service_peek());
  apply_colors();
  apply_background();
}

static void window_unload(Window *window) {
  if (s_type_timer) {
    app_timer_cancel(s_type_timer);
    s_type_timer = NULL;
  }

  layer_destroy(s_type_layer);
  layer_destroy(s_clock_layer);
  layer_destroy(s_date_layer);

  layer_destroy(s_steps_icon_layer);
  layer_destroy(s_steps_val_layer);
  layer_destroy(s_sleep_icon_layer);
  layer_destroy(s_sleep_val_layer);
  layer_destroy(s_battery_icon_layer);
  layer_destroy(s_battery_val_layer);

  bitmap_layer_destroy(s_bg_layer);
  gbitmap_destroy(s_bg_bitmap);

  fonts_unload_custom_font(s_font_clock);
  fonts_unload_custom_font(s_font_date);
  fonts_unload_custom_font(s_font_val);
  fonts_unload_custom_font(s_font_icons);
}

// ── Init / deinit ─────────────────────────────────────────────────────────────
static void init() {
  s_type_timer    = NULL;
  s_type_buf[0]   = '\0';
  s_type_word_idx = 0;
  s_type_visible  = false;

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

  s_light_was_on     = light_is_on();
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