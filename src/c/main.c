// ZenNotes viewer: browse a ZenNotes vault by folder and read notes on the watch.
//
// The phone (src/pkjs/index.js) talks to the ZenNotes HTTP API and streams the
// result here as UTF-8 bytes split over several AppMessages:
//   KIND   1 = list, 2 = note, 3 = error (TEXT), 4 = display settings
//          (THEME, FONT, STATUS, SCROLLBAR, LANG, SCROLL)
//   LEVEL  list depth the payload is for (list only)
//   OFFSET byte offset of CHUNK in the whole payload
//   TOTAL  payload size in bytes
//   CHUNK  the bytes
// Upstream the watch sends CMD (1 = root list, 2 = open entry, 3 = search,
// 4 = refresh the list at LEVEL, 5 = note page ARG),
// LEVEL + ARG (which list and row) and TEXT (search query).
//
// List payload: first line is the header, then one line per entry: a type
// character (D folder, N note, R recent, S search, T tags, H one tag), the
// title, and optionally
// a tab and a short date shown at the right edge.
// Note payload: one line per block: a tag character, an indent digit, then
// the text. Tags: t title, m title meta (folder, date), 1-3 heading,
// p paragraph, l bullet, o numbered, G/g table header/body row (cells
// separated by tabs), C callout title and Q callout line (the digit is the
// callout colour, not an indent), k code line (with "\x01<class>" colour
// markers, see src/pkjs/highlight.js) and K code block footer
// ("language\tline count"), P/N link to the previous/next page of a long
// note (the digit is the target page),
// c open task, x done task, q quote, k code, r rule, b blank.
// Japanese renders through the system fonts' language-pack extension.

#include <pebble.h>

#define CMD_LIST 1
#define CMD_OPEN 2
#define CMD_SEARCH 3
#define CMD_REFRESH 4
#define CMD_PAGE 5
#define CMD_LINK 6
#define CMD_BACK 7
#define CMD_IMAGE 8

#define KIND_LIST 1
#define KIND_NOTE 2
#define KIND_ERROR 3
#define KIND_THEME 4
#define KIND_IMAGE 5

#define MAX_PAYLOAD 32768
#define MAX_ENTRIES 100
#define MAX_DEPTH 12
#define PERSIST_THEME 1
#define PERSIST_FONT 2
#define PERSIST_STATUS 3
#define PERSIST_SCROLLBAR 4
#define PERSIST_LANG 5
#define PERSIST_SCROLL 6
#define PERSIST_MARKS 7
#define PERSIST_CHAIN 8
#define PERSIST_RUNNING 9

#define MARGIN PBL_IF_ROUND_ELSE(30, 6)
#define ROW_H 36
#define ACTION_H 30  // search / recent rows

// Height of the Vim-style status line at the bottom (none on round screens,
// where the bottom edge is cut off).
#define STATUS_H PBL_IF_ROUND_ELSE(0, 16)

#if defined(PBL_TOUCH)
#define PBL_IF_TOUCH_ELSE_STR(touch, buttons) touch
#else
#define PBL_IF_TOUCH_ELSE_STR(touch, buttons) buttons
#endif

// ---- Strings ----
// Watch-side text in Japanese and English. Text from the phone (errors,
// list headers, "Search"/"Recent") is localised on the phone.

typedef enum {
  STR_LOADING,
  STR_SEARCHING,
  STR_EMPTY_FOLDER,
  STR_RELOAD_HINT,
  STR_TOO_BIG,
  STR_NO_MEMORY,
  STR_ERROR,
  STR_LINES,  // printf format with one %d
  STR_LINE_ONE,
  STR_BACK,
  STR_EXIT,
  STR_OPEN,
  STR_NEXT,
  STR_RELOAD,
  STR_MORE,
  STR_PREV,
  STR_NO_PHONE,
  STR_SEND_FAILED,
  STR_NO_RESPONSE,
  STR_JUMP,
  STR_NO_HEADINGS,
  STR_COUNT
} StrId;

static const char *const s_strings[2][STR_COUNT] = {
  {
    "読み込み中…", "検索中…", "このフォルダにはノートがありません",
    PBL_IF_TOUCH_ELSE_STR("タップして再読み込み", "SELECTで再読み込み"),
    "データが大きすぎます", "メモリが足りません", "エラー", "%d行", "%d行",
    // Status line hints: kept in English (see TR_NAV).
    "back", "exit", "open", "next", "reload", "more", "prev",
    "スマホと接続されていません", "スマホに送信できませんでした",
    "応答がありません。もう一度試してください",
    "jump", "見出しもリンクもありません",
  },
  {
    "Loading…", "Searching…", "No notes in this folder",
    PBL_IF_TOUCH_ELSE_STR("Tap to reload", "Press SELECT to reload"),
    "Too much data", "Out of memory", "Error", "%d lines", "%d line",
    "back", "exit", "open", "next", "reload", "more", "prev",
    "Phone not connected", "Could not reach the phone",
    "No response. Try again",
    "jump", "No headings or links",
  },
};
#define ICON_W 16
#define INDENT 10

#define MARQUEE_STEP 3
#define MARQUEE_TICK_MS 50
#define MARQUEE_PAUSE_MS 1200
#define MARQUEE_LOOPS 3

// ---- Theme ----
// Tuned for the 64-colour reflective e-paper panel, where colours look muted
// and greys wash out: all text is pure black or white, the selection is a full
// inversion, and colour is kept to bold headings and solid shapes (icons,
// bars, underlines) that stay recognisable even when the colour reads weakly.
// Colour carries meaning: one ink colour for all headings (level is shown by
// the rule under it), manila for folders, green only for done.

// Callout colour slots, set by the phone from the Obsidian callout type.
#define CALLOUT_COLORS 7

typedef struct {
  GColor bg, fg, hl_bg, hl_fg;
  GColor accent, folder, done, quote, code_bg, rule;
  // note, tip, success, warning/question, danger, example, quote
  GColor callout[CALLOUT_COLORS];
  // Syntax colours: keyword (also bold), string, number/constant, comment.
  GColor syn_kw, syn_str, syn_num, syn_com;
  GColor link;  // text of a block holding a wikilink
} Theme;

#define C(name) {.argb = GColor##name##ARGB8}

static const Theme s_themes[] = {
  {  // dark
    C(Black), C(White), C(White), C(Black),
    C(PictonBlue), C(ChromeYellow),
    C(ScreaminGreen), C(LightGray), C(DarkGray), C(LightGray),
    {C(PictonBlue), C(Cyan), C(ScreaminGreen), C(ChromeYellow), C(SunsetOrange),
     C(LavenderIndigo), C(LightGray)},
    C(PictonBlue), C(ScreaminGreen), C(ChromeYellow), C(RichBrilliantLavender),
    C(Magenta),  // saturated: pale colours wash out on e-paper; far from the heading blue
  },
  {  // light
    C(White), C(Black), C(Black), C(White),
    C(CobaltBlue), C(Orange),
    C(IslamicGreen), C(DarkGray), C(LightGray), C(DarkGray),
    {C(CobaltBlue), C(TiffanyBlue), C(IslamicGreen), C(Orange), C(Red),
     C(Purple), C(DarkGray)},
    C(CobaltBlue), C(DarkGreen), C(WindsorTan), C(Purple),
    C(DukeBlue),
  },
};

#undef C

static int s_theme_index;
static bool s_font_large = true;
static bool s_show_status = true;
static bool s_show_scrollbar = false;
static int s_lang;  // 0 = Japanese, 1 = English
// How far one UP/DOWN click moves in notes and code. Holding a button always
// speeds up from there.
typedef enum { SCROLL_SMALL, SCROLL_HALF, SCROLL_PAGE, SCROLL_MODES } ScrollMode;
static int s_scroll_mode = SCROLL_SMALL;
#define TR(id) (s_strings[s_lang][id])
// Status line hints are always English: Japanese glyphs come from the
// language pack with different metrics and never line up with the icons.
#define TR_NAV(id) (s_strings[1][id])

static int16_t prv_status_h(void) {
  return s_show_status ? STATUS_H : 0;
}

// ---- Loading state ----
// Shown in the status line of the top window: LOAD while waiting for the
// phone, then a fill that tracks the bytes received.
typedef enum { LOAD_IDLE, LOAD_WAIT, LOAD_RECV } LoadState;
static LoadState s_load;
static int s_load_pct;
static AppTimer *s_load_timer;
static void prv_status_refresh(void);
#define T (&s_themes[s_theme_index])

// ---- State ----

typedef struct {
  Window *window;
  MenuLayer *menu;
  int level;
  char *buf;               // owns header and titles
  char *header;
  char *titles[MAX_ENTRIES];
  char *dates[MAX_ENTRIES];  // NULL when the row has no date
  Layer *status_layer;
  char types[MAX_ENTRIES];
  int count;
  bool loaded;
  char status[160];
} ListView;

static ListView *s_views[MAX_DEPTH];

// Folder path the user was in, by folder titles, so the next launch can
// walk back into it. An empty title marks a list that isn't a folder
// (recent, search) and ends the walk.
#define RESTORE_DEPTH 4
static struct {
  uint8_t len;
  char titles[RESTORE_DEPTH][48];
} s_chain;
static int s_restore_len;
static int s_restore_level = -1;  // list level the walk is waiting for

static void prv_chain_save(void) {
  persist_write_data(PERSIST_CHAIN, &s_chain, sizeof(s_chain));
}

typedef struct {
  const char *text;  // for table rows: the first cell, cells separated by '\0'
  int16_t y;
  int16_t h;
  int16_t table;     // index into s_tables, -1 if not a table row
  int16_t code;      // index into s_codes, -1 if not part of a code block
  int8_t link;       // first wikilink in the text (index of its J block), -1 if none
  bool more_links;   // the text has other links after that one
  char tag;
  uint8_t indent;
  uint8_t cells;
} Block;

#define MAX_COLS 8
#define COL_MAX_W 120
#define CELL_PAD 3

typedef struct {
  int16_t first;  // first block of the table
  int16_t count;
  int16_t width;  // full grid width; wider than the screen means it scrolls
  int16_t offset; // horizontal scroll
  uint8_t cols;
  int16_t col_w[MAX_COLS];
} Table;

static Window *s_note_window;
static ScrollLayer *s_scroll_layer;
static Layer *s_note_layer;
static Layer *s_title_bar;  // note name pinned on top once the title scrolls away
static Layer *s_scroll_bar;  // position marker at the right edge
static Layer *s_note_status;
static char *s_note_buf;
static Block *s_blocks;
static int s_num_blocks;
static Table *s_tables;
static int s_num_tables;
static int s_link_depth;  // wikilinks followed since the note was opened from a list

// A fenced code block: its k lines followed by the K footer block.
#define CODE_PREVIEW_LINES 3
// Lowest y a note block may start at; see prv_note_layout().
#define LAYOUT_MAX_Y 31000

typedef struct {
  int16_t first;  // first k line
  int16_t count;  // number of k lines
  int16_t footer; // K block
} CodeBlock;

static CodeBlock *s_codes;
static int s_num_codes;
static int16_t s_space_w;  // width of a space in the code font
static int16_t s_code_lh;  // code line height

// Reassembly buffer for the payload currently arriving.
static char *s_rx_buf;
static uint32_t s_rx_total;
static uint32_t s_rx_received;
static int s_rx_kind;
static int s_rx_level;

// Marquee for the selected row when its title doesn't fit.
static ListView *s_mq_view;
static AppTimer *s_mq_timer;
static int16_t s_mq_offset;
static int16_t s_mq_max;
static bool s_mq_at_end;
static int s_mq_loops;

#if defined(PBL_MICROPHONE)
static DictationSession *s_dictation;
static int s_dictation_level;
#endif

static ListView *prv_push_list(int level, const char *status);
static void prv_note_message(const char *text);
static GFont prv_body_bold(void);
static int16_t prv_text_w(const char *text, GFont font);
static void prv_button_scroll(ClickRecognizerRef recognizer, ScrollLayer *scroll, int dir);
static void prv_momentum_stop(void);

// ---- Upstream ----

static void prv_load_end(void) {
  if (s_load_timer) {
    app_timer_cancel(s_load_timer);
    s_load_timer = NULL;
  }
  if (s_load != LOAD_IDLE) {
    s_load = LOAD_IDLE;
    prv_status_refresh();
  }
}

// Watchdog: if nothing arrives for this long after a request (or between
// chunks), ask once more, then give up with a message instead of leaving
// "Loading" on screen. The phone's own server timeout is 15 s.
#define LOAD_WATCHDOG_MS 20000
static int s_watchdog_retries;
static void prv_resend(void);
static void prv_show_error(const char *text);

static void prv_load_timeout(void *data) {
  s_load_timer = NULL;
  if (s_load == LOAD_IDLE) {
    return;
  }
  if (s_watchdog_retries == 0) {
    s_watchdog_retries++;
    s_load_timer = app_timer_register(LOAD_WATCHDOG_MS, prv_load_timeout, NULL);
    prv_resend();
    return;
  }
  prv_load_end();
  prv_show_error(TR(STR_NO_RESPONSE));
}

// Something arrived: push the watchdog back.
static void prv_load_alive(void) {
  if (s_load_timer) {
    app_timer_reschedule(s_load_timer, LOAD_WATCHDOG_MS);
  }
}

static void prv_load_begin(void) {
  s_load = LOAD_WAIT;
  s_load_pct = 0;
  if (s_load_timer) {
    app_timer_reschedule(s_load_timer, LOAD_WATCHDOG_MS);
  } else {
    s_load_timer = app_timer_register(LOAD_WATCHDOG_MS, prv_load_timeout, NULL);
  }
  prv_status_refresh();
}

// The last command sent to the phone, kept until the phone acknowledges it
// so it can be resent when the outbox is busy or the send fails.
static struct {
  int cmd;
  int level;
  int arg;
  bool has_text;
  char text[256];
} s_pending;
static bool s_pending_active;
static int s_pending_tries;
static AppTimer *s_retry_timer;

static bool prv_try_send(void) {
  DictionaryIterator *iter;
  if (app_message_outbox_begin(&iter) != APP_MSG_OK) {
    return false;
  }
  dict_write_int32(iter, MESSAGE_KEY_CMD, s_pending.cmd);
  dict_write_int32(iter, MESSAGE_KEY_LEVEL, s_pending.level);
  dict_write_int32(iter, MESSAGE_KEY_ARG, s_pending.arg);
  if (s_pending.has_text) {
    dict_write_cstring(iter, MESSAGE_KEY_TEXT, s_pending.text);
  }
  return app_message_outbox_send() == APP_MSG_OK;
}

static void prv_retry(void *data);

static void prv_schedule_retry(void) {
  if (++s_pending_tries > 5) {
    s_pending_active = false;
    prv_load_end();
    prv_show_error(TR(STR_SEND_FAILED));
    return;
  }
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
  }
  s_retry_timer = app_timer_register(200 * s_pending_tries, prv_retry, NULL);
}

static void prv_retry(void *data) {
  s_retry_timer = NULL;
  if (s_pending_active && !prv_try_send()) {
    prv_schedule_retry();
  }
}

static void prv_outbox_sent(DictionaryIterator *iter, void *ctx) {
  s_pending_active = false;
}

static void prv_outbox_failed(DictionaryIterator *iter, AppMessageResult reason, void *ctx) {
  if (s_pending_active) {
    prv_schedule_retry();
  }
}

// Send the last request again (watchdog), keeping the loading state.
static void prv_resend(void) {
  if (!connection_service_peek_pebble_app_connection()) {
    return;
  }
  s_pending_active = true;
  s_pending_tries = 0;
  if (!prv_try_send()) {
    prv_schedule_retry();
  }
}

