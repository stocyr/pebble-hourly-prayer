#include <pebble.h>

typedef struct {
  int wday;
  int hour;
  int minute;
} TimeInWeek;

TimeInWeek last_prayer_of_week = {4, 13, 00}; // Stop reminding after Sunday 13:00 

static Window *s_menu_window, *s_preview_window, *s_prayer_window, *s_reminder_window;
static TextLayer *s_next_prayer_text_layer, *s_next_prayer_time_text_layer, *s_cancel_text_layer;
static TextLayer *s_prayer_title_text_layer, *s_prayer_head_text_layer, *s_prayer_body_text_layer;
static TextLayer *s_reminder_title_text_layer;
static BitmapLayer *s_bitmap_layer;
static Layer *s_prayer_canvas_layer;
static GBitmap *s_cancel_bitmap, *s_confirm_bitmap, *s_snooze_bitmap, *s_large_cross_bitmap;
static AppTimer *s_update_countdown_timer;
static ActionBarLayer *s_action_bar_layer;
static ScrollLayer *s_scroll_layer;

static WakeupId s_wakeup_id = -1;
static time_t s_wakeup_timestamp = 0;
static char s_countdown_text[30];
static int s_next_prayer_id = -1;
static int s_current_prayer_id = -1;

typedef struct {
  char title[11];
  char head[28];
  int hour;
  int minute;
  char body[290];
} Prayer;

// Array of liturgy of the hours prayers  ü ö ä
Prayer hours_array[] = {
  {"Laudes",      "Gebet zur ersten Stunde",      9, 15, "Herr Jesus Christus, du unser Gott und wahres Licht, erhelle unsere Sinne und Gedanken, lass und strahlen, damit uns die Dunkelheit nicht überdecke, sondern wir dich mit David preisen und ausrufen: Meine Augen eilen den Nachtwachen voraus, denn ich sinne nach über deine Verheissungen."},
  {"Terz",        "Gebet zur dritten Stunde",    10, 30, "Nimm den Heiligen Geist nicht hinweg von uns, du Gerechter. Vielmehr bitten wir dich: Erneuere in uns einen aufrechten und lebensspendenen Geist, einen Geist der Sohnschaft und Lauterkeit, einen Geist der Heiligkeit, Gerechtigkeit und Vollmacht, o Allmächtiger."},
  {"Sext",        "Gebet zur sechsten Stunde",   11, 45, "Tilge unsere Qualen durch dein heilbringendes und lebensspendendes Leiden und die Nägel, an denen du gehangen. Gib uns, o Gott, eine Zeit der Freude, einen Wandel ohne Makel, ein Leben in Frieden, damit wir deinem heiligen Namen gerecht werden."},
  {"Non",         "Gebet zur neunten Stunde",    13, 00, "Wende ab unseren Sinn von irdischer Sorge und sinnlicher Genusssucht und erlöse uns. Erhöre auch uns, du gerechter, die wir den Todesspruch verdienen ob unserer Sünden. Gedenke unser, o Herr, wenn du in dein Reich kommst."},
  {"Vesper",      "Gebet zur elften Stunde",     14, 15, "Tilge, vergib und verzeih uns unsere Missetaten, o Gott, die freiwilligen und unfreiwilligen, die bewussten und unbewussten, die sichtbaren und unsichtbaren. Herr, vergib uns um deines heiligen Namens willen, der über uns ausgerufen ist."},
  {"Komplet",     "Gebet zur zwölften Stunde",   15, 30, "Lass deine Barmherzigkeit über uns kommen, wie wir uns auf dich, o Herr, verlassen, denn alle Augen warten auf dich, dass du ihnen Speise gibst zur rechten Zeit. Erhöre auch uns, Gott, unser Erlöser, Hoffnung der ganzen Erde."},
  {"Nacht",       "Mitternachtsgebet",           16, 45, "Kehre gnädig ein und erfülle uns, wasche uns von Makel rein, du Gerechter, und errette unsere Seelen. Wie du mit deinen Jüngern warst und ihnen den Frieden gabst, o Heiland, so komme auch zu uns und gib uns deinen Frieden, errette uns und erlöse unsere Seelen."}
};
int num_hours = ARRAY_LENGTH(hours_array);

static const uint32_t const segments[] = { 100, 2000, 250, 450, 250, 450, 250, 450, 250, 450, 250, 450, 250, 450, 250, 450, 250 };
VibePattern pat = {
  .durations = segments,
  .num_segments = ARRAY_LENGTH(segments),
};

enum {
  PERSIST_WAKEUP // Persistent storage key for wakeup_id
};



static int find_next_prayer(time_t *future_timestamp) {
  bool return_next_weeks_start = false;
  // Get current time
  time_t current_timestamp = time(NULL);
  struct tm* current_time = localtime(&current_timestamp);
  
  // Handle the weekend pause
  // Weekdays: https://sourceware.org/newlib/libc.html#Timefns
  int weekday_starting_monday = (current_time->tm_wday - 1) % 7;
  if (weekday_starting_monday > last_prayer_of_week.wday) {
    return_next_weeks_start = true;
  } else if (weekday_starting_monday == last_prayer_of_week.wday) {
    if (current_time->tm_hour > last_prayer_of_week.hour) {
      return_next_weeks_start = true;
    } else if (current_time->tm_hour == last_prayer_of_week.hour) {
      if (current_time->tm_min >= last_prayer_of_week.minute) {
        return_next_weeks_start = true;
      }
    }
  }

  if (return_next_weeks_start) {
    *future_timestamp = clock_to_timestamp(MONDAY, hours_array[0].hour, hours_array[0].minute);
    char tmp[30];
    strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(future_timestamp));
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Find next prayer -> it is weekend. Time: %s", tmp);
    return 0;
  }

  // If normal work week, just search for the next timestamp in the prayers
  int current_day_minutes = current_time->tm_hour * MINUTES_PER_HOUR + current_time->tm_min;
  for (int prayer_hour = 0; prayer_hour < num_hours; ++prayer_hour) {
    int prayer_day_minutes = hours_array[prayer_hour].hour * MINUTES_PER_HOUR + hours_array[prayer_hour].minute;
    if (current_day_minutes < prayer_day_minutes) {
      // Next prayer is today, just later
      *future_timestamp = clock_to_timestamp(TODAY, hours_array[prayer_hour].hour, hours_array[prayer_hour].minute);

      char tmp[30];
      strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(future_timestamp));
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Find next prayer -> still today: Time: %s", tmp);

      return prayer_hour;
    }
  }

  // If we made it until here, that means we're already after the last prayer --> schedule it for tomorrow
  // `wday + 2` because (+1) the WeekDay enum starts with TODAY and (+1) we want tomorrow.
  *future_timestamp = clock_to_timestamp(current_time->tm_wday + 2, hours_array[0].hour, hours_array[0].minute);

  char tmp[30];
  strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(future_timestamp));
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Find next prayer -> tomorrow: Time: %s", tmp);

  return 0;
}

void schedule_wakeup_time(time_t *timestamp, int32_t cookie) {
  // Cancel any existing wakeup first
  if (s_wakeup_id >= 0) {
    wakeup_cancel(s_wakeup_id);
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Cancelled existing wakeup id: %d", (int)s_wakeup_id);
  }
  
  // Schedule the new wakeup
  char tmp[30];
  strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(timestamp));
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Scheduling prayer \"%d\" for %s", (int)cookie, tmp);
  s_wakeup_id = wakeup_schedule(*timestamp, cookie, false);
  if (s_wakeup_id < 0) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "Scheduling returned %d!", (signed int)s_wakeup_id);
    if (s_wakeup_id == E_RANGE) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Since we got an \"E_RANGE\" error, canceling all scheduled wakeups and retrying...");
      wakeup_cancel_all();
      s_wakeup_id = wakeup_schedule(*timestamp, cookie, false);
      if (s_wakeup_id < 0) {
        APP_LOG(APP_LOG_LEVEL_ERROR, "Scheduling returned %d AGAIN!", (signed int)s_wakeup_id);
      } else {
        APP_LOG(APP_LOG_LEVEL_DEBUG, "Successfully scheduled wakeup, writing persistent storage: %d", (int)s_wakeup_id);
        persist_write_int(PERSIST_WAKEUP, s_wakeup_id);
      }
    }
  } else {
    // Store the handle so we can cancel if necessary, or look it up next launch
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Successful scheduled wakeup --> write wakeup id to persistent storage %d", (int)s_wakeup_id);
    persist_write_int(PERSIST_WAKEUP, s_wakeup_id);
  }
}