static void prv_send(int cmd, int level, int arg, const char *text) {
  if (!connection_service_peek_pebble_app_connection()) {
    prv_show_error(TR(STR_NO_PHONE));
    return;
  }
  s_pending.cmd = cmd;
  s_pending.level = level;
  s_pending.arg = arg;
  s_pending.has_text = text != NULL;
  if (text) {
    strncpy(s_pending.text, text, sizeof(s_pending.text) - 1);
    s_pending.text[sizeof(s_pending.text) - 1] = '\0';
  }
  s_pending_active = true;
  s_pending_tries = 0;
  s_watchdog_retries = 0;
  prv_load_begin();
  if (!prv_try_send()) {
    prv_schedule_retry();
  }
}

// ---- List rows ----

static bool prv_is_action(char type) {
  return type == 'S' || type == 'R' || type == 'T';
}

static GFont prv_row_font(char type) {
  return fonts_get_system_font(prv_is_action(type) ? FONT_KEY_GOTHIC_18_BOLD
                                                   : FONT_KEY_GOTHIC_24_BOLD);
}

static GFont prv_date_font(void) {
  return fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
}

// Every row has an icon, so all titles start on the same column.
#define TEXT_X (MARGIN + ICON_W + 6)

// Width left for the title once the date (if any) is placed at the right.
static int16_t prv_title_avail(ListView *v, int row, int16_t cell_w) {
  int16_t avail = cell_w - TEXT_X - MARGIN;
  if (v->dates[row]) {
    GSize d = graphics_text_layout_get_content_size(
        v->dates[row], prv_date_font(), GRect(0, 0, 100, 20),
        GTextOverflowModeWordWrap, GTextAlignmentLeft);
    avail -= d.w + 6;
  }
  return avail;
}

// The black band that marks "where you are": list header and note title bar.
// With keep_tail, a long path is cut from the front ("…/PebbleOS") so the
// deepest folder stays visible.
static void prv_draw_band(GContext *ctx, GRect bounds, const char *text, bool keep_tail) {
  static char s_band_buf[128];
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  int16_t avail = bounds.size.w - 6;
  const char *shown = text;
  if (keep_tail) {
    const char *p = text;
    while (graphics_text_layout_get_content_size(shown, font, GRect(0, 0, 1000, 20),
                                                 GTextOverflowModeWordWrap,
                                                 GTextAlignmentLeft).w > avail) {
      const char *slash = strchr(p, '/');
      if (!slash) {
        break;
      }
      p = slash + 1;
      snprintf(s_band_buf, sizeof(s_band_buf), "…/%s", p);
      shown = s_band_buf;
    }
  }
  graphics_context_set_fill_color(ctx, T->hl_bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_text_color(ctx, T->hl_fg);
  graphics_draw_text(ctx, shown, font, GRect(bounds.origin.x + 3, bounds.origin.y - 2, avail,
                                             bounds.size.h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

// ---- Status line ----
// Vim-style: a mode block on the left, what BACK and SELECT do, and a ruler
// on the right. While loading, the mode reads LOAD and the bar fills with
// the share of the payload received.

static void prv_draw_back_icon(GContext *ctx, int16_t x, int16_t cy) {
  for (int i = 0; i < 4; i++) {
    graphics_draw_line(ctx, GPoint(x + i, cy - i), GPoint(x + i, cy + i));
  }
}

typedef enum { HINT_BACK, HINT_SELECT, HINT_HOLD } HintKind;

// One "icon + label" hint: ◀ BACK, ● SELECT, ◉ SELECT held.
static int16_t prv_status_hint(GContext *ctx, int16_t x, int16_t h, HintKind kind,
                               const char *label) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  // Centre on the x-height of Gothic 14 Bold as drawn at y = -2 (rows 5-11).
  int16_t cy = h / 2;
  graphics_context_set_stroke_color(ctx, T->fg);
  graphics_context_set_fill_color(ctx, T->fg);
  if (kind == HINT_SELECT) {
    graphics_fill_circle(ctx, GPoint(x + 3, cy), 3);
  } else if (kind == HINT_HOLD) {
    graphics_draw_circle(ctx, GPoint(x + 3, cy), 3);
    graphics_fill_circle(ctx, GPoint(x + 3, cy), 1);
  } else {
    prv_draw_back_icon(ctx, x, cy);
  }
  x += 9;
  graphics_context_set_text_color(ctx, T->fg);
  int16_t w = prv_text_w(label, font);
  graphics_draw_text(ctx, label, font, GRect(x, -2, w + 4, h), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);
  return x + w + 7;
}

static void prv_draw_status(GContext *ctx, GRect bounds, Window *window, const char *mode,
                            const char *back, const char *select, const char *hold,
                            const char *ruler) {
  GFont font = fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD);
  int16_t w = bounds.size.w;
  int16_t h = bounds.size.h;
  bool loading = s_load != LOAD_IDLE && window_stack_get_top_window() == window;
  char pct[12];

  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  if (loading) {
    mode = "LOAD";
    if (s_load == LOAD_RECV) {
      graphics_context_set_fill_color(ctx, T->code_bg);
      graphics_fill_rect(ctx, GRect(0, 1, w * s_load_pct / 100, h - 1), 0, GCornerNone);
      snprintf(pct, sizeof(pct), "%d%%", s_load_pct);
      ruler = pct;
    } else {
      ruler = "...";
    }
  }
  graphics_context_set_fill_color(ctx, T->fg);
  graphics_fill_rect(ctx, GRect(0, 0, w, 1), 0, GCornerNone);

  int16_t mode_w = prv_text_w(mode, font) + 8;
  graphics_context_set_fill_color(ctx, T->hl_bg);
  graphics_fill_rect(ctx, GRect(0, 1, mode_w, h - 1), 0, GCornerNone);
  graphics_context_set_text_color(ctx, T->hl_fg);
  graphics_draw_text(ctx, mode, font, GRect(4, -2, mode_w, h), GTextOverflowModeFill,
                     GTextAlignmentLeft, NULL);

  int16_t x = mode_w + 5;
  if (back) {
    x = prv_status_hint(ctx, x, h, HINT_BACK, back);
  }
  if (select) {
    x = prv_status_hint(ctx, x, h, HINT_SELECT, select);
  }
  if (hold) {
    prv_status_hint(ctx, x, h, HINT_HOLD, hold);
  }
  if (ruler) {
    graphics_context_set_text_color(ctx, T->fg);
    graphics_draw_text(ctx, ruler, font, GRect(w - 60, -2, 57, h), GTextOverflowModeFill,
                       GTextAlignmentRight, NULL);
  }
}

// Vim's ruler: Top, Bot, All or a percentage.
static void prv_scroll_ruler(char *buf, size_t size, int16_t top, int16_t content,
                             int16_t visible) {
  int16_t max = content - visible;
  if (max <= 0) {
    snprintf(buf, size, "All");
  } else if (top <= 0) {
    snprintf(buf, size, "Top");
  } else if (top >= max) {
    snprintf(buf, size, "Bot");
  } else {
    snprintf(buf, size, "%d%%", (int)((int32_t)top * 100 / max));
  }
}

// Icons are drawn rather than emoji so they don't depend on font coverage.
static void prv_draw_icon(GContext *ctx, char type, GPoint o, bool highlighted) {
  GColor line = highlighted ? T->hl_fg : T->fg;
  switch (type) {
    case 'D':
      graphics_context_set_fill_color(ctx, T->folder);
      graphics_fill_rect(ctx, GRect(o.x, o.y, 7, 4), 1, GCornersTop);
      graphics_fill_rect(ctx, GRect(o.x, o.y + 2, ICON_W, 12), 2, GCornersAll);
      break;
    case 'R': {
      GPoint c = GPoint(o.x + 7, o.y + 7);
      graphics_context_set_stroke_color(ctx, line);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_circle(ctx, c, 7);
      graphics_draw_line(ctx, c, GPoint(c.x, c.y - 4));
      graphics_draw_line(ctx, c, GPoint(c.x + 3, c.y));
      graphics_context_set_stroke_width(ctx, 1);
      break;
    }
    case 'N':
      graphics_context_set_stroke_color(ctx, line);
      graphics_draw_rect(ctx, GRect(o.x + 2, o.y, 12, 15));
      graphics_draw_line(ctx, GPoint(o.x + 5, o.y + 4), GPoint(o.x + 10, o.y + 4));
      graphics_draw_line(ctx, GPoint(o.x + 5, o.y + 7), GPoint(o.x + 10, o.y + 7));
      graphics_draw_line(ctx, GPoint(o.x + 5, o.y + 10), GPoint(o.x + 8, o.y + 10));
      break;
    case 'T':
    case 'H': {
      // "#": two slanted verticals crossed by two horizontals.
      GColor c = (type == 'H' && !highlighted) ? T->accent : line;
      graphics_context_set_stroke_color(ctx, c);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_line(ctx, GPoint(o.x + 5, o.y + 1), GPoint(o.x + 3, o.y + 14));
      graphics_draw_line(ctx, GPoint(o.x + 11, o.y + 1), GPoint(o.x + 9, o.y + 14));
      graphics_draw_line(ctx, GPoint(o.x + 1, o.y + 5), GPoint(o.x + 14, o.y + 5));
      graphics_draw_line(ctx, GPoint(o.x + 0, o.y + 10), GPoint(o.x + 13, o.y + 10));
      graphics_context_set_stroke_width(ctx, 1);
      break;
    }
    case 'S':
      graphics_context_set_stroke_color(ctx, line);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_circle(ctx, GPoint(o.x + 6, o.y + 6), 5);
      graphics_draw_line(ctx, GPoint(o.x + 10, o.y + 10), GPoint(o.x + 15, o.y + 15));
      graphics_context_set_stroke_width(ctx, 1);
      break;
  }
}

static void prv_mq_stop(void) {
  if (s_mq_timer) {
    app_timer_cancel(s_mq_timer);
    s_mq_timer = NULL;
  }
  s_mq_view = NULL;
  s_mq_offset = 0;
}

static void prv_mq_tick(void *data) {
  s_mq_timer = NULL;
  ListView *v = s_mq_view;
  if (!v || !v->menu) {
    return;
  }
  uint32_t next = MARQUEE_TICK_MS;
  if (s_mq_at_end) {
    s_mq_offset = 0;
    s_mq_at_end = false;
    next = MARQUEE_PAUSE_MS;
  } else {
    s_mq_offset += MARQUEE_STEP;
    if (s_mq_offset >= s_mq_max) {
      s_mq_offset = s_mq_max;
      s_mq_at_end = true;
      next = MARQUEE_PAUSE_MS;
      s_mq_loops++;
    }
  }
  layer_mark_dirty(menu_layer_get_layer(v->menu));
  // After a few passes, stop back at the start to save battery.
  if (s_mq_loops >= MARQUEE_LOOPS && !s_mq_at_end) {
    return;
  }
  s_mq_timer = app_timer_register(next, prv_mq_tick, NULL);
}

static void prv_mq_start(ListView *v, int row) {
  prv_mq_stop();
  if (!v->menu || row >= v->count) {
    return;
  }
  GRect bounds = layer_get_bounds(menu_layer_get_layer(v->menu));
  int16_t avail = prv_title_avail(v, row, bounds.size.w);
  GSize size = graphics_text_layout_get_content_size(
      v->titles[row], prv_row_font(v->types[row]), GRect(0, 0, 4000, ROW_H),
      GTextOverflowModeWordWrap, GTextAlignmentLeft);
  if (size.w <= avail) {
    return;
  }
  s_mq_view = v;
  s_mq_max = size.w - avail + 4;
  s_mq_at_end = false;
  s_mq_loops = 0;
  s_mq_timer = app_timer_register(MARQUEE_PAUSE_MS, prv_mq_tick, NULL);
}

// ---- List window ----
// While a list has no entries the menu shows one row with the status text.

static uint16_t prv_num_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  ListView *v = ctx;
  return v->count ? v->count : 1;
}

static int16_t prv_cell_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  ListView *v = ctx;
  if (!v->count) {
    return 70;
  }
  return (idx->row < v->count && prv_is_action(v->types[idx->row])) ? ACTION_H : ROW_H;
}

static int16_t prv_header_height(MenuLayer *menu, uint16_t section, void *ctx) {
#if defined(PBL_ROUND)
  return 0;
#else
  ListView *v = ctx;
  return (v->level > 0 && v->header) ? MENU_CELL_BASIC_HEADER_HEIGHT : 0;
#endif
}

static void prv_draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *data) {
  ListView *v = data;
  if (v->header) {
    prv_draw_band(ctx, layer_get_bounds(cell), v->header, true);
  }
}

static void prv_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  ListView *v = data;
  if (v->count == 0) {
    menu_cell_basic_draw(ctx, cell, v->status,
                         (v->loaded && v->level == 0)
                             ? TR(STR_RELOAD_HINT)
                             : NULL,
                         NULL);
    return;
  }
  if (idx->row >= v->count) {
    return;
  }
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  char type = v->types[idx->row];
  bool action = prv_is_action(type);
  int16_t avail = prv_title_avail(v, idx->row, bounds.size.w);
  bool scrolling = highlighted && s_mq_view == v;
  int16_t offset = scrolling ? s_mq_offset : 0;
  // A scrolling title runs to the cell edge instead of ending in an ellipsis.
  int16_t width = scrolling ? avail + s_mq_max + MARGIN : avail;
  GColor fg = highlighted ? T->hl_fg : T->fg;
  GColor bg = highlighted ? T->hl_bg : T->bg;

  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, v->titles[idx->row], prv_row_font(type),
                     GRect(TEXT_X - offset, action ? 1 : 2, width, bounds.size.h),
                     // No ellipsis while scrolling: the box runs past both edges.
                     scrolling ? GTextOverflowModeWordWrap : GTextOverflowModeTrailingEllipsis,
                     GTextAlignmentLeft, NULL);
  if (offset > 0) {
    // Cover the text that scrolled past the left edge or under the date.
    graphics_context_set_fill_color(ctx, bg);
    graphics_fill_rect(ctx, GRect(0, 0, TEXT_X, bounds.size.h), 0, GCornerNone);
    if (v->dates[idx->row]) {
      graphics_fill_rect(ctx, GRect(TEXT_X + avail, 0, bounds.size.w, bounds.size.h), 0,
                         GCornerNone);
    }
  }
  if (v->dates[idx->row]) {
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, v->dates[idx->row], prv_date_font(),
                       GRect(0, bounds.size.h / 2 - 10, bounds.size.w - MARGIN, 20),
                       GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }
  prv_draw_icon(ctx, type, GPoint(MARGIN, bounds.size.h / 2 - 7), highlighted);
  // A rule above the first vault row separates it from the action rows.
  if (!action && idx->row > 0 && prv_is_action(v->types[idx->row - 1])) {
    graphics_context_set_fill_color(ctx, T->fg);
    graphics_fill_rect(ctx, GRect(0, 0, bounds.size.w, 2), 0, GCornerNone);
  }
}

static void prv_selection_changed(MenuLayer *menu, MenuIndex new_idx, MenuIndex old_idx,
                                  void *ctx) {
  ListView *v = ctx;
  if (v->status_layer) {
    layer_mark_dirty(v->status_layer);
  }
  prv_mq_start(ctx, new_idx.row);
}

#if defined(PBL_MICROPHONE)
// A fresh dictation session per search: reusing one made every search after
// the first fail on the watch (the dictation UI showed an error). The result
// is handled after the callback returns, once the dictation UI has closed.
static char s_dictation_text[256];
static bool s_dictation_ok;