static void timer_handler(void *data) {
  char tmp[30];
  if (s_wakeup_timestamp == 0) {
    // get the wakeup timestamp for showing a countdown
    wakeup_query(s_wakeup_id, &s_wakeup_timestamp);
  }

  // Find out how far in the future the wakeup timestamp is
  time_t now = time(NULL);
  struct tm current_time = *localtime(&now);
  struct tm future_time = *localtime(&s_wakeup_timestamp);
  strftime(tmp, sizeof(tmp), "%D %H:%M", &future_time);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "wakeup timestamp: %s", tmp);
  strftime(tmp, sizeof(tmp), "%D %H:%M", &current_time);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "current timestamp: %s", tmp);

  strcpy(s_countdown_text, "");

  if (future_time.tm_mday != current_time.tm_mday) {
    // If the next scheduled wakeup is not today, also show the day
    strftime(tmp, sizeof(tmp), "%A", &future_time);
    strcat(s_countdown_text, tmp);
    strcat(s_countdown_text, "\n");
    APP_LOG(APP_LOG_LEVEL_DEBUG, "current text (starting with day name): %s", s_countdown_text);
  }
  // add string 'hh:mm's_countdown_text
  strftime(tmp, sizeof(tmp), "%H:%M", &future_time);
  strcat(s_countdown_text, tmp);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "current text (starting with time): %s", s_countdown_text);

  if (future_time.tm_mday == current_time.tm_mday) {
    int minute_countdown = (future_time.tm_hour * MINUTES_PER_HOUR + future_time.tm_min) - (current_time.tm_hour * MINUTES_PER_HOUR + current_time.tm_min);
    if (minute_countdown < 60) {
      // add string 'in ... minutes'
      snprintf(tmp, sizeof(tmp), "\n(in %d minutes)", minute_countdown);
      strcat(s_countdown_text, tmp);
      APP_LOG(APP_LOG_LEVEL_DEBUG, "current text (with countdown, since < 1h delta): %s", s_countdown_text);
    }
  }

  layer_mark_dirty(text_layer_get_layer(s_next_prayer_time_text_layer));
  s_update_countdown_timer = app_timer_register(1000 * SECONDS_PER_MINUTE, timer_handler, data);
}

static void preview_screen_back_handler(ClickRecognizerRef recognizer, void *context) {
  app_timer_cancel(s_update_countdown_timer);
  window_stack_pop_all(true); // Exit app
}

// Cancel the current wakeup event on the countdown screen
static void preview_screen_cancel_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Clicked down --> remove wakeup");
  app_timer_cancel(s_update_countdown_timer);
  wakeup_cancel_all();
  s_wakeup_id = -1;
  persist_delete(PERSIST_WAKEUP);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Then exit the app.");
  window_stack_pop_all(true); // Exit app
}

static void preview_screen_up_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Clicked up --> debug show next prayer");

  // Since we didn't enter the below window through the wakeup_handler which would provide
  // a "reason" (cookie with the prayer id), we have to provide this by ourselves
  time_t tmp;
  s_current_prayer_id = find_next_prayer(&tmp);
  //s_current_prayer_id = (time(NULL) / 10) % num_hours;
  //s_current_prayer_id = num_hours - 2;

  // Remove the preview window
  window_stack_pop(false);
  // Display the prayer window
  window_stack_push(s_reminder_window, true);
  // Start vibration pattern
  vibes_enqueue_custom_pattern(pat);
}

static void preview_screen_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_BACK, preview_screen_back_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, preview_screen_cancel_handler);
  // Hidden feature: if pressing top, the next prayer is launched directly
  window_single_click_subscribe(BUTTON_ID_UP, preview_screen_up_handler);
}

static void preview_window_load(Window *window) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Loading the preview window");
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  window_set_click_config_provider(window, preview_screen_click_config_provider);

  s_next_prayer_text_layer = text_layer_create(GRect(0, 10, bounds.size.w, 40));
  text_layer_set_text(s_next_prayer_text_layer, "Next prayer");
  text_layer_set_font(s_next_prayer_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD));
  text_layer_set_text_alignment(s_next_prayer_text_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_next_prayer_text_layer));

  s_next_prayer_time_text_layer = text_layer_create(GRect(0, 45, bounds.size.w, 80));
  text_layer_set_text(s_next_prayer_time_text_layer, s_countdown_text);
  text_layer_set_font(s_next_prayer_time_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24));
  text_layer_set_text_alignment(s_next_prayer_time_text_layer, GTextAlignmentCenter);
  layer_add_child(window_layer, text_layer_get_layer(s_next_prayer_time_text_layer));

  // Place a cancel text next to the bottom button to cancel wakeup timer
  s_cancel_text_layer = text_layer_create(GRect(0, 130, bounds.size.w, 28));
  text_layer_set_text(s_cancel_text_layer, "Disable prayers: ");
  text_layer_set_font(s_cancel_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD));
  text_layer_set_text_alignment(s_cancel_text_layer, GTextAlignmentRight);
  layer_add_child(window_layer, text_layer_get_layer(s_cancel_text_layer));

  s_wakeup_timestamp = 0;
  s_update_countdown_timer = app_timer_register(0, timer_handler, NULL);
}

static void preview_window_unload(Window *window) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Unloading the preview window");
  text_layer_destroy(s_next_prayer_time_text_layer);
  text_layer_destroy(s_cancel_text_layer);
  text_layer_destroy(s_next_prayer_text_layer);
}

static void prayer_screen_click_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Anything was pressed --> leaving.");
  // Exit app
  window_stack_pop_all(true);
}

static void prayer_click_config_provider(void *context) {
  //window_single_click_subscribe(BUTTON_ID_SELECT, prayer_screen_click_handler); // can be used to turn on the backlight
  //window_single_click_subscribe(BUTTON_ID_UP, prayer_screen_click_handler);     // is used for scrolling
  //window_single_click_subscribe(BUTTON_ID_DOWN, prayer_screen_click_handler);   // is used for scrolling
  window_single_click_subscribe(BUTTON_ID_BACK, prayer_screen_click_handler);
}

static void prayer_canvas_update_proc(Layer *layer, GContext *ctx) {
  // Here's where the prayer window drawing happens
  // Set the line color
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  // Set the stroke width (must be an odd integer value)
  graphics_context_set_stroke_width(ctx, 1);
  GPoint start = GPoint(0, 67);
  GPoint end = GPoint(layer_get_bounds(layer).size.w, 67);
  // Draw a line
  graphics_draw_line(ctx, start, end);
}