static void prv_dictation_finish(void *data) {
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
    s_dictation = NULL;
  }
  if (!s_dictation_ok) {
    return;
  }
  ListView *v = prv_push_list(s_dictation_level + 1, TR(STR_SEARCHING));
  if (v) {
    prv_send(CMD_SEARCH, s_dictation_level, 0, s_dictation_text);
  }
}

static void prv_dictation_done(DictationSession *session, DictationSessionStatus status,
                               char *transcription, void *ctx) {
  s_dictation_ok = status == DictationSessionStatusSuccess && transcription && transcription[0];
  if (s_dictation_ok) {
    strncpy(s_dictation_text, transcription, sizeof(s_dictation_text) - 1);
    s_dictation_text[sizeof(s_dictation_text) - 1] = '\0';
  }
  app_timer_register(0, prv_dictation_finish, NULL);
}
#endif

static void prv_start_search(ListView *v) {
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    return;  // one already running
  }
  s_dictation = dictation_session_create(sizeof(s_dictation_text), prv_dictation_done, NULL);
  if (!s_dictation) {
    vibes_short_pulse();
    return;
  }
  s_dictation_level = v->level;
  if (dictation_session_start(s_dictation) != DictationSessionStatusSuccess) {
    dictation_session_destroy(s_dictation);
    s_dictation = NULL;
    vibes_short_pulse();
  }
#else
  vibes_short_pulse();
#endif
}

static void prv_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  ListView *v = ctx;
  if (v->count == 0) {
    if (v->loaded && v->level == 0) {
      v->loaded = false;
      strncpy(v->status, TR(STR_LOADING), sizeof(v->status) - 1);
      menu_layer_reload_data(menu);
      prv_send(CMD_LIST, 0, 0, NULL);
    }
    return;
  }
  if (idx->row >= v->count) {
    return;
  }
  switch (v->types[idx->row]) {
    case 'N':
      s_link_depth = 0;
      window_stack_push(s_note_window, true);
      prv_note_message(TR(STR_LOADING));
      prv_send(CMD_OPEN, v->level, idx->row, NULL);
      break;
    case 'S':
      if (v->level < RESTORE_DEPTH) {
        s_chain.titles[v->level][0] = '\0';
      }
      prv_start_search(v);
      break;
    default:
      if (v->level < RESTORE_DEPTH) {
        if (v->types[idx->row] == 'D') {
          strncpy(s_chain.titles[v->level], v->titles[idx->row], sizeof(s_chain.titles[0]) - 1);
          s_chain.titles[v->level][sizeof(s_chain.titles[0]) - 1] = '\0';
        } else {
          s_chain.titles[v->level][0] = '\0';
        }
      }
      s_restore_level = -1;  // the user took over
      if (prv_push_list(v->level + 1, TR(STR_LOADING))) {
        prv_send(CMD_OPEN, v->level, idx->row, NULL);
      }
      break;
  }
}

// Long SELECT: fetch the note list again and redraw this list.
static void prv_select_long(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  ListView *v = ctx;
  vibes_short_pulse();
  prv_send(CMD_REFRESH, v->level, 0, NULL);
}

static void prv_apply_menu_colors(ListView *v) {
  if (v->menu) {
    menu_layer_set_normal_colors(v->menu, T->bg, T->fg);
    menu_layer_set_highlight_colors(v->menu, T->hl_bg, T->hl_fg);
  }
}

static void prv_list_status_draw(Layer *layer, GContext *ctx) {
  ListView *v = *(ListView **)layer_get_data(layer);
  const char *select = NULL;
  char ruler[16] = "";
  if (v->count > 0) {
    select = TR_NAV(STR_OPEN);
    snprintf(ruler, sizeof(ruler), "%d/%d",
             menu_layer_get_selected_index(v->menu).row + 1, v->count);
  } else if (v->loaded && v->level == 0) {
    select = TR_NAV(STR_RELOAD);
  }
  prv_draw_status(ctx, layer_get_bounds(layer), v->window, "LIST",
                  TR_NAV(v->level == 0 ? STR_EXIT : STR_BACK), select, NULL, ruler);
}

static void prv_list_frames(ListView *v) {
  GRect bounds = layer_get_bounds(window_get_root_layer(v->window));
  int16_t content_h = bounds.size.h - prv_status_h();
  layer_set_frame(menu_layer_get_layer(v->menu), GRect(0, 0, bounds.size.w, content_h));
  layer_set_frame(v->status_layer, GRect(0, content_h, bounds.size.w, STATUS_H));
  layer_set_hidden(v->status_layer, prv_status_h() == 0);
}

static void prv_list_load(Window *window) {
  ListView *v = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, T->bg);
  v->menu = menu_layer_create(layer_get_bounds(root));
  v->status_layer = layer_create_with_data(GRect(0, 0, 0, 0), sizeof(ListView *));
  *(ListView **)layer_get_data(v->status_layer) = v;
  layer_set_update_proc(v->status_layer, prv_list_status_draw);
  menu_layer_set_callbacks(v->menu, v, (MenuLayerCallbacks){
    .get_num_rows = prv_num_rows,
    .get_cell_height = prv_cell_height,
    .get_header_height = prv_header_height,
    .draw_header = prv_draw_header,
    .draw_row = prv_draw_row,
    .select_click = prv_select,
    .select_long_click = prv_select_long,
    .selection_changed = prv_selection_changed,
  });
  prv_apply_menu_colors(v);
  menu_layer_set_click_config_onto_window(v->menu, window);
  layer_add_child(root, menu_layer_get_layer(v->menu));
  layer_add_child(root, v->status_layer);
  prv_list_frames(v);
}

static void prv_list_appear(Window *window) {
  ListView *v = window_get_user_data(window);
  // Being at level L means the first L titles are the current folder path.
  int len = v->level < RESTORE_DEPTH ? v->level : RESTORE_DEPTH;
  if (s_chain.len != len) {
    s_chain.len = len;
    prv_chain_save();
  }
  prv_mq_start(v, menu_layer_get_selected_index(v->menu).row);
}

static void prv_list_disappear(Window *window) {
  if (s_mq_view == window_get_user_data(window)) {
    prv_mq_stop();
  }
}

static void prv_list_unload(Window *window) {
  ListView *v = window_get_user_data(window);
  if (s_mq_view == v) {
    prv_mq_stop();
  }
  menu_layer_destroy(v->menu);
  layer_destroy(v->status_layer);
  if (s_views[v->level] == v) {
    s_views[v->level] = NULL;
  }
  free(v->buf);
  free(v);
  window_destroy(window);
}

static ListView *prv_push_list(int level, const char *status) {
  if (level >= MAX_DEPTH) {
    vibes_short_pulse();
    return NULL;
  }
  ListView *v = calloc(1, sizeof(ListView));
  if (!v) {
    return NULL;
  }
  v->level = level;
  strncpy(v->status, status, sizeof(v->status) - 1);
  v->window = window_create();
  if (!v->window) {
    free(v);
    vibes_short_pulse();
    return NULL;
  }
  window_set_user_data(v->window, v);
  window_set_window_handlers(v->window, (WindowHandlers){
    .load = prv_list_load,
    .appear = prv_list_appear,
    .disappear = prv_list_disappear,
    .unload = prv_list_unload,
  });
  s_views[level] = v;
  window_stack_push(v->window, true);
  return v;
}

static void prv_apply_list(int level, char *buf) {
  ListView *v = (level >= 0 && level < MAX_DEPTH) ? s_views[level] : NULL;
  if (!v) {
    free(buf);
    return;
  }
  free(v->buf);
  v->buf = buf;
  v->count = 0;
  v->header = buf;
  char *nl = strchr(buf, '\n');
  char *p = NULL;
  if (nl) {
    *nl = '\0';
    p = nl + 1;
  }
  while (p && *p && v->count < MAX_ENTRIES) {
    nl = strchr(p, '\n');
    if (nl) {
      *nl = '\0';
    }
    if (p[0] && p[1]) {
      char *tab = strchr(p, '\t');
      if (tab) {
        *tab = '\0';
      }
      v->types[v->count] = p[0];
      v->titles[v->count] = p + 1;
      v->dates[v->count] = (tab && tab[1]) ? tab + 1 : NULL;
      v->count++;
    }
    p = nl ? nl + 1 : NULL;
  }
  v->loaded = true;
  strncpy(v->status, TR(STR_EMPTY_FOLDER), sizeof(v->status) - 1);
  if (v->menu) {
    menu_layer_reload_data(v->menu);
    // A refreshed list can be shorter than the old one.
    if (v->count > 0 && menu_layer_get_selected_index(v->menu).row >= v->count) {
      menu_layer_set_selected_index(v->menu, MenuIndex(0, 0), MenuRowAlignCenter, false);
    }
    layer_mark_dirty(v->status_layer);
    prv_mq_start(v, menu_layer_get_selected_index(v->menu).row);
  }
  // Walking back into the last folder after launch.
  if (s_restore_level == level) {
    s_restore_level = -1;
    if (level < s_restore_len && s_chain.titles[level][0]) {
      for (int r = 0; r < v->count; r++) {
        if (v->types[r] == 'D' && strcmp(v->titles[r], s_chain.titles[level]) == 0) {
          if (v->menu) {
            menu_layer_set_selected_index(v->menu, MenuIndex(0, r), MenuRowAlignCenter, false);
          }
          if (prv_push_list(level + 1, TR(STR_LOADING))) {
            s_restore_level = level + 1;
            prv_send(CMD_OPEN, level, r, NULL);
          }
          break;
        }
      }
    }
  }
}

// ---- Note window ----

static GFont prv_block_font(char tag) {
  switch (tag) {
    case 't':
    case '1':
    case '2':
      return fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD);
    case 'm':
      return fonts_get_system_font(s_font_large ? FONT_KEY_GOTHIC_18 : FONT_KEY_GOTHIC_14);
    case 'C':
      return prv_body_bold();
    case '3':
      return fonts_get_system_font(s_font_large ? FONT_KEY_GOTHIC_24_BOLD
                                                : FONT_KEY_GOTHIC_18_BOLD);
    default:
      return fonts_get_system_font(s_font_large ? FONT_KEY_GOTHIC_24 : FONT_KEY_GOTHIC_18);
  }
}

static GFont prv_body_bold(void) {
  return fonts_get_system_font(s_font_large ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD);
}

// Tables use one size smaller than the body so more columns fit.
static GFont prv_table_font(bool header) {
  if (s_font_large) {
    return fonts_get_system_font(header ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  }
  return fonts_get_system_font(header ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_14);
}

static GFont prv_code_font(bool bold) {
  if (s_font_large) {
    return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_18_BOLD : FONT_KEY_GOTHIC_18);
  }
  return fonts_get_system_font(bold ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_14);
}

static int16_t prv_text_w(const char *text, GFont font) {
  return graphics_text_layout_get_content_size(text, font, GRect(0, 0, 2000, 100),
                                               GTextOverflowModeWordWrap,
                                               GTextAlignmentLeft).w;
}

// Space width and line height for the current code font. There is no
// monospace system font, so indentation is laid out from the space width.
static void prv_code_metrics(void) {
  GFont font = prv_code_font(false);
  s_space_w = prv_text_w("a a", font) - prv_text_w("aa", font);
  if (s_space_w < 2) {
    s_space_w = 4;
  }
  s_code_lh = graphics_text_layout_get_content_size(
      "Ag", font, GRect(0, 0, 100, 100), GTextOverflowModeWordWrap, GTextAlignmentLeft).h + 3;
}

static GColor prv_syn_color(char cls) {
  switch (cls) {
    case 'k': return T->syn_kw;
    case 's': return T->syn_str;
    case 'n': return T->syn_num;
    case 'c': return T->syn_com;
    default: return T->fg;
  }
}

// Draws one marked-up code line from x and returns where it ends. With a NULL
// ctx it only measures. Words are measured one at a time so each colour run
// lands where the previous one ended.
static int16_t prv_code_runs(GContext *ctx, const char *s, int16_t x, int16_t y, int16_t max_x) {
  char seg[100];
  int n = 0;
  char cls = 'p';
  while (true) {
    char c = *s;
    bool boundary = c == '\0' || c == '\x01' || c == ' ' ||
                    (n >= 90 && ((uint8_t)c & 0xC0) != 0x80);
    if (boundary && n > 0) {
      seg[n] = '\0';
      n = 0;
      GFont font = prv_code_font(cls == 'k');
      int16_t w = prv_text_w(seg, font);
      if (ctx && x + w > 0) {
        graphics_context_set_text_color(ctx, prv_syn_color(cls));
        graphics_draw_text(ctx, seg, font, GRect(x, y, w + 8, s_code_lh),
                           GTextOverflowModeFill, GTextAlignmentLeft, NULL);
      }
      x += w;
      if (ctx && x >= max_x) {
        break;
      }
    }
    if (c == '\0') {
      break;
    }
    if (c == '\x01') {
      if (s[1]) {
        cls = s[1];
        s += 2;
      } else {
        s++;
      }
      continue;
    }
    if (c == ' ') {
      x += s_space_w;
    } else {
      seg[n++] = c;
    }
    s++;
  }
  return x;
}

static bool prv_is_page_link(char tag) {
  return tag == 'N' || tag == 'P';
}

static bool prv_is_code(char tag) {
  return tag == 'k' || tag == 'K';
}

static bool prv_is_table(char tag) {
  return tag == 'G' || tag == 'g';
}

static bool prv_is_callout(char tag) {
  return tag == 'C' || tag == 'Q';
}

// The digit of a callout line is its colour, not an indent.
static int16_t prv_block_indent(const Block *b) {
  return prv_is_callout(b->tag) ? 0 : b->indent;
}

static const char *prv_cell(const Block *b, int k) {
  const char *p = b->text;
  for (int i = 0; i < k; i++) {
    p += strlen(p) + 1;
  }
  return p;
}

// Space before the text for bullets, checkboxes and quote bars.
static int16_t prv_block_lead(char tag) {
  switch (tag) {
    case 'l': return s_font_large ? 12 : 10;
    case 'c':
    case 'x': return s_font_large ? 20 : 15;
    case 'q': return 9;
    case 'C':
    case 'Q': return 11;
    case 'k': return 3;
    default: return 0;
  }
}

// ---- Title bar ----
// Same size as the list header. Shown only after the title block has scrolled
// out of view, so the name never appears twice. Not used on round screens,
// where the top edge is cut off.

#if !defined(PBL_ROUND)
static void prv_title_bar_draw(Layer *layer, GContext *ctx) {
  if (s_num_blocks > 0 && s_blocks[0].tag == 't') {
    prv_draw_band(ctx, layer_get_bounds(layer), s_blocks[0].text, false);
  }
}
#endif

static void prv_title_bar_update(void) {
  if (!s_title_bar) {
    return;
  }
  bool show = false;
  if (s_num_blocks > 0 && s_blocks[0].tag == 't') {
    int16_t top = -scroll_layer_get_content_offset(s_scroll_layer).y;
    show = top > s_blocks[0].y + s_blocks[0].h - MENU_CELL_BASIC_HEADER_HEIGHT;
  }
  if (layer_get_hidden(s_title_bar) == show) {
    layer_set_hidden(s_title_bar, !show);
  }
}

#if !defined(PBL_ROUND)
// A thin thumb showing where in the note you are and how much is left.
static void prv_scroll_bar_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int16_t content = layer_get_frame(s_note_layer).size.h;
  int16_t visible = bounds.size.h;
  if (content <= visible) {
    return;
  }
  int16_t top = -scroll_layer_get_content_offset(s_scroll_layer).y;
  int16_t thumb = visible * visible / content;
  if (thumb < 12) {
    thumb = 12;
  }
  int16_t y = (int32_t)top * (visible - thumb) / (content - visible);
  graphics_context_set_fill_color(ctx, T->rule);
  graphics_fill_rect(ctx, GRect(0, y, bounds.size.w, thumb), 1, GCornersAll);
}
#endif