static void prayer_window_load(Window *window) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Loading the prayer window - prayer \"%d\"", s_current_prayer_id);
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  window_set_background_color(window, GColorBlack);

  s_prayer_title_text_layer = text_layer_create(GRect(0, -8, bounds.size.w, 34));
  text_layer_set_text(s_prayer_title_text_layer, hours_array[s_current_prayer_id].title);
  text_layer_set_font(s_prayer_title_text_layer, fonts_get_system_font(FONT_KEY_DROID_SERIF_28_BOLD));
  text_layer_set_text_alignment(s_prayer_title_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_prayer_title_text_layer, GColorBlack);
  text_layer_set_text_color(s_prayer_title_text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_prayer_title_text_layer));

  s_prayer_head_text_layer = text_layer_create(GRect(0, 26, bounds.size.w, 60));
  text_layer_set_text(s_prayer_head_text_layer, hours_array[s_current_prayer_id].head);
  text_layer_set_font(s_prayer_head_text_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_prayer_head_text_layer, GTextAlignmentCenter);
  text_layer_set_overflow_mode(s_prayer_head_text_layer, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_prayer_head_text_layer, GColorBlack);
  text_layer_set_text_color(s_prayer_head_text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_prayer_head_text_layer));

  // Draw horizontal line
  s_prayer_canvas_layer = layer_create(bounds);
  // Assign the custom drawing procedure
  layer_set_update_proc(s_prayer_canvas_layer, prayer_canvas_update_proc);

  // Add to Window
  layer_add_child(window_get_root_layer(window), s_prayer_canvas_layer);

  // Set up scrolling window for body text
  GFont body_font = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  GRect shrinking_rect = GRect(0, 0, bounds.size.w, 2000);
  char *body_text = hours_array[s_current_prayer_id].body;
  GSize text_size = graphics_text_layout_get_content_size(body_text, body_font, shrinking_rect, GTextOverflowModeWordWrap, GTextAlignmentLeft);
  text_size.h += 2;
  GRect text_bounds = bounds;
  text_bounds.size.h = text_size.h;

  s_prayer_body_text_layer = text_layer_create(text_bounds);
  text_layer_set_text(s_prayer_body_text_layer, body_text);
  text_layer_set_font(s_prayer_body_text_layer, body_font);
  text_layer_set_text_alignment(s_prayer_body_text_layer, GTextAlignmentLeft);
  text_layer_set_overflow_mode(s_prayer_body_text_layer, GTextOverflowModeWordWrap);
  text_layer_set_background_color(s_prayer_body_text_layer, GColorBlack);
  text_layer_set_text_color(s_prayer_body_text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_prayer_body_text_layer));

  // Create the ScrollLayer
  s_scroll_layer = scroll_layer_create(GRect(0, 69, bounds.size.w, bounds.size.h - 69));

  // Set the scrolling content size
  scroll_layer_set_content_size(s_scroll_layer, text_size);

  // Let the ScrollLayer receive click events
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);

  // This bit is from https://github.com/Neal/pebble-hackernews/blob/ef929657a6220f204b019ef483a0e75eaad1b955/src/windows/storyview.c
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks) {
		.click_config_provider = prayer_click_config_provider,
	});

  // Add the TextLayer as a child of the ScrollLayer
  scroll_layer_add_child(s_scroll_layer, text_layer_get_layer(s_prayer_body_text_layer));

  // Add the ScrollLayer as a child of the Window
  layer_add_child(window_layer, scroll_layer_get_layer(s_scroll_layer));
}

static void prayer_window_unload(Window *window) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Unloading the prayer window");
  text_layer_destroy(s_prayer_title_text_layer);
  layer_destroy(s_prayer_canvas_layer);
  text_layer_destroy(s_prayer_head_text_layer);
  text_layer_destroy(s_prayer_body_text_layer);
  scroll_layer_destroy(s_scroll_layer);
  bitmap_layer_destroy(s_bitmap_layer);
}

static void reminder_cancel_click_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Pressed cancel.");
  vibes_cancel();

  // Schedule next prayer hour
  time_t next_prayer_timestamp;
  s_next_prayer_id = find_next_prayer(&next_prayer_timestamp);
  schedule_wakeup_time(&next_prayer_timestamp, s_next_prayer_id);

  window_stack_pop_all(true);
}

static void reminder_confirm_click_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Pressed confirm.");
  vibes_cancel();

  // Schedule next prayer hour
  time_t next_prayer_timestamp;
  s_next_prayer_id = find_next_prayer(&next_prayer_timestamp);
  schedule_wakeup_time(&next_prayer_timestamp, s_next_prayer_id);

  // Open prayer window
  window_stack_push(s_prayer_window, true);
}

static time_t get_timestamp_plus_snooze(int prayer_id, int snooze_minutes) {
  // Use current time plus snooze minutes instead of original prayer time
  time_t now = time(NULL);
  return now + (SECONDS_PER_MINUTE * snooze_minutes);
}