static void prv_note_scrolled(ScrollLayer *scroll_layer, void *ctx) {
  prv_title_bar_update();
  if (s_scroll_bar) {
    layer_mark_dirty(s_scroll_bar);
  }
  if (s_note_status) {
    layer_mark_dirty(s_note_status);
  }
}

// Column widths fit the widest cell (capped, longer cells wrap); row height
// fits the tallest wrapped cell.
static void prv_layout_table(Table *t) {
  t->cols = 0;
  for (int c = 0; c < MAX_COLS; c++) {
    t->col_w[c] = 24;
  }
  for (int r = 0; r < t->count; r++) {
    Block *b = &s_blocks[t->first + r];
    GFont font = prv_table_font(b->tag == 'G');
    for (int c = 0; c < b->cells; c++) {
      GSize size = graphics_text_layout_get_content_size(
          prv_cell(b, c), font, GRect(0, 0, 1000, 100), GTextOverflowModeWordWrap,
          GTextAlignmentLeft);
      int16_t w = size.w + CELL_PAD * 2 + 1;
      if (w > COL_MAX_W) {
        w = COL_MAX_W;
      }
      if (w > t->col_w[c]) {
        t->col_w[c] = w;
      }
    }
    if (b->cells > t->cols) {
      t->cols = b->cells;
    }
  }
  t->width = 1;
  for (int c = 0; c < t->cols; c++) {
    t->width += t->col_w[c];
  }
  for (int r = 0; r < t->count; r++) {
    Block *b = &s_blocks[t->first + r];
    GFont font = prv_table_font(b->tag == 'G');
    int16_t h = 0;
    for (int c = 0; c < b->cells; c++) {
      GSize size = graphics_text_layout_get_content_size(
          prv_cell(b, c), font, GRect(0, 0, t->col_w[c] - CELL_PAD * 2, 1000),
          GTextOverflowModeWordWrap, GTextAlignmentLeft);
      if (size.h > h) {
        h = size.h;
      }
    }
    b->h = h + 6;
  }
}

static void prv_note_layout(void) {
  if (!s_note_layer) {
    return;
  }
  GRect bounds = layer_get_bounds(window_get_root_layer(s_note_window));
  int16_t width = bounds.size.w - MARGIN * 2;
  int32_t y = PBL_IF_ROUND_ELSE(28, 2);
  prv_code_metrics();
  for (int t = 0; t < s_num_tables; t++) {
    prv_layout_table(&s_tables[t]);
    s_tables[t].offset = 0;
  }
  bool overflow = false;
  for (int i = 0; i < s_num_blocks; i++) {
    Block *b = &s_blocks[i];
    char prev = i > 0 ? s_blocks[i - 1].tag : 0;
    int16_t h;
    int16_t gap = 0;
    if (i > 0 && (b->tag == '1' || b->tag == '2' || b->tag == '3')) {
      gap += 6;
    }
    // A little air around tables and callouts.
    if (i > 0 && ((prv_is_table(b->tag) != prv_is_table(prev)) ||
                  (prv_is_code(b->tag) != prv_is_code(prev)) ||
                  prv_is_page_link(b->tag) || prv_is_page_link(prev) ||
                  b->tag == 'I' || prev == 'I' ||
                  (b->tag == 'C') || (prv_is_callout(prev) && !prv_is_callout(b->tag)))) {
      gap += 5;
    }
    if (b->tag == 'J') {
      gap = 0;
      h = 0;  // link list for the outline; not drawn
    } else if (prv_is_table(b->tag)) {
      h = b->h;  // set by prv_layout_table
    } else if (b->tag == 'k') {
      // Only the first few lines show in the note; the viewer has the rest.
      h = (b->code < 0 || i - s_codes[b->code].first < CODE_PREVIEW_LINES) ? s_code_lh : 0;
    } else if (b->tag == 'K') {
      h = s_code_lh + 4;
    } else if (prv_is_page_link(b->tag)) {
      h = graphics_text_layout_get_content_size(
              b->text, prv_body_bold(), GRect(0, 0, width - 20, 1000),
              GTextOverflowModeWordWrap, GTextAlignmentLeft).h + 10;
    } else if (b->tag == 'I') {
      h = graphics_text_layout_get_content_size(
              prv_cell(b, 1), prv_body_bold(), GRect(0, 0, width - 24, 1000),
              GTextOverflowModeWordWrap, GTextAlignmentLeft).h + 10;
    } else if (b->tag == 'b' || (!b->text[0] && b->tag != 'r')) {
      h = 6;
    } else if (b->tag == 'r') {
      h = 10;
    } else {
      int16_t x = prv_block_indent(b) * INDENT + prv_block_lead(b->tag);
      GSize size = graphics_text_layout_get_content_size(
          b->text, prv_block_font(b->tag), GRect(0, 0, width - x, 30000),
          GTextOverflowModeWordWrap, GTextAlignmentLeft);
      switch (b->tag) {
        case '1':
        case '2':
          h = size.h + 9;  // room for the rule under the heading
          break;
        case 'm':
          h = size.h + 10;  // rule closing the title block
          break;
        default:
          h = size.h + 3;
          break;
      }
    }
    // Block y/h are int16_t and the content layer can't be taller than
    // that either. Past the limit, hide the rest of the text (the phone keeps
    // pages small enough that this shouldn't happen) but still place the
    // next-page link where it can be reached.
    if (!overflow && y + gap + h > LAYOUT_MAX_Y && b->tag != 'N') {
      overflow = true;
    }
    if (overflow && b->tag != 'N' && b->tag != 'J') {
      gap = 0;
      h = 0;
    }
    y += gap;
    b->y = y;
    b->h = h;
    y += h;
  }
  y += PBL_IF_ROUND_ELSE(40, 10);
  if (y > 32000) {
    y = 32000;
  }
  layer_set_frame(s_note_layer, GRect(0, 0, bounds.size.w, y));
  scroll_layer_set_content_size(s_scroll_layer, GSize(bounds.size.w, y));
  scroll_layer_set_content_offset(s_scroll_layer, GPointZero, false);
  layer_mark_dirty(s_note_layer);
  prv_title_bar_update();
  if (s_title_bar) {
    layer_mark_dirty(s_title_bar);
  }
  if (s_scroll_bar) {
    layer_mark_dirty(s_scroll_bar);
  }
  if (s_note_status) {
    layer_mark_dirty(s_note_status);
  }
}

// Arrow in the side margin when a table continues past that edge.
static void prv_draw_more(GContext *ctx, int16_t x, int16_t cy, bool right) {
  graphics_context_set_stroke_color(ctx, T->fg);
  for (int i = 0; i < 4; i++) {
    int16_t lx = right ? x + i : x + 3 - i;
    graphics_draw_line(ctx, GPoint(lx, cy - 3 + i), GPoint(lx, cy + 3 - i));
  }
}

static void prv_draw_table_row(GContext *ctx, const Block *b, int16_t layer_w) {
  Table *t = &s_tables[b->table];
  bool header = b->tag == 'G';
  int16_t x = MARGIN - t->offset;
  if (header) {
    graphics_context_set_fill_color(ctx, T->code_bg);
    graphics_fill_rect(ctx, GRect(x, b->y, t->width, b->h), 0, GCornerNone);
  }
  graphics_context_set_text_color(ctx, T->fg);
  GFont font = prv_table_font(header);
  int16_t cx = x;
  for (int c = 0; c < t->cols; c++) {
    if (c < b->cells) {
      graphics_draw_text(ctx, prv_cell(b, c), font,
                         GRect(cx + CELL_PAD, b->y + 1, t->col_w[c] - CELL_PAD * 2, b->h),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
    }
    cx += t->col_w[c];
  }
  // Grid lines.
  graphics_context_set_fill_color(ctx, T->rule);
  graphics_fill_rect(ctx, GRect(x, b->y, t->width, 1), 0, GCornerNone);
  if (b == &s_blocks[t->first + t->count - 1]) {
    graphics_fill_rect(ctx, GRect(x, b->y + b->h - 1, t->width, 1), 0, GCornerNone);
  }
  cx = x;
  for (int c = 0; c <= t->cols; c++) {
    graphics_fill_rect(ctx, GRect(cx, b->y, 1, b->h), 0, GCornerNone);
    if (c < t->cols) {
      cx += t->col_w[c];
    }
  }
  // Hide what scrolled into the side margins, and mark hidden columns there.
  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_rect(ctx, GRect(0, b->y, MARGIN, b->h), 0, GCornerNone);
  graphics_fill_rect(ctx, GRect(layer_w - MARGIN, b->y, MARGIN, b->h), 0, GCornerNone);
  if (header) {
    int16_t cy = b->y + b->h / 2;
    if (t->offset > 0) {
      prv_draw_more(ctx, 1, cy, false);
    }
    if (t->offset < t->width - (layer_w - MARGIN * 2)) {
      prv_draw_more(ctx, layer_w - MARGIN + 1, cy, true);
    }
  }
}

static void prv_note_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int16_t width = bounds.size.w - MARGIN * 2;
  int16_t top = -scroll_layer_get_content_offset(s_scroll_layer).y;
  int16_t visible = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;

  for (int i = 0; i < s_num_blocks; i++) {
    Block *b = &s_blocks[i];
    if (b->h <= 0 || b->y + b->h < top) {
      continue;
    }
    if (b->y > top + visible) {
      break;
    }
    if (prv_is_table(b->tag)) {
      prv_draw_table_row(ctx, b, bounds.size.w);
      continue;
    }
    // Code preview: coloured text on the page background (colour on grey
    // reads poorly on e-paper) inside a thin frame; the grey footer below is
    // the part that looks pressable.
    if (b->tag == 'k') {
      if (b->h > 0) {
        prv_code_runs(ctx, b->text, MARGIN + 3, b->y, MARGIN + width);
        // Clip long lines at the frame.
        graphics_context_set_fill_color(ctx, T->bg);
        graphics_fill_rect(ctx, GRect(MARGIN + width + 1, b->y, MARGIN, b->h), 0, GCornerNone);
        graphics_context_set_fill_color(ctx, T->rule);
        graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, 1, b->h), 0, GCornerNone);
        graphics_fill_rect(ctx, GRect(MARGIN + width + 1, b->y, 1, b->h), 0, GCornerNone);
        if (b->code >= 0 && i == s_codes[b->code].first) {
          graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, width + 4, 1), 0, GCornerNone);
        }
      }
      continue;
    }
    if (prv_is_page_link(b->tag)) {
      // Button-like row linking to the next (N) or previous (P) page.
      graphics_context_set_fill_color(ctx, T->code_bg);
      graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, width + 4, b->h), 3, GCornersAll);
      graphics_context_set_stroke_color(ctx, T->fg);
      int16_t cy = b->y + b->h / 2;
      for (int k = 0; k < 4; k++) {
        int16_t ly = b->tag == 'N' ? cy - 2 + k : cy + 2 - k;
        graphics_draw_line(ctx, GPoint(MARGIN + 6 - (3 - k), ly), GPoint(MARGIN + 6 + (3 - k), ly));
      }
      graphics_context_set_text_color(ctx, T->fg);
      graphics_draw_text(ctx, b->text, prv_body_bold(),
                         GRect(MARGIN + 15, b->y + 1, width - 18, b->h),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      continue;
    }
    if (b->tag == 'I') {
      // Image: a band with a little picture icon, opened in the image viewer.
      graphics_context_set_fill_color(ctx, T->code_bg);
      graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, width + 4, b->h), 3, GCornersAll);
      int16_t cy = b->y + b->h / 2;
      graphics_context_set_stroke_color(ctx, T->fg);
      graphics_draw_rect(ctx, GRect(MARGIN + 1, cy - 6, 15, 12));
      graphics_context_set_fill_color(ctx, T->fg);
      graphics_fill_circle(ctx, GPoint(MARGIN + 11, cy - 2), 1);
      graphics_draw_line(ctx, GPoint(MARGIN + 3, cy + 3), GPoint(MARGIN + 6, cy - 1));
      graphics_draw_line(ctx, GPoint(MARGIN + 6, cy - 1), GPoint(MARGIN + 10, cy + 3));
      graphics_context_set_text_color(ctx, T->fg);
      graphics_draw_text(ctx, prv_cell(b, 1), prv_body_bold(),
                         GRect(MARGIN + 21, b->y + 1, width - 24, b->h),
                         GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
      continue;
    }
    if (b->tag == 'K') {
      // Footer: "▶ JavaScript  12行", the cue that the block opens.
      static char s_footer[64];
      const char *lang = b->text;
      const char *lines = b->cells > 1 ? prv_cell(b, 1) : "";
      char count[24];
      int n = atoi(lines);
      snprintf(count, sizeof(count), TR(n == 1 ? STR_LINE_ONE : STR_LINES), n);
      snprintf(s_footer, sizeof(s_footer), lang[0] ? "%s  %s" : "%s%s", lang, count);
      graphics_context_set_fill_color(ctx, T->code_bg);
      graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, width + 4, b->h - 2), 0, GCornerNone);
      prv_draw_more(ctx, MARGIN + 3, b->y + (b->h - 2) / 2, true);
      graphics_context_set_text_color(ctx, T->fg);
      graphics_draw_text(ctx, s_footer, prv_code_font(true),
                         GRect(MARGIN + 11, b->y - 1, width - 12, b->h),
                         GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
      continue;
    }
    int16_t x = MARGIN + prv_block_indent(b) * INDENT;
    int16_t lead = prv_block_lead(b->tag);
    GColor color = T->fg;
    // Vertical centre of the first text line, and checkbox size.
    int16_t mid = s_font_large ? 17 : 12;
    int16_t box = s_font_large ? 13 : 10;
    switch (b->tag) {
      case 'b':
        continue;
      case 'r':
        graphics_context_set_fill_color(ctx, T->rule);
        graphics_fill_rect(ctx, GRect(MARGIN, b->y + 4, width, 2), 0, GCornerNone);
        continue;
      case 'm':
        graphics_context_set_fill_color(ctx, T->rule);
        graphics_fill_rect(ctx, GRect(MARGIN, b->y + b->h - 4, width, 2), 0, GCornerNone);
        break;
      // Heading level is the length of the rule: full, short, none.
      case '1':
        color = T->accent;
        graphics_context_set_fill_color(ctx, T->accent);
        graphics_fill_rect(ctx, GRect(x, b->y + b->h - 5, width - (x - MARGIN), 3), 0,
                           GCornerNone);
        break;
      case '2':
        color = T->accent;
        graphics_context_set_fill_color(ctx, T->accent);
        graphics_fill_rect(ctx, GRect(x, b->y + b->h - 5, 28, 3), 0, GCornerNone);
        break;
      case '3':
        color = T->accent;
        break;
      case 'l':
        graphics_context_set_fill_color(ctx, T->fg);
        graphics_fill_circle(ctx, GPoint(x + 3, b->y + mid), s_font_large ? 3 : 2);
        break;
      case 'c':
        graphics_context_set_stroke_color(ctx, T->fg);
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_round_rect(ctx, GRect(x + 1, b->y + mid - box / 2, box, box), 2);
        graphics_context_set_stroke_width(ctx, 1);
        break;
      case 'x': {
        int16_t top = b->y + mid - box / 2;
        graphics_context_set_fill_color(ctx, T->done);
        graphics_fill_rect(ctx, GRect(x, top, box + 2, box + 2), 2, GCornersAll);
        graphics_context_set_stroke_color(ctx, T->bg);
        graphics_context_set_stroke_width(ctx, 2);
        graphics_draw_line(ctx, GPoint(x + 3, top + box / 2 + 1),
                           GPoint(x + box / 2, top + box - 2));
        graphics_draw_line(ctx, GPoint(x + box / 2, top + box - 2),
                           GPoint(x + box - 1, top + 3));
        graphics_context_set_stroke_width(ctx, 1);
        break;
      }
      case 'q':
        graphics_context_set_fill_color(ctx, T->quote);
        graphics_fill_rect(ctx, GRect(x, b->y, 4, b->h), 0, GCornerNone);
        break;
      // Callouts: a thick bar in the type's colour, continuous from the title
      // through the body lines.
      case 'C':
      case 'Q':
        graphics_context_set_fill_color(ctx, T->callout[b->indent % CALLOUT_COLORS]);
        graphics_fill_rect(ctx, GRect(x, b->y, 6, b->h), 0, GCornerNone);
        break;
      case 'k':
        graphics_context_set_fill_color(ctx, T->code_bg);
        graphics_fill_rect(ctx, GRect(MARGIN - 2, b->y, width + 4, b->h), 0, GCornerNone);
        break;
    }
    GRect text_box = GRect(x + lead, b->y, width - (x - MARGIN) - lead, b->h);
    if (b->link >= 0) {
      color = T->link;
    }
    graphics_context_set_text_color(ctx, color);
    graphics_draw_text(ctx, b->text, prv_block_font(b->tag), text_box,
                       GTextOverflowModeWordWrap, GTextAlignmentLeft, NULL);
  }
}

// Scroll positions of recently read notes (and pages), so reopening one
// returns to where you were. Keyed by a hash of title, meta line and page.
#define MARKS 10
typedef struct {
  uint32_t key;
  int16_t offset;
} Mark;
static Mark s_marks[MARKS];

static uint32_t prv_note_key(void) {
  if (s_num_blocks < 2 || s_blocks[0].tag != 't') {
    return 0;
  }
  uint32_t h = 5381;
  for (int i = 0; i < 2; i++) {
    for (const char *p = s_blocks[i].text; *p; p++) {
      h = h * 33 + (uint8_t)*p;
    }
  }
  for (int i = 0; i < s_num_blocks; i++) {
    if (s_blocks[i].tag == 'P') {
      h = h * 33 + s_blocks[i].indent + 1;  // page number
      break;
    }
  }
  return h ? h : 1;
}

static void prv_note_remember(void) {
  uint32_t key = prv_note_key();
  if (!key || !s_scroll_layer) {
    return;
  }
  int16_t offset = -scroll_layer_get_content_offset(s_scroll_layer).y;
  // Move this note to the front, dropping the oldest.
  int i = 0;
  while (i < MARKS - 1 && s_marks[i].key != key) {
    i++;
  }
  memmove(&s_marks[1], &s_marks[0], i * sizeof(Mark));
  s_marks[0].key = key;
  s_marks[0].offset = offset;
}

static void prv_note_restore(void) {
  uint32_t key = prv_note_key();
  for (int i = 0; key && i < MARKS; i++) {
    if (s_marks[i].key == key) {
      if (s_marks[i].offset > 0) {
        scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -s_marks[i].offset), false);
      }
      return;
    }
  }
}

static void prv_note_free(void) {
  prv_note_remember();
  prv_momentum_stop();
  free(s_blocks);
  s_blocks = NULL;
  s_num_blocks = 0;
  free(s_tables);
  s_tables = NULL;
  s_num_tables = 0;
  free(s_codes);
  s_codes = NULL;
  s_num_codes = 0;
  free(s_note_buf);
  s_note_buf = NULL;
}

// Takes ownership of buf (the note payload described at the top).
static void prv_note_set(char *buf) {
  if (!s_note_layer) {
    free(buf);
    return;
  }
  prv_note_free();
  s_note_buf = buf;
  int lines = 1;
  for (char *p = buf; *p; p++) {
    if (*p == '\n') {
      lines++;
    }
  }
  s_blocks = malloc(lines * sizeof(Block));
  if (!s_blocks) {
    prv_note_free();
    return;
  }
  char *line = buf;
  while (line) {
    char *nl = strchr(line, '\n');
    if (nl) {
      *nl = '\0';
    }
    if (line[0] && line[1]) {
      Block *b = &s_blocks[s_num_blocks++];
      b->tag = line[0];
      b->indent = (line[1] >= '0' && line[1] <= '9') ? line[1] - '0' : 0;
      b->text = line + 2;
      b->table = -1;
      b->code = -1;
      b->cells = 1;
      b->link = -1;
      b->more_links = false;
      // The phone ends a block that holds wikilinks with "\x02<index>[+]".
      char *mark = strchr(b->text, '\x02');
      if (mark) {
        *mark = '\0';
        b->link = atoi(mark + 1);
        b->more_links = strchr(mark + 1, '+') != NULL;
      }
      if (prv_is_table(b->tag) || b->tag == 'K' || b->tag == 'I') {
        for (char *p = line + 2; *p && b->cells < MAX_COLS; p++) {
          if (*p == '\t') {
            *p = '\0';
            b->cells++;
          }
        }
      }
    }
    line = nl ? nl + 1 : NULL;
  }
  // Group consecutive table rows into tables.
  int tables = 0;
  for (int i = 0; i < s_num_blocks; i++) {
    if (prv_is_table(s_blocks[i].tag) && (i == 0 || !prv_is_table(s_blocks[i - 1].tag))) {
      tables++;
    }
  }
  if (tables > 0) {
    s_tables = calloc(tables, sizeof(Table));
  }
  for (int i = 0; s_tables && i < s_num_blocks; i++) {
    if (!prv_is_table(s_blocks[i].tag)) {
      continue;
    }
    if (i == 0 || !prv_is_table(s_blocks[i - 1].tag)) {
      s_tables[s_num_tables].first = i;
      s_num_tables++;
    }
    s_blocks[i].table = s_num_tables - 1;
    s_tables[s_num_tables - 1].count++;
  }
  // Group code lines with the footer that ends their block.
  int codes = 0;
  for (int i = 0; i < s_num_blocks; i++) {
    codes += s_blocks[i].tag == 'K';
  }
  if (codes > 0) {
    s_codes = calloc(codes, sizeof(CodeBlock));
  }
  for (int i = 0, start = -1; s_codes && i < s_num_blocks; i++) {
    if (s_blocks[i].tag == 'k' && start < 0) {
      start = i;
    } else if (s_blocks[i].tag == 'K') {
      CodeBlock *c = &s_codes[s_num_codes];
      c->first = start < 0 ? i : start;
      c->count = start < 0 ? 0 : i - start;
      c->footer = i;
      for (int k = c->first; k <= i; k++) {
        s_blocks[k].code = s_num_codes;
      }
      s_num_codes++;
      start = -1;
    } else if (s_blocks[i].tag != 'k') {
      start = -1;
    }
  }
  if (!s_tables) {
    // Out of memory or no tables: draw table rows as nothing rather than crash.
    for (int i = 0; i < s_num_blocks; i++) {
      if (prv_is_table(s_blocks[i].tag)) {
        s_blocks[i].tag = 'b';
      }
    }
  }
  prv_note_layout();
  prv_note_restore();
}

static void prv_note_message(const char *text) {
  size_t len = strlen(text);
  char *buf = malloc(len + 3);
  if (!buf) {
    return;
  }
  buf[0] = 'p';
  buf[1] = '0';
  memcpy(buf + 2, text, len + 1);
  prv_note_set(buf);
}

// ---- Momentum ----
// Sideways flicks on tables and code keep sliding and slow down, so a fast
// flick travels further.
#define MOMENTUM_TICK_MS 40

static AppTimer *s_mom_timer;
static int32_t s_mom_v;    // px/s, in offset direction
static int32_t s_mom_pos;  // 1/16 px
static void (*s_mom_apply)(int16_t offset);
static int16_t (*s_mom_get)(void);

static void prv_momentum_stop(void) {
  if (s_mom_timer) {
    app_timer_cancel(s_mom_timer);
    s_mom_timer = NULL;
  }
  s_mom_apply = NULL;
}

static void prv_momentum_tick(void *data) {
  s_mom_timer = NULL;
  if (!s_mom_apply) {
    return;
  }
  s_mom_pos += s_mom_v * 16 * MOMENTUM_TICK_MS / 1000;
  int16_t target = s_mom_pos / 16;
  s_mom_apply(target);
  s_mom_v = s_mom_v * 88 / 100;
  // Stop at an edge (the offset got clamped) or once nearly still.
  if (s_mom_get() != target || abs(s_mom_v) < 40) {
    prv_momentum_stop();
    return;
  }
  s_mom_timer = app_timer_register(MOMENTUM_TICK_MS, prv_momentum_tick, NULL);
}

static void prv_momentum_start(int32_t v, void (*apply)(int16_t), int16_t (*get)(void)) {
  prv_momentum_stop();
  if (abs(v) < 150) {
    return;
  }
  if (v > 4000) {
    v = 4000;
  } else if (v < -4000) {
    v = -4000;
  }
  s_mom_v = v;
  s_mom_apply = apply;
  s_mom_get = get;
  s_mom_pos = get() * 16;
  s_mom_timer = app_timer_register(MOMENTUM_TICK_MS, prv_momentum_tick, NULL);
}

// ---- Code viewer ----
// Full-screen view of one code block: no wrapping, line numbers in a fixed
// gutter, scrolls vertically and sideways. Lines point into the note buffer,
// which stays alive because the note window is still on the stack below.

static Window *s_code_window;
static ScrollLayer *s_code_scroll;
static Layer *s_code_layer;
static Layer *s_code_band;
static Layer *s_code_status;
static const char **s_code_lines;
static int s_code_count;
static int16_t s_code_hoff;   // sideways scroll
static int16_t s_code_width;  // widest line, including the gutter
static int16_t s_code_gutter;
static char s_code_title[64];

static int16_t prv_code_view_w(void) {
  return layer_get_bounds(s_code_layer).size.w;
}

static void prv_code_hscroll_to(int16_t offset) {
  int16_t max = s_code_width - prv_code_view_w();
  if (offset > max) {
    offset = max;
  }
  if (offset < 0) {
    offset = 0;
  }
  if (offset != s_code_hoff) {
    s_code_hoff = offset;
    layer_mark_dirty(s_code_layer);
  }
}

static void prv_code_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  int16_t top = -scroll_layer_get_content_offset(s_code_scroll).y;
  int16_t visible = layer_get_bounds(scroll_layer_get_layer(s_code_scroll)).size.h;
  int first = (top - 4) / s_code_lh;
  if (first < 0) {
    first = 0;
  }
  for (int i = first; i < s_code_count; i++) {
    int16_t y = 4 + i * s_code_lh;
    if (y > top + visible) {
      break;
    }
    prv_code_runs(ctx, s_code_lines[i], s_code_gutter + 4 - s_code_hoff, y, bounds.size.w);
  }
  // Gutter: covers code scrolled under it, then the line numbers.
  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_rect(ctx, GRect(0, top, s_code_gutter, visible), 0, GCornerNone);
  graphics_context_set_fill_color(ctx, T->rule);
  graphics_fill_rect(ctx, GRect(s_code_gutter - 1, top, 1, visible), 0, GCornerNone);
  graphics_context_set_text_color(ctx, T->fg);
  GFont font = prv_code_font(false);
  char num[12];
  for (int i = first; i < s_code_count; i++) {
    int16_t y = 4 + i * s_code_lh;
    if (y > top + visible) {
      break;
    }
    snprintf(num, sizeof(num), "%d", i + 1);
    graphics_draw_text(ctx, num, font, GRect(0, y, s_code_gutter - 4, s_code_lh),
                       GTextOverflowModeFill, GTextAlignmentRight, NULL);
  }
  // Mark lines continuing past the right edge.
  if (s_code_hoff < s_code_width - bounds.size.w) {
    prv_draw_more(ctx, bounds.size.w - 5, top + visible / 2, true);
  }
}

static void prv_code_band_draw(Layer *layer, GContext *ctx) {
  prv_draw_band(ctx, layer_get_bounds(layer), s_code_title, false);
}

static void prv_code_scrolled(ScrollLayer *scroll_layer, void *ctx) {
  layer_mark_dirty(s_code_layer);
  if (s_code_status) {
    layer_mark_dirty(s_code_status);
  }
}

static void prv_code_status_draw(Layer *layer, GContext *ctx) {
  int16_t top = -scroll_layer_get_content_offset(s_code_scroll).y;
  int first = top / s_code_lh + 1;
  if (first > s_code_count) {
    first = s_code_count;
  }
  char ruler[16];
  snprintf(ruler, sizeof(ruler), "L%d/%d", first, s_code_count);
  prv_draw_status(ctx, layer_get_bounds(layer), s_code_window, "CODE", TR_NAV(STR_BACK),
                  s_code_width > prv_code_view_w() ? TR_NAV(STR_NEXT) : NULL, NULL, ruler);
}

static void prv_code_frames(void) {
  GRect bounds = layer_get_bounds(window_get_root_layer(s_code_window));
  int16_t band_h = PBL_IF_ROUND_ELSE(0, MENU_CELL_BASIC_HEADER_HEIGHT);
  int16_t content_h = bounds.size.h - prv_status_h();
  layer_set_frame(scroll_layer_get_layer(s_code_scroll),
                  GRect(0, band_h, bounds.size.w, content_h - band_h));
  layer_set_frame(s_code_status, GRect(0, content_h, bounds.size.w, STATUS_H));
  layer_set_hidden(s_code_status, prv_status_h() == 0);
}

static void prv_code_select(ClickRecognizerRef recognizer, void *ctx) {
  int16_t max = s_code_width - prv_code_view_w();
  int16_t page = (prv_code_view_w() - s_code_gutter) * 3 / 4;
  prv_code_hscroll_to(s_code_hoff >= max ? 0 : s_code_hoff + page);
}

static void prv_code_up(ClickRecognizerRef recognizer, void *ctx) {
  prv_button_scroll(recognizer, s_code_scroll, -1);
}

static void prv_code_down(ClickRecognizerRef recognizer, void *ctx) {
  prv_button_scroll(recognizer, s_code_scroll, 1);
}

static void prv_code_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_code_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_code_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_code_select);
}

#if defined(PBL_TOUCH)
static int16_t s_code_vbase;
static int16_t s_code_hbase;

static void prv_code_vpan(const Recognizer *recognizer, RecognizerEvent event) {
  switch (event) {
    case RecognizerEvent_Started:
      s_code_vbase = scroll_layer_get_content_offset(s_code_scroll).y;
      break;
    case RecognizerEvent_Updated: {
      GPoint d = pan_recognizer_get_delta_since_start(recognizer);
      scroll_layer_set_content_offset(s_code_scroll, GPoint(0, s_code_vbase + d.y), false);
      break;
    }
    case RecognizerEvent_Cancelled:
      scroll_layer_set_content_offset(s_code_scroll, GPoint(0, s_code_vbase), true);
      break;
    default:
      break;
  }
}

static void prv_pop_code(void *data) {
  if (window_stack_get_top_window() == s_code_window) {
    window_stack_pop(true);
  }
}

// Sideways drag scrolls long lines; when every line fits, a drag to the
// right goes back to the note.
static int16_t prv_code_hoff_get(void) {
  return s_code_hoff;
}

static void prv_code_hpan(const Recognizer *recognizer, RecognizerEvent event) {
  bool fits = s_code_width <= prv_code_view_w();
  switch (event) {
    case RecognizerEvent_Started:
      prv_momentum_stop();
      s_code_hbase = s_code_hoff;
      break;
    case RecognizerEvent_Updated:
      if (!fits) {
        prv_code_hscroll_to(s_code_hbase - pan_recognizer_get_delta_since_start(recognizer).x);
      }
      break;
    case RecognizerEvent_Completed:
      if (fits && pan_recognizer_get_total_delta(recognizer).x > 40) {
        app_timer_register(0, prv_pop_code, NULL);
      } else if (!fits) {
        // The finger moving left scrolls the offset right.
        prv_momentum_start(-pan_recognizer_get_velocity(recognizer).x, prv_code_hscroll_to,
                           prv_code_hoff_get);
      }
      break;
    case RecognizerEvent_Cancelled:
      prv_code_hscroll_to(s_code_hbase);
      break;
  }
}
#endif