static void reminder_snooze_click_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Pressed snooze. - %lu", (int32_t)s_wakeup_timestamp);
  vibes_cancel();

  // Adjust wakeup time to + 2 minutes
  time_t snooze_time = get_timestamp_plus_snooze(s_current_prayer_id, 2);
  schedule_wakeup_time(&snooze_time, s_current_prayer_id);
  window_stack_pop_all(true);
}

static void reminder_snooze_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Pressed long snooze.");
  vibes_cancel();

  // Adjust wakeup time to + 10 minutes
  time_t snooze_time = get_timestamp_plus_snooze(s_current_prayer_id, 10);
  schedule_wakeup_time(&snooze_time, s_current_prayer_id);
  window_stack_pop_all(true);
}

static void reminder_click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, reminder_cancel_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, reminder_confirm_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, reminder_snooze_click_handler);
  window_long_click_subscribe(BUTTON_ID_DOWN, 400, reminder_snooze_long_click_handler, NULL);
}

static void reminder_window_load(Window *window) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Loading the reminder window");
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  char tmp[30];
  strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(&s_wakeup_timestamp));
  APP_LOG(APP_LOG_LEVEL_DEBUG, "s_wakeup_timestamp = %s, wakeup_id = %d", tmp, (int)s_wakeup_id);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Current prayer hour: %s", hours_array[s_current_prayer_id].title);
  
  window_set_background_color(window, GColorBlack);

  // Bitmap layer with cross image
  s_bitmap_layer = bitmap_layer_create(bounds);
  s_large_cross_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CROSS_PIC);
  bitmap_layer_set_bitmap(s_bitmap_layer, s_large_cross_bitmap);
  layer_add_child(window_layer, bitmap_layer_get_layer(s_bitmap_layer));

  // Action layer with cancel, confirm, snooze buttons
  s_cancel_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CANCEL);
  s_confirm_bitmap = gbitmap_create_with_resource(RESOURCE_ID_CONFIRM);
  s_snooze_bitmap = gbitmap_create_with_resource(RESOURCE_ID_SNOOZE);

  s_action_bar_layer = action_bar_layer_create();
  action_bar_layer_set_click_config_provider(s_action_bar_layer, reminder_click_config_provider);

  action_bar_layer_set_icon(s_action_bar_layer, BUTTON_ID_UP, s_cancel_bitmap);
  action_bar_layer_set_icon(s_action_bar_layer, BUTTON_ID_SELECT, s_confirm_bitmap);
  action_bar_layer_set_icon(s_action_bar_layer, BUTTON_ID_DOWN, s_snooze_bitmap);
  action_bar_layer_set_icon_animated(s_action_bar_layer, BUTTON_ID_UP, s_cancel_bitmap, true);
  action_bar_layer_set_icon_animated(s_action_bar_layer, BUTTON_ID_SELECT, s_confirm_bitmap, true);
  action_bar_layer_set_icon_animated(s_action_bar_layer, BUTTON_ID_DOWN, s_snooze_bitmap, true);

  action_bar_layer_add_to_window(s_action_bar_layer, window);


  // Text layer with prayer title
  s_reminder_title_text_layer = text_layer_create(GRect(0, 132, 115, 25));
  text_layer_set_text(s_reminder_title_text_layer, hours_array[s_current_prayer_id].title);
  text_layer_set_font(s_reminder_title_text_layer, fonts_get_system_font(FONT_KEY_ROBOTO_CONDENSED_21));
  text_layer_set_text_alignment(s_reminder_title_text_layer, GTextAlignmentCenter);
  text_layer_set_background_color(s_reminder_title_text_layer, GColorBlack);
  text_layer_set_text_color(s_reminder_title_text_layer, GColorWhite);
  layer_add_child(window_layer, text_layer_get_layer(s_reminder_title_text_layer));
}