static void prv_code_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, T->bg);
  int16_t band_h = PBL_IF_ROUND_ELSE(0, MENU_CELL_BASIC_HEADER_HEIGHT);

  prv_code_metrics();
  char num[12];
  snprintf(num, sizeof(num), "%d", s_code_count);
  s_code_gutter = prv_text_w(num, prv_code_font(false)) + 8 + PBL_IF_ROUND_ELSE(20, 0);
  s_code_width = 0;
  for (int i = 0; i < s_code_count; i++) {
    int16_t w = prv_code_runs(NULL, s_code_lines[i], 0, 0, 0);
    if (w > s_code_width) {
      s_code_width = w;
    }
  }
  s_code_width += s_code_gutter + 12;
  s_code_hoff = 0;

  s_code_scroll = scroll_layer_create(GRect(0, band_h, bounds.size.w, bounds.size.h - band_h));
  scroll_layer_set_shadow_hidden(s_code_scroll, true);
  int16_t content_h = 8 + s_code_count * s_code_lh + PBL_IF_ROUND_ELSE(40, 0);
  s_code_layer = layer_create(GRect(0, 0, bounds.size.w, content_h));
  layer_set_update_proc(s_code_layer, prv_code_draw);
  scroll_layer_add_child(s_code_scroll, s_code_layer);
  scroll_layer_set_content_size(s_code_scroll, GSize(bounds.size.w, content_h));
  scroll_layer_set_callbacks(s_code_scroll, (ScrollLayerCallbacks){
    .click_config_provider = prv_code_click_config,
    .content_offset_changed_handler = prv_code_scrolled,
  });
  scroll_layer_set_click_config_onto_window(s_code_scroll, window);
  layer_add_child(root, scroll_layer_get_layer(s_code_scroll));

  if (band_h > 0) {
    s_code_band = layer_create(GRect(0, 0, bounds.size.w, band_h));
    layer_set_update_proc(s_code_band, prv_code_band_draw);
    layer_add_child(root, s_code_band);
  }
  s_code_status = layer_create(GRect(0, 0, 0, 0));
  layer_set_update_proc(s_code_status, prv_code_status_draw);
  layer_add_child(root, s_code_status);
  prv_code_frames();

#if defined(PBL_TOUCH)
  window_set_touch_bridge_disabled(window, true);
  window_attach_recognizer(window, pan_recognizer_create(prv_code_vpan, NULL, PanAxis_Vertical));
  window_attach_recognizer(window, pan_recognizer_create(prv_code_hpan, NULL, PanAxis_Horizontal));
#endif
}

static void prv_code_unload(Window *window) {
  prv_momentum_stop();
  layer_destroy(s_code_status);
  s_code_status = NULL;
  if (s_code_band) {
    layer_destroy(s_code_band);
    s_code_band = NULL;
  }
  layer_destroy(s_code_layer);
  s_code_layer = NULL;
  scroll_layer_destroy(s_code_scroll);
  s_code_scroll = NULL;
  free(s_code_lines);
  s_code_lines = NULL;
  s_code_window = NULL;
  window_destroy(window);
}

static void prv_open_code(int index) {
  if (index < 0 || index >= s_num_codes || s_code_window) {
    return;
  }
  CodeBlock *c = &s_codes[index];
  if (c->count == 0) {
    return;
  }
  s_code_lines = malloc(c->count * sizeof(char *));
  if (!s_code_lines) {
    vibes_short_pulse();
    return;
  }
  for (int i = 0; i < c->count; i++) {
    s_code_lines[i] = s_blocks[c->first + i].text;
  }
  s_code_count = c->count;
  Block *footer = &s_blocks[c->footer];
  char count[24];
  snprintf(count, sizeof(count), TR(s_code_count == 1 ? STR_LINE_ONE : STR_LINES), s_code_count);
  snprintf(s_code_title, sizeof(s_code_title), footer->text[0] ? "%s  %s" : "%s%s",
           footer->text, count);
  s_code_window = window_create();
  if (!s_code_window) {
    free(s_code_lines);
    s_code_lines = NULL;
    vibes_short_pulse();
    return;
  }
  window_set_window_handlers(s_code_window, (WindowHandlers){
    .load = prv_code_load,
    .unload = prv_code_unload,
  });
  window_stack_push(s_code_window, true);
}

// ---- Image viewer ----
// The phone fits the picture to the screen and sends it one byte a pixel
// (GBitmapFormat8Bit); the rows are written straight into the bitmap as
// they arrive, so no second copy of the picture is held.

#define IMAGE_WATCHDOG_MS 65000

static Window *s_image_window;
static Layer *s_image_layer;
static GBitmap *s_image;
static uint32_t s_image_total;
static uint32_t s_image_received;
static int16_t s_image_w;
static char s_image_msg[96];

static void prv_image_draw(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, T->bg);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  if (s_image && s_image_received >= s_image_total) {
    GRect r = gbitmap_get_bounds(s_image);
    graphics_draw_bitmap_in_rect(
        ctx, s_image,
        GRect((b.size.w - r.size.w) / 2, (b.size.h - r.size.h) / 2, r.size.w, r.size.h));
    return;
  }
  char text[112];
  if (s_image && s_image_total) {
    snprintf(text, sizeof(text), "%s %d%%", TR(STR_LOADING),
             (int)(s_image_received * 100 / s_image_total));
  } else {
    snprintf(text, sizeof(text), "%s", s_image_msg[0] ? s_image_msg : TR(STR_LOADING));
  }
  graphics_context_set_text_color(ctx, T->fg);
  graphics_draw_text(ctx, text, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(MARGIN + 4, b.size.h / 2 - 30, b.size.w - (MARGIN + 4) * 2, 80),
                     GTextOverflowModeWordWrap, GTextAlignmentCenter, NULL);
}

static void prv_image_free(void) {
  if (s_image) {
    gbitmap_destroy(s_image);
    s_image = NULL;
  }
  s_image_total = 0;
  s_image_received = 0;
}

static void prv_image_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  window_set_background_color(window, T->bg);
  s_image_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_image_layer, prv_image_draw);
  layer_add_child(root, s_image_layer);
}

static void prv_image_unload(Window *window) {
  layer_destroy(s_image_layer);
  s_image_layer = NULL;
  prv_image_free();
  s_image_window = NULL;
  window_destroy(window);
}

#if defined(PBL_TOUCH)
static void prv_image_touch(const TouchEvent *event, void *ctx) {
  // Any tap closes the picture (BACK does too).
  if (event->type == TouchEvent_Liftoff && !event->non_navigational &&
      window_stack_get_top_window() == s_image_window) {
    window_stack_pop(true);
  }
}

static void prv_image_appear(Window *window) {
  touch_service_subscribe(prv_image_touch, NULL);
}

static void prv_image_disappear(Window *window) {
  touch_service_unsubscribe();
}
#endif

static void prv_open_image(int index) {
  if (s_image_window) {
    return;
  }
  s_image_window = window_create();
  if (!s_image_window) {
    vibes_short_pulse();
    return;
  }
  s_image_msg[0] = '\0';
  prv_image_free();
  window_set_window_handlers(s_image_window, (WindowHandlers){
    .load = prv_image_load,
#if defined(PBL_TOUCH)
    .appear = prv_image_appear,
    .disappear = prv_image_disappear,
#endif
    .unload = prv_image_unload,
  });
  window_stack_push(s_image_window, true);
  prv_send(CMD_IMAGE, 0, index, NULL);
  // A big picture can take the phone a while to download before the first
  // chunk: wait longer than for a note (the phone gives up after 60 s).
  if (s_load_timer) {
    app_timer_reschedule(s_load_timer, IMAGE_WATCHDOG_MS);
  }
}

static void prv_image_message(const char *text) {
  strncpy(s_image_msg, text, sizeof(s_image_msg) - 1);
  s_image_msg[sizeof(s_image_msg) - 1] = '\0';
  prv_image_free();
  if (s_image_layer) {
    layer_mark_dirty(s_image_layer);
  }
}

// One chunk of a picture: LEVEL is its width, TOTAL its size in bytes.
static void prv_image_chunk(DictionaryIterator *iter) {
  Tuple *offset_t = dict_find(iter, MESSAGE_KEY_OFFSET);
  Tuple *total_t = dict_find(iter, MESSAGE_KEY_TOTAL);
  Tuple *chunk_t = dict_find(iter, MESSAGE_KEY_CHUNK);
  Tuple *width_t = dict_find(iter, MESSAGE_KEY_LEVEL);
  if (!offset_t || !total_t || !chunk_t || !s_image_window) {
    return;
  }
  uint32_t offset = offset_t->value->uint32;
  uint32_t total = total_t->value->uint32;
  if (offset == 0) {
    int16_t w = width_t ? width_t->value->int32 : 0;
    prv_image_free();
    if (w <= 0 || total % w || total / w > 400 || w > 400) {
      prv_load_end();
      prv_image_message(TR(STR_TOO_BIG));
      return;
    }
    s_image = gbitmap_create_blank(GSize(w, total / w), GBitmapFormat8Bit);
    if (!s_image) {
      prv_load_end();
      prv_image_message(TR(STR_NO_MEMORY));
      return;
    }
    s_image_w = w;
    s_image_total = total;
    s_image_received = 0;
  }
  uint16_t len = chunk_t->length;
  if (!s_image || total != s_image_total || offset != s_image_received || offset + len > total) {
    return;
  }
  uint8_t *data = gbitmap_get_data(s_image);
  uint16_t stride = gbitmap_get_bytes_per_row(s_image);
  for (uint16_t i = 0; i < len; i++) {
    uint32_t p = offset + i;
    data[(p / s_image_w) * stride + p % s_image_w] = chunk_t->value->data[i];
  }
  s_image_received += len;
  if (s_image_received < s_image_total) {
    prv_load_alive();
  } else {
    prv_load_end();
  }
  if (s_image_layer) {
    layer_mark_dirty(s_image_layer);
  }
}

// ---- Sideways table scrolling ----
// Moves the table nearest the middle of the screen; touch drags it, SELECT
// pages through it.

// Distance from the middle of the screen to the content range y0..y1, or -1
// if the range is off screen.
static int32_t prv_view_distance(int16_t y0, int16_t y1) {
  int16_t top = -scroll_layer_get_content_offset(s_scroll_layer).y;
  int16_t visible = layer_get_bounds(scroll_layer_get_layer(s_scroll_layer)).size.h;
  if (y1 < top || y0 > top + visible) {
    return -1;
  }
  int16_t centre = top + visible / 2;
  return centre < y0 ? y0 - centre : (centre > y1 ? centre - y1 : 0);
}

// The wide table nearest the middle of the screen, with its distance.
static Table *prv_table_in_view(int32_t *dist) {
  if (!s_note_layer || s_num_tables == 0) {
    return NULL;
  }
  int16_t screen_w = layer_get_bounds(s_note_layer).size.w - MARGIN * 2;
  Table *best = NULL;
  int32_t best_d = 0;
  for (int i = 0; i < s_num_tables; i++) {
    Table *t = &s_tables[i];
    if (t->width <= screen_w) {
      continue;
    }
    Block *last = &s_blocks[t->first + t->count - 1];
    if (s_blocks[t->first].h <= 0) {
      continue;  // past the layout limit, not shown
    }
    int32_t d = prv_view_distance(s_blocks[t->first].y, last->y + last->h);
    if (d >= 0 && (!best || d < best_d)) {
      best = t;
      best_d = d;
    }
  }
  if (dist) {
    *dist = best_d;
  }
  return best;
}

// The code block nearest the middle of the screen, or -1.
static int prv_code_in_view(int32_t *dist) {
  int best = -1;
  int32_t best_d = 0;
  for (int i = 0; s_note_layer && i < s_num_codes; i++) {
    Block *footer = &s_blocks[s_codes[i].footer];
    if (footer->h <= 0) {
      continue;  // past the layout limit, not shown
    }
    int32_t d = prv_view_distance(s_blocks[s_codes[i].first].y, footer->y + footer->h);
    if (d >= 0 && (best < 0 || d < best_d)) {
      best = i;
      best_d = d;
    }
  }
  if (dist) {
    *dist = best_d;
  }
  return best;
}

static void prv_table_scroll_to(Table *t, int16_t offset) {
  int16_t max = t->width - (layer_get_bounds(s_note_layer).size.w - MARGIN * 2);
  if (offset > max) {
    offset = max;
  }
  if (offset < 0) {
    offset = 0;
  }
  if (offset != t->offset) {
    t->offset = offset;
    layer_mark_dirty(s_note_layer);
  }
}

// The page link nearest the middle of the screen, or -1.
// The page link or image band nearest the middle of the screen.
static int prv_band_in_view(int32_t *dist) {
  int best = -1;
  int32_t best_d = 0;
  for (int i = 0; s_note_layer && i < s_num_blocks; i++) {
    Block *b = &s_blocks[i];
    if ((!prv_is_page_link(b->tag) && b->tag != 'I') || b->h <= 0) {
      continue;
    }
    int32_t d = prv_view_distance(b->y, b->y + b->h);
    if (d >= 0 && (best < 0 || d < best_d)) {
      best = i;
      best_d = d;
    }
  }
  if (dist) {
    *dist = best_d;
  }
  return best;
}

static void prv_open_page(const Block *b) {
  int page = b->indent;
  // Drop the current page first: two long pages don't fit in memory at once.
  prv_note_message(TR(STR_LOADING));
  prv_send(CMD_PAGE, 0, page, NULL);
}

// Following wikilinks: the phone keeps the notes we came from, BACK returns
// to them one at a time before leaving the note view (s_link_depth deep).
static void prv_open_link(int index) {
  s_link_depth++;
  prv_note_message(TR(STR_LOADING));
  prv_send(CMD_LINK, 0, index, NULL);
}

static void prv_note_back(void) {
  if (s_link_depth > 0) {
    s_link_depth--;
    prv_note_message(TR(STR_LOADING));
    prv_send(CMD_BACK, 0, 0, NULL);
  } else if (window_stack_get_top_window() == s_note_window) {
    window_stack_pop(true);
  }
}

static void prv_note_back_click(ClickRecognizerRef recognizer, void *ctx) {
  prv_note_back();
}

// What SELECT would act on: the nearest of a code block, a page link or a
// wide table.
typedef enum { FOCUS_NONE, FOCUS_CODE, FOCUS_PAGE, FOCUS_TABLE } FocusKind;

static FocusKind prv_note_focus(int *code, int *page, Table **table) {
  int32_t table_d, code_d, page_d;
  *table = prv_table_in_view(&table_d);
  *code = prv_code_in_view(&code_d);
  *page = prv_band_in_view(&page_d);
  FocusKind kind = FOCUS_NONE;
  int32_t best = 0;
  if (*code >= 0) {
    kind = FOCUS_CODE;
    best = code_d;
  }
  if (*page >= 0 && (kind == FOCUS_NONE || page_d < best)) {
    kind = FOCUS_PAGE;
    best = page_d;
  }
  if (*table && (kind == FOCUS_NONE || table_d < best)) {
    kind = FOCUS_TABLE;
  }
  return kind;
}

// SELECT acts on whatever is nearest the middle of the screen: opens a code
// block, follows a page link, or pages a wide table sideways.
static void prv_note_select(ClickRecognizerRef recognizer, void *ctx) {
  int code;
  int page;
  Table *t;
  FocusKind kind = prv_note_focus(&code, &page, &t);
  if (kind == FOCUS_CODE) {
    prv_open_code(code);
    return;
  }
  if (kind == FOCUS_PAGE) {
    if (s_blocks[page].tag == 'I') {
      prv_open_image(atoi(s_blocks[page].text));
    } else {
      prv_open_page(&s_blocks[page]);
    }
    return;
  }
  if (kind != FOCUS_TABLE) {
    return;
  }
  int16_t step = (layer_get_bounds(s_note_layer).size.w - MARGIN * 2) * 3 / 4;
  int16_t max = t->width - (layer_get_bounds(s_note_layer).size.w - MARGIN * 2);
  prv_table_scroll_to(t, t->offset >= max ? 0 : t->offset + step);
}

// UP/DOWN: a click moves by the chosen step (a couple of lines, half a
// screen, or most of a screen with a line of overlap); every repeat while
// the button is held adds to the step, up to most of a screen.
static int s_scroll_repeat;

static void prv_button_scroll(ClickRecognizerRef recognizer, ScrollLayer *scroll, int dir) {
  int16_t visible = layer_get_frame(scroll_layer_get_layer(scroll)).size.h;
  int16_t page = visible - visible / 5;
  bool repeating = click_recognizer_is_repeating(recognizer);
  int16_t step;
  switch (s_scroll_mode) {
    case SCROLL_HALF: step = visible / 2; break;
    case SCROLL_PAGE: step = page; break;
    default: step = 30; break;
  }
  s_scroll_repeat = repeating ? s_scroll_repeat + 1 : 0;
  step += s_scroll_repeat * 12;
  if (step > page) {
    step = page;
  }
  GPoint offset = scroll_layer_get_content_offset(scroll);
  offset.y -= dir * step;
  // Animate single clicks; repeats follow each other too quickly for it.
  scroll_layer_set_content_offset(scroll, offset, !repeating);
}

static void prv_note_up(ClickRecognizerRef recognizer, void *ctx) {
  prv_button_scroll(recognizer, s_scroll_layer, -1);
}

static void prv_note_down(ClickRecognizerRef recognizer, void *ctx) {
  prv_button_scroll(recognizer, s_scroll_layer, 1);
}

// ---- Outline ----
// Long SELECT in a note lists its headings, then its wikilinks; choosing a
// heading scrolls there, choosing a link opens that note.

static Window *s_toc_window;
static MenuLayer *s_toc_menu;
static Layer *s_toc_status;
static int16_t *s_toc_blocks;  // block index of each heading, then of each link (J)
static int s_toc_count;
static int s_toc_headings;     // rows before the first link
static int s_toc_start = -1;   // row to open on; -1 for the heading in view

static uint16_t prv_toc_rows(MenuLayer *menu, uint16_t section, void *ctx) {
  return s_toc_count ? s_toc_count : 1;
}

static int16_t prv_toc_height(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  return 32;
}

static void prv_toc_draw_row(GContext *ctx, const Layer *cell, MenuIndex *idx, void *data) {
  if (!s_toc_count) {
    menu_cell_basic_draw(ctx, cell, TR(STR_NO_HEADINGS), NULL, NULL);
    return;
  }
  Block *b = &s_blocks[s_toc_blocks[idx->row]];
  GRect bounds = layer_get_bounds(cell);
  bool highlighted = menu_cell_layer_is_highlighted(cell);
  if (b->tag == 'J') {
    static char s_row[80];
    snprintf(s_row, sizeof(s_row), "\U0001F517%s", b->text);
    graphics_context_set_text_color(ctx, highlighted ? T->hl_fg : T->fg);
    graphics_draw_text(ctx, s_row, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                       GRect(MARGIN, 4, bounds.size.w - MARGIN * 2, 22),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
    return;
  }
  int16_t x = MARGIN + (b->tag - '1') * 12;
  graphics_context_set_text_color(ctx, highlighted ? T->hl_fg : T->fg);
  // One line: Japanese glyphs are taller than the line box, so word wrap
  // left the top of a second line showing; end the line with "…" instead.
  GFont font = fonts_get_system_font(b->tag == '1' ? FONT_KEY_GOTHIC_24_BOLD
                                                   : FONT_KEY_GOTHIC_18_BOLD);
  int16_t line_h = b->tag == '1' ? 30 : 22;
  graphics_draw_text(ctx, b->text, font,
                     GRect(x, b->tag == '1' ? 0 : 4, bounds.size.w - x - MARGIN, line_h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
}

static void prv_toc_select(MenuLayer *menu, MenuIndex *idx, void *ctx) {
  if (!s_toc_count) {
    window_stack_pop(true);
    return;
  }
  if (idx->row >= s_toc_headings) {
    int link = idx->row - s_toc_headings;
    window_stack_pop(true);
    prv_open_link(link);
    return;
  }
  int16_t y = s_blocks[s_toc_blocks[idx->row]].y - 2;
  window_stack_pop(true);
  scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, -y), false);
}

static void prv_toc_status_draw(Layer *layer, GContext *ctx) {
  char ruler[16] = "";
  if (s_toc_count) {
    snprintf(ruler, sizeof(ruler), "%d/%d", menu_layer_get_selected_index(s_toc_menu).row + 1,
             s_toc_count);
  }
  const char *select = NULL;
  if (s_toc_count) {
    bool link = menu_layer_get_selected_index(s_toc_menu).row >= s_toc_headings;
    select = TR_NAV(link ? STR_OPEN : STR_JUMP);
  }
  prv_draw_status(ctx, layer_get_bounds(layer), s_toc_window, "TOC", TR_NAV(STR_BACK), select,
                  NULL, ruler);
}

static void prv_toc_selection_changed(MenuLayer *menu, MenuIndex new_idx, MenuIndex old_idx,
                                      void *ctx) {
  // Also called while the outline is being set up, before the status line
  // exists; marking a NULL layer dirty faults inside the firmware.
  if (s_toc_status) {
    layer_mark_dirty(s_toc_status);
  }
}

static void prv_toc_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  int16_t content_h = bounds.size.h - prv_status_h();
  window_set_background_color(window, T->bg);
  s_toc_menu = menu_layer_create(GRect(0, 0, bounds.size.w, content_h));
  menu_layer_set_callbacks(s_toc_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = prv_toc_rows,
    .get_cell_height = prv_toc_height,
    .draw_row = prv_toc_draw_row,
    .select_click = prv_toc_select,
    .selection_changed = prv_toc_selection_changed,
  });
  menu_layer_set_normal_colors(s_toc_menu, T->bg, T->fg);
  menu_layer_set_highlight_colors(s_toc_menu, T->hl_bg, T->hl_fg);
  menu_layer_set_click_config_onto_window(s_toc_menu, window);
  layer_add_child(root, menu_layer_get_layer(s_toc_menu));

  s_toc_status = layer_create(GRect(0, content_h, bounds.size.w, STATUS_H));
  layer_set_update_proc(s_toc_status, prv_toc_status_draw);
  layer_set_hidden(s_toc_status, prv_status_h() == 0);
  layer_add_child(root, s_toc_status);

  // Start on the heading for the part of the note on screen.
  int16_t top = -scroll_layer_get_content_offset(s_scroll_layer).y;
  int current = 0;
  for (int i = 0; i < s_toc_headings; i++) {
    if (s_blocks[s_toc_blocks[i]].y <= top + 10) {
      current = i;
    }
  }
  if (s_toc_start >= 0 && s_toc_start < s_toc_count) {
    current = s_toc_start;
  }
  menu_layer_set_selected_index(s_toc_menu, MenuIndex(0, current), MenuRowAlignCenter, false);
}

static void prv_toc_unload(Window *window) {
  layer_destroy(s_toc_status);
  s_toc_status = NULL;
  menu_layer_destroy(s_toc_menu);
  s_toc_menu = NULL;
  free(s_toc_blocks);
  s_toc_blocks = NULL;
  s_toc_count = 0;
  s_toc_headings = 0;
  s_toc_window = NULL;
  window_destroy(window);
}

static bool prv_is_toc_heading(const Block *b) {
  return b->tag >= '1' && b->tag <= '3' && b->h > 0;
}

// start: row to select, or -1 for the heading of the part on screen.
static void prv_open_toc_at(int start) {
  if (s_toc_window || !s_note_layer) {
    return;
  }
  int count = 0;
  for (int i = 0; i < s_num_blocks; i++) {
    count += prv_is_toc_heading(&s_blocks[i]) || s_blocks[i].tag == 'J';
  }
  s_toc_blocks = count ? malloc(count * sizeof(int16_t)) : NULL;
  s_toc_count = 0;
  for (int i = 0; s_toc_blocks && i < s_num_blocks; i++) {
    if (prv_is_toc_heading(&s_blocks[i])) {
      s_toc_blocks[s_toc_count++] = i;
    }
  }
  s_toc_headings = s_toc_count;
  for (int i = 0; s_toc_blocks && i < s_num_blocks; i++) {
    if (s_blocks[i].tag == 'J') {
      s_toc_blocks[s_toc_count++] = i;
    }
  }
  s_toc_start = start;
  s_toc_window = window_create();
  if (!s_toc_window) {
    free(s_toc_blocks);
    s_toc_blocks = NULL;
    s_toc_count = 0;
    s_toc_headings = 0;
    vibes_short_pulse();
    return;
  }
  window_set_window_handlers(s_toc_window, (WindowHandlers){
    .load = prv_toc_load,
    .unload = prv_toc_unload,
  });
  window_stack_push(s_toc_window, true);
}

static void prv_open_toc(ClickRecognizerRef recognizer, void *ctx) {
  prv_open_toc_at(-1);
}

// A block with one wikilink opens it; with several, the outline opens on
// the first of them so the right one can be picked.
static void prv_follow_block_link(const Block *b) {
  if (!b->more_links) {
    prv_open_link(b->link);
    return;
  }
  int headings = 0;
  for (int i = 0; i < s_num_blocks; i++) {
    headings += prv_is_toc_heading(&s_blocks[i]);
  }
  prv_open_toc_at(headings + b->link);
}

static void prv_note_click_config(void *ctx) {
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, prv_open_toc, NULL);
  window_single_click_subscribe(BUTTON_ID_BACK, prv_note_back_click);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 100, prv_note_up);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 100, prv_note_down);
  window_single_click_subscribe(BUTTON_ID_SELECT, prv_note_select);
}

#if defined(PBL_TOUCH)
// A bare ScrollLayer doesn't follow the finger, so drive it with a vertical
// pan; a swipe to the right goes back to the list.
static int16_t s_pan_base;

static void prv_note_pan(const Recognizer *recognizer, RecognizerEvent event) {
  switch (event) {
    case RecognizerEvent_Started:
      s_pan_base = scroll_layer_get_content_offset(s_scroll_layer).y;
      break;
    case RecognizerEvent_Updated: {
      GPoint d = pan_recognizer_get_delta_since_start(recognizer);
      scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, s_pan_base + d.y), false);
      break;
    }
    case RecognizerEvent_Cancelled:
      scroll_layer_set_content_offset(s_scroll_layer, GPoint(0, s_pan_base), true);
      break;
    default:
      break;
  }
}

static void prv_pop_note(void *data) {
  if (window_stack_get_top_window() == s_note_window) {
    prv_note_back();
  }
}

// Tap: opens the code block under the finger. Detected from raw touch events
// (little movement, short press) rather than the tap recognizer, which on the
// watch rarely survived the natural jitter of a fingertip.
#define TAP_SLOP 10
#define TAP_MAX_MS 600

static GPoint s_touch_start;
static uint64_t s_touch_start_ms;
static bool s_touch_tracking;

static uint64_t prv_now_ms(void) {
  time_t sec;
  uint16_t ms;
  time_ms(&sec, &ms);
  return (uint64_t)sec * 1000 + ms;
}

static void prv_note_tap_at(GPoint p) {
  if (!s_note_layer) {
    return;
  }
  int16_t y = p.y - scroll_layer_get_content_offset(s_scroll_layer).y;
  for (int i = 0; i < s_num_blocks; i++) {
    Block *b = &s_blocks[i];
    if (b->h > 0 && y >= b->y && y < b->y + b->h) {
      if (b->code >= 0) {
        prv_open_code(b->code);
      } else if (prv_is_page_link(b->tag)) {
        prv_open_page(b);
      } else if (b->tag == 'I') {
        prv_open_image(atoi(b->text));
      } else if (b->link >= 0) {
        prv_follow_block_link(b);
      }
      return;
    }
  }
}

static void prv_note_touch(const TouchEvent *event, void *ctx) {
  switch (event->type) {
    case TouchEvent_Touchdown:
      s_touch_tracking = !event->non_navigational;
      s_touch_start = GPoint(event->x, event->y);
      s_touch_start_ms = prv_now_ms();
      break;
    case TouchEvent_PositionUpdate:
      if (abs(event->x - s_touch_start.x) > TAP_SLOP ||
          abs(event->y - s_touch_start.y) > TAP_SLOP) {
        s_touch_tracking = false;
      }
      break;
    case TouchEvent_Liftoff:
      if (s_touch_tracking && prv_now_ms() - s_touch_start_ms <= TAP_MAX_MS &&
          abs(event->x - s_touch_start.x) <= TAP_SLOP &&
          abs(event->y - s_touch_start.y) <= TAP_SLOP) {
        prv_note_tap_at(s_touch_start);
      }
      s_touch_tracking = false;
      break;
  }
}

static void prv_note_appear(Window *window) {
  touch_service_subscribe(prv_note_touch, NULL);
}

static void prv_note_disappear(Window *window) {
  touch_service_unsubscribe();
  s_touch_tracking = false;
}

// Sideways drag: scrolls a wide table in view; with no table in view, a drag
// to the right goes back to the list.
static Table *s_hpan_table;
static int16_t s_hpan_base;

static Table *s_mom_table;

static void prv_mom_table_apply(int16_t offset) {
  if (s_mom_table) {
    prv_table_scroll_to(s_mom_table, offset);
  }
}

static int16_t prv_mom_table_get(void) {
  return s_mom_table ? s_mom_table->offset : 0;
}

static void prv_note_hpan(const Recognizer *recognizer, RecognizerEvent event) {
  switch (event) {
    case RecognizerEvent_Started:
      prv_momentum_stop();
      s_hpan_table = prv_table_in_view(NULL);
      s_hpan_base = s_hpan_table ? s_hpan_table->offset : 0;
      break;
    case RecognizerEvent_Updated:
      if (s_hpan_table) {
        GPoint d = pan_recognizer_get_delta_since_start(recognizer);
        prv_table_scroll_to(s_hpan_table, s_hpan_base - d.x);
      }
      break;
    case RecognizerEvent_Completed:
      if (!s_hpan_table && pan_recognizer_get_total_delta(recognizer).x > 40) {
        // Pop outside the callback: unloading the window destroys this recognizer.
        app_timer_register(0, prv_pop_note, NULL);
      } else if (s_hpan_table) {
        s_mom_table = s_hpan_table;
        prv_momentum_start(-pan_recognizer_get_velocity(recognizer).x, prv_mom_table_apply,
                           prv_mom_table_get);
      }
      s_hpan_table = NULL;
      break;
    case RecognizerEvent_Cancelled:
      if (s_hpan_table) {
        prv_table_scroll_to(s_hpan_table, s_hpan_base);
      }
      s_hpan_table = NULL;
      break;
  }
}
#endif