static void reminder_window_unload(Window *window) {
  // Destroy the ActionBarLayer
  action_bar_layer_destroy(s_action_bar_layer);

  // Destroy the title text layer
  text_layer_destroy(s_reminder_title_text_layer);

  // Destroy the background
  gbitmap_destroy(s_large_cross_bitmap);

  // Destroy the icon GBitmaps
  gbitmap_destroy(s_cancel_bitmap);
  gbitmap_destroy(s_confirm_bitmap);
  gbitmap_destroy(s_snooze_bitmap);
}


static void wakeup_handler(WakeupId id, int32_t reason) {  // returns if the wakeup is still scheduled (e.g. snoozed)
  APP_LOG(APP_LOG_LEVEL_DEBUG, "wakeup handler");
  //Delete persistent storage value and reset wakeup ID
  persist_delete(PERSIST_WAKEUP);
  s_wakeup_id = -1;
  // Inject the reason into the "next prayer" variable
  s_current_prayer_id = reason;
  // Display the prayer window
  window_stack_push(s_reminder_window, true);
  // Start vibration pattern
  vibes_enqueue_custom_pattern(pat);
  // Note: the scheduling of the next prayer is done in init() further down
}

static void init(void) {
  s_preview_window = window_create();
  window_set_window_handlers(s_preview_window, (WindowHandlers){
    .load = preview_window_load,
    .unload = preview_window_unload,
  });

  s_prayer_window = window_create();
  window_set_window_handlers(s_prayer_window, (WindowHandlers){
    .load = prayer_window_load,
    .unload = prayer_window_unload,
  });

  s_reminder_window = window_create();
  window_set_window_handlers(s_reminder_window, (WindowHandlers){
    .load = reminder_window_load,
    .unload = reminder_window_unload,
  });

  // Check to see if we were launched by a wakeup event
  if (launch_reason() == APP_LAUNCH_WAKEUP) {
    // If woken by wakeup event, 
    int32_t reason = 0;
    if (wakeup_get_launch_event(&s_wakeup_id, &reason)) {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Launch reason: wakeup (id=%d, reason=%d)", (signed int)s_wakeup_id, (signed int)reason);
      wakeup_handler(s_wakeup_id, reason);
    }
  }

  if (launch_reason() != APP_LAUNCH_WAKEUP) {
    // If launched by the user and future prayer is already scheduled
    APP_LOG(APP_LOG_LEVEL_DEBUG, "Launch reason: user started the app");

    bool needs_reschedule = false; // reasons might be an invalid schedule or non at all
    // Check if we have already scheduled a wakeup event
    if (persist_exists(PERSIST_WAKEUP)) {
      s_wakeup_id = persist_read_int(PERSIST_WAKEUP);
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Persistent storage exists: wakeup id = %d", (signed int)s_wakeup_id);
      // query if event is still valid, otherwise delete
      if (wakeup_query(s_wakeup_id, &s_wakeup_timestamp)) {
        char tmp[30];
        strftime(tmp, sizeof(tmp), "%D %H:%M", localtime(&s_wakeup_timestamp));
        APP_LOG(APP_LOG_LEVEL_DEBUG, "wakeup was scheduled on startup, id = %d at %s", (signed int)s_wakeup_id, tmp);
      } else {
        persist_delete(PERSIST_WAKEUP);
        s_wakeup_id = -1;
        needs_reschedule = true;
      }
    } else {
      APP_LOG(APP_LOG_LEVEL_DEBUG, "wakeup hasn't been scheduled on startup");
      needs_reschedule = true;
    }

    if (needs_reschedule) {
      time_t next_prayer_timestamp;
      s_next_prayer_id = find_next_prayer(&next_prayer_timestamp);
      schedule_wakeup_time(&next_prayer_timestamp, s_next_prayer_id);
    }

    APP_LOG(APP_LOG_LEVEL_DEBUG, "Pushing the preview window to stack");
    window_stack_push(s_preview_window, true);
  }

  // Subscribe to wakeup service to get wakeup events while app is running
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Subscribing to wakeup service");
  wakeup_service_subscribe(wakeup_handler);
}

static void deinit(void) {
  window_destroy(s_menu_window);
}

int main(void) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Starting Init");
  init();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Init finished, starting event loop");
  app_event_loop();
  deinit();
}