static void prv_note_status_draw(Layer *layer, GContext *ctx) {
  const char *select = NULL;
  int code;
  int page;
  Table *t;
  switch (prv_note_focus(&code, &page, &t)) {
    case FOCUS_CODE:
      select = TR_NAV(STR_OPEN);
      break;
    case FOCUS_PAGE:
      select = TR_NAV(s_blocks[page].tag == 'I'   ? STR_OPEN
                      : s_blocks[page].tag == 'N' ? STR_MORE
                                                  : STR_PREV);
      break;
    case FOCUS_TABLE:
      select = TR_NAV(STR_NEXT);
      break;
    default:
      break;
  }
  char ruler[16] = "";
  if (s_note_layer) {
    prv_scroll_ruler(ruler, sizeof(ruler), -scroll_layer_get_content_offset(s_scroll_layer).y,
                     layer_get_frame(s_note_layer).size.h,
                     layer_get_frame(scroll_layer_get_layer(s_scroll_layer)).size.h);
  }
  // Long SELECT opens the outline when the page has headings.
  bool headings = false;
  for (int i = 0; i < s_num_blocks && !headings; i++) {
    headings = (s_blocks[i].tag >= '1' && s_blocks[i].tag <= '3') || s_blocks[i].tag == 'J';
  }
  prv_draw_status(ctx, layer_get_bounds(layer), s_note_window, "NOTE", TR_NAV(STR_BACK), select,
                  headings ? "toc" : NULL, ruler);
}

static void prv_note_frames(void) {
  GRect bounds = layer_get_bounds(window_get_root_layer(s_note_window));
  int16_t content_h = bounds.size.h - prv_status_h();
  layer_set_frame(scroll_layer_get_layer(s_scroll_layer), GRect(0, 0, bounds.size.w, content_h));
  layer_set_frame(s_note_status, GRect(0, content_h, bounds.size.w, STATUS_H));
  layer_set_hidden(s_note_status, prv_status_h() == 0);
  if (s_scroll_bar) {
    layer_set_frame(s_scroll_bar, GRect(bounds.size.w - 3, 0, 3, content_h));
    layer_set_hidden(s_scroll_bar, !s_show_scrollbar);
  }
}

static void prv_note_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  window_set_background_color(window, T->bg);

  s_scroll_layer = scroll_layer_create(bounds);
  scroll_layer_set_shadow_hidden(s_scroll_layer, true);

  s_note_layer = layer_create(bounds);
  layer_set_update_proc(s_note_layer, prv_note_draw);
  scroll_layer_add_child(s_scroll_layer, s_note_layer);
  layer_add_child(root, scroll_layer_get_layer(s_scroll_layer));
  scroll_layer_set_callbacks(s_scroll_layer, (ScrollLayerCallbacks){
    .click_config_provider = prv_note_click_config,
    .content_offset_changed_handler = prv_note_scrolled,
  });
  scroll_layer_set_click_config_onto_window(s_scroll_layer, window);

#if !defined(PBL_ROUND)
  s_scroll_bar = layer_create(GRect(bounds.size.w - 3, 0, 3, bounds.size.h));
  layer_set_update_proc(s_scroll_bar, prv_scroll_bar_draw);
  layer_add_child(root, s_scroll_bar);

  s_title_bar = layer_create(GRect(0, 0, bounds.size.w, MENU_CELL_BASIC_HEADER_HEIGHT));
  layer_set_update_proc(s_title_bar, prv_title_bar_draw);
  layer_set_hidden(s_title_bar, true);
  layer_add_child(root, s_title_bar);
#endif
  s_note_status = layer_create(GRect(0, 0, 0, 0));
  layer_set_update_proc(s_note_status, prv_note_status_draw);
  layer_add_child(root, s_note_status);
  prv_note_frames();

#if defined(PBL_TOUCH)
  window_set_touch_bridge_disabled(window, true);
  window_attach_recognizer(window, pan_recognizer_create(prv_note_pan, NULL, PanAxis_Vertical));
  window_attach_recognizer(window,
                           pan_recognizer_create(prv_note_hpan, NULL, PanAxis_Horizontal));
#endif
}

static void prv_note_unload(Window *window) {
  prv_note_remember();  // before the scroll layer goes away
  layer_destroy(s_note_status);
  s_note_status = NULL;
  if (s_title_bar) {
    layer_destroy(s_title_bar);
    s_title_bar = NULL;
  }
  if (s_scroll_bar) {
    layer_destroy(s_scroll_bar);
    s_scroll_bar = NULL;
  }
  layer_destroy(s_note_layer);
  s_note_layer = NULL;
  scroll_layer_destroy(s_scroll_layer);
  s_scroll_layer = NULL;
  prv_note_free();
}

static void prv_status_refresh(void) {
  Window *top = window_stack_get_top_window();
  if (!top) {
    return;
  }
  if (top == s_note_window) {
    if (s_note_status) {
      layer_mark_dirty(s_note_status);
    }
  } else if (top == s_code_window) {
    if (s_code_status) {
      layer_mark_dirty(s_code_status);
    }
  } else {
    ListView *v = window_get_user_data(top);
    if (v && v->status_layer) {
      layer_mark_dirty(v->status_layer);
    }
  }
}

// ---- Payload handling ----

static void prv_apply_display(int theme, bool font_large, bool status, bool scrollbar,
                              int lang, int scroll_mode) {
  if (scroll_mode >= 0 && scroll_mode < SCROLL_MODES) {
    s_scroll_mode = scroll_mode;
    persist_write_int(PERSIST_SCROLL, scroll_mode);
  }
  if (theme < 0 || theme >= (int)ARRAY_LENGTH(s_themes)) {
    theme = s_theme_index;
  }
  if (lang < 0 || lang > 1) {
    lang = s_lang;
  }
  if (theme == s_theme_index && font_large == s_font_large && status == s_show_status &&
      scrollbar == s_show_scrollbar && lang == s_lang) {
    return;
  }
  s_theme_index = theme;
  s_font_large = font_large;
  s_show_status = status;
  s_show_scrollbar = scrollbar;
  s_lang = lang;
  persist_write_int(PERSIST_THEME, theme);
  persist_write_bool(PERSIST_FONT, font_large);
  persist_write_bool(PERSIST_STATUS, status);
  persist_write_bool(PERSIST_SCROLLBAR, scrollbar);
  persist_write_int(PERSIST_LANG, lang);
  for (int i = 0; i < MAX_DEPTH; i++) {
    ListView *v = s_views[i];
    if (v) {
      window_set_background_color(v->window, T->bg);
      prv_apply_menu_colors(v);
      if (v->menu) {
        prv_list_frames(v);
        menu_layer_reload_data(v->menu);
      }
    }
  }
  window_set_background_color(s_note_window, T->bg);
  if (s_note_layer) {
    prv_note_frames();
  }
  prv_note_layout();
  if (s_code_window && s_code_layer) {
    prv_code_frames();
    layer_mark_dirty(s_code_layer);
  }
}

static void prv_show_error(const char *text) {
  Window *top = window_stack_get_top_window();
  if (top && top == s_image_window) {
    prv_image_message(text);
    return;
  }
  if (top == s_note_window) {
    prv_note_message(text);
    return;
  }
  ListView *v = top ? window_get_user_data(top) : NULL;
  if (!v) {
    return;
  }
  if (v->count == 0) {
    v->loaded = true;
    strncpy(v->status, text, sizeof(v->status) - 1);
    menu_layer_reload_data(v->menu);
  } else {
    vibes_short_pulse();
  }
}

static void prv_inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *kind_t = dict_find(iter, MESSAGE_KEY_KIND);
  if (!kind_t) {
    return;
  }
  int kind = kind_t->value->int32;

  if (kind == KIND_ERROR) {
    prv_load_end();
    Tuple *text_t = dict_find(iter, MESSAGE_KEY_TEXT);
    prv_show_error(text_t ? text_t->value->cstring : TR(STR_ERROR));
    return;
  }
  if (kind == KIND_THEME) {
    Tuple *theme_t = dict_find(iter, MESSAGE_KEY_THEME);
    Tuple *font_t = dict_find(iter, MESSAGE_KEY_FONT);
    Tuple *status_t = dict_find(iter, MESSAGE_KEY_STATUS);
    Tuple *scrollbar_t = dict_find(iter, MESSAGE_KEY_SCROLLBAR);
    Tuple *lang_t = dict_find(iter, MESSAGE_KEY_LANG);
    Tuple *scroll_t = dict_find(iter, MESSAGE_KEY_SCROLL);
    prv_apply_display(theme_t ? (int)theme_t->value->int32 : s_theme_index,
                      font_t ? font_t->value->int32 != 0 : s_font_large,
                      status_t ? status_t->value->int32 != 0 : s_show_status,
                      scrollbar_t ? scrollbar_t->value->int32 != 0 : s_show_scrollbar,
                      lang_t ? (int)lang_t->value->int32 : s_lang,
                      scroll_t ? (int)scroll_t->value->int32 : s_scroll_mode);
    return;
  }

  if (kind == KIND_IMAGE) {
    prv_image_chunk(iter);
    return;
  }

  Tuple *offset_t = dict_find(iter, MESSAGE_KEY_OFFSET);
  Tuple *total_t = dict_find(iter, MESSAGE_KEY_TOTAL);
  Tuple *chunk_t = dict_find(iter, MESSAGE_KEY_CHUNK);
  if (!offset_t || !total_t || !chunk_t) {
    return;
  }
  uint32_t offset = offset_t->value->uint32;
  uint32_t total = total_t->value->uint32;
  uint16_t len = chunk_t->length;

  if (offset == 0) {
    free(s_rx_buf);
    s_rx_buf = NULL;
    if (total == 0 || total > MAX_PAYLOAD) {
      prv_show_error(TR(STR_TOO_BIG));
      return;
    }
    s_rx_buf = malloc(total + 1);
    if (!s_rx_buf) {
      prv_show_error(TR(STR_NO_MEMORY));
      return;
    }
    Tuple *level_t = dict_find(iter, MESSAGE_KEY_LEVEL);
    s_rx_total = total;
    s_rx_received = 0;
    s_rx_kind = kind;
    s_rx_level = level_t ? level_t->value->int32 : 0;
  }

  if (!s_rx_buf || kind != s_rx_kind || total != s_rx_total ||
      offset != s_rx_received || offset + len > total) {
    return;
  }
  memcpy(s_rx_buf + offset, chunk_t->value->data, len);
  s_rx_received += len;
  prv_load_alive();
  if (s_rx_received < s_rx_total) {
    s_load = LOAD_RECV;
    s_load_pct = s_rx_received * 100 / s_rx_total;
    prv_status_refresh();
  } else {
    prv_load_end();
  }

  if (s_rx_received == s_rx_total) {
    char *buf = s_rx_buf;
    buf[s_rx_total] = '\0';
    s_rx_buf = NULL;
    if (kind == KIND_LIST) {
      prv_apply_list(s_rx_level, buf);
    } else if (kind == KIND_NOTE && window_stack_get_top_window() == s_note_window) {
      // Only while the note is in front: the outline and the code viewer
      // point into the current note, so replacing it under them would crash
      // (a watchdog resend can deliver the same note twice).
      prv_note_set(buf);
    } else {
      free(buf);
    }
  }
}

// ---- App ----

// Phone link lost: stop waiting and say so. Back again: load the root list
// if it never arrived.
static void prv_connection_changed(bool connected) {
  if (!connected) {
    s_pending_active = false;
    if (s_load != LOAD_IDLE) {
      prv_load_end();
      prv_show_error(TR(STR_NO_PHONE));
    }
    return;
  }
  ListView *root = s_views[0];
  if (root && root->count == 0) {
    strncpy(root->status, TR(STR_LOADING), sizeof(root->status) - 1);
    root->loaded = false;
    if (root->menu) {
      menu_layer_reload_data(root->menu);
    }
    prv_send(CMD_LIST, 0, 0, NULL);
  }
}

static void prv_init(void) {
  if (persist_exists(PERSIST_THEME)) {
    int index = persist_read_int(PERSIST_THEME);
    if (index >= 0 && index < (int)ARRAY_LENGTH(s_themes)) {
      s_theme_index = index;
    }
  }
  if (persist_exists(PERSIST_FONT)) {
    s_font_large = persist_read_bool(PERSIST_FONT);
  }
  if (persist_exists(PERSIST_STATUS)) {
    s_show_status = persist_read_bool(PERSIST_STATUS);
  }
  if (persist_exists(PERSIST_SCROLLBAR)) {
    s_show_scrollbar = persist_read_bool(PERSIST_SCROLLBAR);
  }
  if (persist_exists(PERSIST_MARKS)) {
    persist_read_data(PERSIST_MARKS, s_marks, sizeof(s_marks));
  }
  if (persist_exists(PERSIST_CHAIN)) {
    persist_read_data(PERSIST_CHAIN, &s_chain, sizeof(s_chain));
    if (s_chain.len > RESTORE_DEPTH) {
      s_chain.len = 0;
    }
    s_restore_len = s_chain.len;
    s_restore_level = s_restore_len > 0 ? 0 : -1;
  }
  // Still marked running: the last session crashed. Don't walk back into
  // the folder it crashed in, or a crash there would repeat on every launch.
  if (persist_exists(PERSIST_RUNNING)) {
    s_restore_level = -1;
  }
  persist_write_bool(PERSIST_RUNNING, true);
  if (persist_exists(PERSIST_SCROLL)) {
    int mode = persist_read_int(PERSIST_SCROLL);
    s_scroll_mode = (mode >= 0 && mode < SCROLL_MODES) ? mode : SCROLL_SMALL;
  }
  if (persist_exists(PERSIST_LANG)) {
    s_lang = persist_read_int(PERSIST_LANG) == 1 ? 1 : 0;
  }
#if defined(PBL_TOUCH)
  // MenuLayer scrolls by swipe and opens rows on tap; the note view handles
  // its own touches (see prv_note_load).
  app_touch_navigation_enable(true);
#endif

  app_message_register_inbox_received(prv_inbox_received);
  app_message_register_outbox_sent(prv_outbox_sent);
  app_message_register_outbox_failed(prv_outbox_failed);
  connection_service_subscribe((ConnectionHandlers){
    .pebble_app_connection_handler = prv_connection_changed,
  });
  app_message_open(app_message_inbox_size_maximum(), 512);

  s_note_window = window_create();
  window_set_window_handlers(s_note_window, (WindowHandlers){
    .load = prv_note_load,
#if defined(PBL_TOUCH)
    .appear = prv_note_appear,
    .disappear = prv_note_disappear,
#endif
    .unload = prv_note_unload,
  });

  prv_push_list(0, TR(STR_LOADING));
  if (connection_service_peek_pebble_app_connection()) {
    // The phone sends the root list by itself once it is ready; arm the
    // watchdog with that request so a lost list is asked for again.
    s_pending.cmd = CMD_LIST;
    s_pending.level = 0;
    s_pending.arg = 0;
    s_pending.has_text = false;
    s_watchdog_retries = 0;
    prv_load_begin();
  } else {
    prv_show_error(TR(STR_NO_PHONE));
  }
}

static void prv_deinit(void) {
  prv_mq_stop();
  if (s_load_timer) {
    app_timer_cancel(s_load_timer);
  }
  if (s_retry_timer) {
    app_timer_cancel(s_retry_timer);
  }
  prv_momentum_stop();
  connection_service_unsubscribe();
#if defined(PBL_MICROPHONE)
  if (s_dictation) {
    dictation_session_destroy(s_dictation);
  }
#endif
  window_destroy(s_note_window);  // unloading it records the open note's position
  persist_write_data(PERSIST_MARKS, s_marks, sizeof(s_marks));
  persist_delete(PERSIST_RUNNING);  // clean exit
  free(s_rx_buf);
}

int main(void) {
  prv_init();
  app_event_loop();
  prv_deinit();
}
