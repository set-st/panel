#pragma once
// Runtime-configured pages and cards, modelled on jtenniswood/espcontrol:
//  * the whole UI (pages, bottom menu, cards) lives in text entities on the
//    device and is edited from the device's web page (configurator.js);
//  * the firmware subscribes to Home Assistant entity states itself through the
//    native API and fires Home Assistant actions directly — no extra Home
//    Assistant integration is needed.
//
// "Panel Pages" text entity: pages separated by ';', each "title|icon|nav"
//   (nav = 1 shows the page in the bottom menu). Page 0 is the start page.
// "Panel Card N" text entity: "type;entity;label;icon;page;size;period"
//     type   - toggle | sensor | action | subpage | alarm | chart
//     entity - HA entity_id (for subpage: target page index; for alarm: "alarm",
//              the source is the "Panel Alarm Sensor" setting)
//     label  - caption shown on the card
//     icon   - MDI icon name from kIcons in panel_icons.h, generated from
//              icons.txt (empty = default for the type)
//     page   - page index the card lives on
//     size   - 1 = 1x1, 2 = 2x1, 3 = 2x2, 4 = full row
//     period - chart only: hours shown, 1 | 6 | 24 (history is kept in RAM
//              from the moment the card exists; it is lost on reboot)
// "Panel Header Sensor" text entity: entity_id shown in the status bar.
// "Panel Alarm Sensor" text entity: the Ukraine Alarm "Air" binary sensor of
//   the region (binary_sensor.<region>_air). Its siblings (_air_red,
//   _air_yellow, _artillery, ...) are derived from it. While an alarm is active
//   the page background turns red (yellow for drones only) and alarm cards
//   show which threats are active.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "esphome/components/api/api_server.h"
#include "panel_icons.h"

namespace panel_cards {

constexpr int SLOTS = 24;
constexpr int MAX_PAGES = 8;
constexpr int GAP = 10;

struct Card {
  std::string type;
  std::string entity;
  std::string label;
  std::string icon;
  int page = 0;
  int size = 1;
  int period = 24;  // chart: hours
  bool valid() const { return !type.empty() && !entity.empty(); }
};

struct Page {
  std::string title;
  std::string icon;
  bool nav = true;
};

struct Widgets {
  lv_obj_t *card = nullptr;
  lv_obj_t *icon = nullptr;
  lv_obj_t *label = nullptr;
  lv_obj_t *value = nullptr;
  lv_obj_t *chart = nullptr;  // created on demand for chart cards
  lv_obj_t *stats = nullptr;
  lv_chart_series_t *series = nullptr;
};

// Chart history: CHART_POINTS buckets over the period, each the average of the
// samples received in it (or the last known value if the sensor was quiet).
constexpr int CHART_POINTS = 120;

struct History {
  std::string entity;
  int period = 0;
  float buckets[CHART_POINTS];
  int filled = 0;           // completed buckets, newest at the end
  float sum = 0;
  int count = 0;
  float last = NAN;
  uint32_t bucket_start = 0;
  uint32_t bucket_ms() const { return (uint32_t) period * 3600000u / CHART_POINTS; }
  void reset(const std::string &e, int p) {
    entity = e;
    period = p;
    filled = 0;
    sum = 0;
    count = 0;
    last = NAN;
    bucket_start = esphome::millis();
  }
  void add(float v) {
    sum += v;
    count++;
    last = v;
  }
  // Closes finished buckets; returns true if the series moved.
  bool advance(uint32_t now) {
    bool moved = false;
    while (period > 0 && now - bucket_start >= bucket_ms()) {
      float v = count > 0 ? sum / count : last;
      if (filled == CHART_POINTS) {
        std::move(buckets + 1, buckets + CHART_POINTS, buckets);
        filled--;
      }
      buckets[filled++] = v;
      sum = 0;
      count = 0;
      bucket_start += bucket_ms();
      moved = true;
    }
    return moved;
  }
  // Completed buckets plus the current partial one.
  int points(float *out) const {
    int n = 0;
    int skip = filled == CHART_POINTS ? 1 : 0;
    for (int i = skip; i < filled; i++) out[n++] = buckets[i];
    out[n++] = count > 0 ? sum / count : last;
    return n;
  }
};

inline Card g_cards[SLOTS];
inline Widgets g_widgets[SLOTS];
inline History g_history[SLOTS];
inline const lv_font_t *g_small_font = nullptr;
inline std::vector<Page> g_pages = {{"Головна", "home", true}};
inline std::map<std::string, std::string> g_states;  // key: entity or entity|attribute
inline std::set<std::string> g_subscribed;
inline std::vector<int> g_nav_stack;  // previously visited pages, for "back"
inline int g_page = 0;
inline std::string g_header_entity;
inline uint32_t g_clicks_blocked_until = 0;

inline lv_obj_t *g_title = nullptr;
inline lv_obj_t *g_back = nullptr;
inline lv_obj_t *g_empty_hint = nullptr;
inline lv_obj_t *g_grid = nullptr;
inline lv_obj_t *g_nav = nullptr;
inline lv_obj_t *g_header_label = nullptr;
inline const lv_font_t *g_icon_font = nullptr;
inline const lv_font_t *g_text_font = nullptr;
inline lv_obj_t *g_page_bg = nullptr;  // page background recoloured during an alarm
inline std::vector<lv_obj_t *> g_nav_buttons;  // index = page, nullptr if not in menu

inline std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(sep, start);
    out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
    if (pos == std::string::npos) break;
    start = pos + 1;
  }
  return out;
}

inline std::string domain_of(const std::string &entity) {
  size_t dot = entity.find('.');
  return dot == std::string::npos ? std::string() : entity.substr(0, dot);
}

inline const char *find_icon(const std::string &name) {
  if (name.empty()) return nullptr;
  const Icon *first = std::begin(kIcons), *last = std::end(kIcons);
  const Icon *it = std::lower_bound(first, last, name, [](const Icon &icon, const std::string &n) {
    return std::strcmp(icon.name, n.c_str()) < 0;
  });
  return it != last && name == it->name ? it->glyph : nullptr;
}

inline const char *icon_glyph(const Card &c) {
  if (const char *g = find_icon(c.icon)) return g;
  // Defaults are forced into the font by REQUIRED in tools/gen_icons.py.
  if (c.type == "sensor") return find_icon("gauge");
  if (c.type == "action") return find_icon("gesture-tap-button");
  if (c.type == "subpage") return find_icon("folder-outline");
  if (c.type == "alarm") return find_icon("alarm-light");
  if (c.type == "chart") return find_icon("chart-line");
  return find_icon("lightbulb");
}

inline bool is_active_state(const std::string &s) {
  return s == "on" || s == "open" || s == "opening" || s == "playing" || s == "unlocked" || s == "home" ||
         s == "heat" || s == "cool" || s == "heat_cool" || s == "auto" || s == "dry" || s == "fan_only" ||
         s == "cleaning";
}

inline bool is_unavailable(const std::string &s) { return s.empty() || s == "unavailable" || s == "unknown"; }

inline std::string state_of(const std::string &key) {
  auto it = g_states.find(key);
  return it == g_states.end() ? std::string() : it->second;
}

// "21.456" -> "21.5", plus unit if HA reports one.
inline std::string format_sensor(const std::string &entity) {
  std::string state = state_of(entity);
  if (is_unavailable(state)) return "--";
  std::string value = state;
  char *end = nullptr;
  float f = strtof(state.c_str(), &end);
  if (end != state.c_str() && *end == '\0' && state.find('.') != std::string::npos) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", f);
    value = buf;
  }
  std::string unit = state_of(entity + "|unit_of_measurement");
  if (!unit.empty()) value += " " + unit;
  return value;
}

inline bool parse_number(const std::string &s, float *out) {
  if (is_unavailable(s)) return false;
  char *end = nullptr;
  float f = strtof(s.c_str(), &end);
  if (end == s.c_str() || *end != '\0' || std::isnan(f)) return false;
  *out = f;
  return true;
}

inline std::string format_number(float v) {
  char buf[24];
  if (std::fabs(v) >= 1000) snprintf(buf, sizeof(buf), "%.0f", v);
  else snprintf(buf, sizeof(buf), "%.1f", v);
  return buf;
}

inline void set_hidden(lv_obj_t *obj, bool hidden) {
  if (obj == nullptr) return;
  if (hidden) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

inline void set_state(lv_obj_t *obj, lv_state_t state, bool on) {
  if (on) {
    lv_obj_add_state(obj, state);
  } else {
    lv_obj_clear_state(obj, state);
  }
}

// ----------------------------------------------------------------- alarm

// Ukraine Alarm integration sensors, by suffix of the region's entity ids.
// Air (red) is ballistic missiles, Air (yellow) is drones.
enum AlarmLevel { ALARM_UNKNOWN = -1, ALARM_CLEAR = 0, ALARM_YELLOW = 1, ALARM_RED = 2 };

struct AlarmKind {
  const char *suffix;
  const char *text;
  AlarmLevel level;
};

inline const AlarmKind kAlarmKinds[] = {
    {"_air_red", "БАЛІСТИКА", ALARM_RED},
    {"_air", "ПОВІТРЯНА ТРИВОГА", ALARM_RED},
    {"_artillery", "АРТОБСТРІЛ", ALARM_RED},
    {"_urban_fights", "ВУЛИЧНІ БОЇ", ALARM_RED},
    {"_chemical", "ХІМІЧНА ЗАГРОЗА", ALARM_RED},
    {"_nuclear", "ЯДЕРНА ЗАГРОЗА", ALARM_RED},
    {"_unknown", "ЗАГРОЗА", ALARM_RED},
    {"_air_yellow", "ДРОНИ", ALARM_YELLOW},
};

inline std::string g_alarm_base;  // "binary_sensor.<region>" (entity without "_air")

constexpr uint32_t kPageBg = 0x0B1114;
inline uint32_t alarm_page_bg(AlarmLevel l) { return l == ALARM_RED ? 0x60120E : l == ALARM_YELLOW ? 0x4A3A00 : kPageBg; }
inline uint32_t alarm_card_bg(AlarmLevel l) {
  return l == ALARM_RED ? 0x60120E : l == ALARM_YELLOW ? 0x6B5300 : l == ALARM_CLEAR ? 0x17320E : 0x212121;
}
inline uint32_t alarm_card_border(AlarmLevel l) {
  return l == ALARM_RED ? 0xE53935 : l == ALARM_YELLOW ? 0xFDD835 : l == ALARM_CLEAR ? 0x2E7D32 : 0x444444;
}

inline bool is_alarm_entity(const std::string &entity) {
  if (g_alarm_base.empty() || entity.compare(0, g_alarm_base.size(), g_alarm_base) != 0) return false;
  for (const auto &k : kAlarmKinds)
    if (entity.size() == g_alarm_base.size() + strlen(k.suffix) && entity.compare(g_alarm_base.size(), std::string::npos, k.suffix) == 0)
      return true;
  return false;
}

// Highest active level and the names of active threats (" · " separated).
inline AlarmLevel alarm_status(std::string *text = nullptr) {
  if (g_alarm_base.empty()) return ALARM_UNKNOWN;
  AlarmLevel level = ALARM_UNKNOWN;
  std::string names;
  for (const auto &k : kAlarmKinds) {
    std::string s = state_of(g_alarm_base + k.suffix);
    if (s == "on") {
      if (k.level > level) level = k.level;
      if (!names.empty()) names += " · ";
      names += k.text;
    } else if (s == "off" && level < ALARM_CLEAR) {
      level = ALARM_CLEAR;
    }
  }
  if (text != nullptr) *text = level == ALARM_UNKNOWN ? "Немає даних" : level == ALARM_CLEAR ? "НЕБО ЧИСТЕ" : names;
  return level;
}

// For YAML: wake the screen when this rises.
inline int alarm_level() { return alarm_status(); }

// ---------------------------------------------------------------- layout

// Grid adapts to the available area: 4 columns on wide screens, fewer on
// narrow ones; three rows of cards fit without scrolling.
inline void apply_size(int i) {
  if (g_grid == nullptr || g_widgets[i].card == nullptr) return;
  lv_coord_t w = lv_obj_get_content_width(g_grid);
  lv_coord_t h = lv_obj_get_content_height(g_grid);
  if (w <= 0 || h <= 0) return;
  int cols = w >= 900 ? 4 : (w >= 600 ? 3 : 2);
  int rows = h >= 380 ? 3 : 2;
  lv_coord_t cw = (w - (cols - 1) * GAP) / cols;
  lv_coord_t rh = (h - (rows - 1) * GAP) / rows;
  int span_x = 1, span_y = 1;
  switch (g_cards[i].size) {
    case 2: span_x = 2; break;
    case 3: span_x = 2; span_y = 2; break;
    case 4: span_x = cols; break;
    default: break;
  }
  if (span_x > cols) span_x = cols;
  lv_coord_t card_h = rh * span_y + GAP * (span_y - 1);
  lv_obj_set_size(g_widgets[i].card, cw * span_x + GAP * (span_x - 1), card_h);
  if (lv_obj_t *chart = g_widgets[i].chart) {
    // Background of the card: full width (inside the border), lower part.
    lv_obj_t *card = g_widgets[i].card;
    lv_coord_t pad = lv_obj_get_style_pad_bottom(card, LV_PART_MAIN);
    lv_coord_t bw = lv_obj_get_style_border_width(card, LV_PART_MAIN);
    lv_coord_t card_w = cw * span_x + GAP * (span_x - 1);
    lv_obj_set_size(chart, card_w - 2 * bw, (card_h - 2 * bw) * 2 / 3);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, pad - bw);
  }
}

// ---------------------------------------------------------------- charts

// Returns true when the chart widgets were just created.
inline bool ensure_chart(int i) {
  Widgets &w = g_widgets[i];
  if (w.chart != nullptr) return false;
  w.chart = lv_chart_create(w.card);
  lv_obj_remove_flag(w.chart, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(w.chart, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_move_to_index(w.chart, 0);  // behind the icon and the labels
  lv_obj_set_style_bg_opa(w.chart, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(w.chart, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(w.chart, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(w.chart, 8, LV_PART_MAIN);
  lv_obj_set_style_radius(w.chart, 0, LV_PART_MAIN);
  lv_obj_set_style_line_width(w.chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_line_opa(w.chart, LV_OPA_50, LV_PART_ITEMS);
  lv_obj_set_style_width(w.chart, 0, LV_PART_INDICATOR);
  lv_obj_set_style_height(w.chart, 0, LV_PART_INDICATOR);
  lv_chart_set_type(w.chart, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(w.chart, 0, 0);
  lv_chart_set_point_count(w.chart, CHART_POINTS);
  w.series = lv_chart_add_series(w.chart, lv_color_hex(0x64D2FF), LV_CHART_AXIS_PRIMARY_Y);

  w.stats = lv_label_create(w.card);
  if (g_small_font != nullptr) lv_obj_set_style_text_font(w.stats, g_small_font, 0);
  lv_obj_set_style_text_color(w.stats, lv_color_hex(0x8899A0), 0);
  lv_obj_align(w.stats, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  return true;
}

inline void hide_chart(int i) {
  set_hidden(g_widgets[i].chart, true);
  set_hidden(g_widgets[i].stats, true);
}

inline void render_chart(int i) {
  Widgets &w = g_widgets[i];
  const History &h = g_history[i];
  if (ensure_chart(i)) apply_size(i);
  set_hidden(w.chart, false);
  set_hidden(w.stats, false);

  static float pts[CHART_POINTS];
  int n = h.points(pts);
  float lo = INFINITY, hi = -INFINITY;
  int known = 0;
  for (int k = 0; k < n; k++)
    if (!std::isnan(pts[k])) {
      lo = std::min(lo, pts[k]);
      hi = std::max(hi, pts[k]);
      known++;
    }

  static int32_t vals[CHART_POINTS];
  // Right-aligned: the newest point is at the right edge.
  for (int k = 0; k < CHART_POINTS; k++) vals[k] = LV_CHART_POINT_NONE;
  if (known > 0) {
    // Don't blow sensor noise up to the full height: the visible range is at
    // least 2% of the value (6 V around 285 V, 0.4 degrees around 21).
    float mid = (lo + hi) / 2;
    float span = std::max(hi - lo, std::max(std::fabs(mid) * 0.02f, 0.1f));
    float min = mid - span * 0.6f, max = mid + span * 0.6f;
    for (int k = 0; k < n; k++)
      if (!std::isnan(pts[k])) vals[CHART_POINTS - n + k] = (int32_t) std::lround((pts[k] - min) / (max - min) * 1000);
  }
  lv_chart_set_axis_range(w.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
  lv_chart_set_series_values(w.chart, w.series, vals, CHART_POINTS);

  if (known < 2)
    lv_label_set_text(w.stats, "збираю дані…");
  else
    lv_label_set_text(w.stats, ("мін " + format_number(lo) + "  макс " + format_number(hi)).c_str());
}

// ---------------------------------------------------------------- rendering

inline void render_slot(int i) {
  Widgets &w = g_widgets[i];
  if (w.card == nullptr) return;
  const Card &c = g_cards[i];
  if (!c.valid() || c.page != g_page) {
    set_hidden(w.card, true);
    return;
  }
  set_hidden(w.card, false);
  apply_size(i);
  lv_label_set_text(w.icon, icon_glyph(c));
  lv_label_set_text(w.label, c.label.empty() ? c.entity.c_str() : c.label.c_str());

  if (c.type != "chart") hide_chart(i);

  if (c.type == "alarm") {
    // Big status text at the bottom, card title in the corner.
    std::string text;
    AlarmLevel level = alarm_status(&text);
    lv_obj_set_style_bg_color(w.card, lv_color_hex(alarm_card_bg(level)), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(w.card, lv_color_hex(alarm_card_border(level)), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(w.label, g_alarm_base.empty() ? "Не налаштовано" : text.c_str());
    lv_label_set_text(w.value, c.label.empty() ? "Тривога" : c.label.c_str());
    set_state(w.card, LV_STATE_CHECKED, false);
    set_state(w.card, LV_STATE_DISABLED, false);
    return;
  }
  if (c.type != "chart") lv_obj_set_width(w.label, lv_pct(100));
  lv_obj_remove_local_style_prop(w.card, LV_STYLE_BG_COLOR, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_remove_local_style_prop(w.card, LV_STYLE_BORDER_COLOR, LV_PART_MAIN | LV_STATE_DEFAULT);

  if (c.type == "subpage") {
    int target = atoi(c.entity.c_str());
    if (c.label.empty() && target >= 0 && target < (int) g_pages.size())
      lv_label_set_text(w.label, g_pages[target].title.c_str());
    lv_label_set_text(w.value, "");
    set_state(w.card, LV_STATE_CHECKED, false);
    set_state(w.card, LV_STATE_DISABLED, false);
    return;
  }

  std::string state = state_of(c.entity);
  bool unavailable = is_unavailable(state) && c.type != "action";
  set_state(w.card, LV_STATE_DISABLED, unavailable);

  std::string value;
  bool on = false;
  if (c.type == "chart") {
    value = format_sensor(c.entity);
    render_chart(i);
    lv_obj_set_width(w.label, lv_pct(55));
  } else if (c.type == "sensor") {
    value = format_sensor(c.entity);
  } else if (c.type == "toggle") {
    on = is_active_state(state);
    value = unavailable ? "н/д" : (on ? "Увімк" : "Вимк");
  }
  set_state(w.card, LV_STATE_CHECKED, on);
  lv_label_set_text(w.value, value.c_str());
}

inline void render_nav() {
  for (int i = 0; i < (int) g_nav_buttons.size(); i++)
    if (g_nav_buttons[i] != nullptr) set_state(g_nav_buttons[i], LV_STATE_CHECKED, i == g_page);
}

inline void render_page() {
  if (g_page >= (int) g_pages.size()) g_page = 0;
  if (g_title != nullptr) lv_label_set_text(g_title, g_pages[g_page].title.c_str());
  set_hidden(g_back, g_nav_stack.empty());
  bool any = false;
  for (int i = 0; i < SLOTS; i++) any = any || (g_cards[i].valid() && g_cards[i].page == g_page);
  set_hidden(g_empty_hint, any);
  if (g_grid != nullptr) {
    set_hidden(g_grid, !any);
    lv_obj_update_layout(g_grid);
    lv_obj_scroll_to_y(g_grid, 0, LV_ANIM_OFF);
  }
  for (int i = 0; i < SLOTS; i++) render_slot(i);
  render_nav();
}

inline void render_header() {
  if (g_header_label == nullptr) return;
  lv_label_set_text(g_header_label, g_header_entity.empty() ? "" : format_sensor(g_header_entity).c_str());
}

inline void render_alarm() {
  if (g_page_bg != nullptr)
    lv_obj_set_style_bg_color(g_page_bg, lv_color_hex(alarm_page_bg(alarm_status())), LV_PART_MAIN);
  for (int i = 0; i < SLOTS; i++)
    if (g_cards[i].type == "alarm") render_slot(i);
}

inline void render_entity(const std::string &entity) {
  float v;
  if (parse_number(state_of(entity), &v))
    for (int i = 0; i < SLOTS; i++)
      if (g_cards[i].type == "chart" && g_cards[i].entity == entity) g_history[i].add(v);
  for (int i = 0; i < SLOTS; i++)
    if (g_cards[i].entity == entity && g_cards[i].type != "subpage" && g_cards[i].type != "alarm") render_slot(i);
  if (entity == g_header_entity) render_header();
  if (is_alarm_entity(entity)) render_alarm();
}

// ------------------------------------------------------- Home Assistant API

inline void subscribe_key(const std::string &entity, const std::string &attribute) {
  auto *api = esphome::api::global_api_server;
  if (api == nullptr) return;
  std::string key = attribute.empty() ? entity : entity + "|" + attribute;
  if (!g_subscribed.insert(key).second) return;

  std::function<void(esphome::StringRef)> cb = [key, entity](esphome::StringRef value) {
    g_states[key] = value.str();
    render_entity(entity);
  };
  esphome::optional<std::string> attr;
  if (!attribute.empty()) attr = attribute;
  api->subscribe_home_assistant_state(entity, attr, std::move(cb));

  // Subscriptions are only replayed to Home Assistant when it (re)connects.
  // If it is already connected, announce the new subscription right away so a
  // card added from the web page goes live without a reboot.
  if (!api->is_connected()) return;
  esphome::api::SubscribeHomeAssistantStateResponse req;
  req.entity_id = esphome::StringRef(entity);
  req.attribute = esphome::StringRef(attribute);
  req.once = false;
  for (auto &client : api->active_clients()) {
    if (!client || client->is_marked_for_removal() || !client->is_authenticated()) continue;
    const char *name = client->get_name();
    if (name == nullptr || strstr(name, "Home Assistant") == nullptr) continue;
    (void) client->send_message(req);
  }
}

inline void subscribe_entity(const std::string &entity, bool with_unit) {
  if (entity.empty()) return;
  subscribe_key(entity, "");
  if (with_unit) subscribe_key(entity, "unit_of_measurement");
}

inline void call_action(const std::string &service, const std::string &entity) {
  auto *api = esphome::api::global_api_server;
  if (api == nullptr || !api->is_connected()) {
    ESP_LOGW("panel_cards", "Home Assistant not connected; dropping %s", service.c_str());
    return;
  }
  esphome::api::HomeassistantActionRequest req;
  req.service = esphome::StringRef(service);
  esphome::api::HomeassistantServiceMap kv;
  kv.key = esphome::StringRef("entity_id");
  kv.value = esphome::StringRef(entity);
  req.data.init(1);
  req.data.push_back(kv);
  ESP_LOGI("panel_cards", "Calling %s on %s", service.c_str(), entity.c_str());
  api->send_homeassistant_action(req);
}

// ------------------------------------------------------------- navigation

// Ignore taps for a short while (used when a touch wakes the dimmed screen).
inline void suppress_clicks(uint32_t ms) { g_clicks_blocked_until = esphome::millis() + ms; }
inline bool clicks_blocked() { return (int32_t) (g_clicks_blocked_until - esphome::millis()) > 0; }

inline void set_page(int page, bool push = true) {
  if (page < 0 || page >= (int) g_pages.size()) page = 0;
  if (push && page != g_page) g_nav_stack.push_back(g_page);
  g_page = page;
  render_page();
}

// Bottom-menu navigation starts a fresh history.
inline void open_root(int page) {
  g_nav_stack.clear();
  set_page(page, false);
}

inline void go_home() { open_root(0); }

inline void go_back() {
  if (clicks_blocked()) return;
  if (g_nav_stack.empty()) {
    go_home();
    return;
  }
  int prev = g_nav_stack.back();
  g_nav_stack.pop_back();
  set_page(prev, false);
}

inline void nav_event_cb(lv_event_t *e) {
  if (clicks_blocked()) return;
  open_root((int) (intptr_t) lv_event_get_user_data(e));
}

inline void rebuild_nav() {
  if (g_nav == nullptr) return;
  lv_obj_clean(g_nav);
  g_nav_buttons.assign(g_pages.size(), nullptr);
  for (int i = 0; i < (int) g_pages.size(); i++) {
    const Page &p = g_pages[i];
    if (!p.nav) continue;
    lv_obj_t *btn = lv_btn_create(g_nav);
    lv_obj_remove_style_all(btn);
    lv_obj_set_height(btn, lv_pct(100));
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 8, 0);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x103E5C), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_TOP, LV_STATE_CHECKED);
    lv_obj_set_style_border_width(btn, 2, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00FF00), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1A4A6A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, nav_event_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);

    if (const char *glyph = find_icon(p.icon)) {
      lv_obj_t *icon = lv_label_create(btn);
      if (g_icon_font != nullptr) lv_obj_set_style_text_font(icon, g_icon_font, 0);
      lv_label_set_text(icon, glyph);
    }
    lv_obj_t *label = lv_label_create(btn);
    if (g_text_font != nullptr) lv_obj_set_style_text_font(label, g_text_font, 0);
    lv_label_set_text(label, p.title.c_str());
    g_nav_buttons[i] = btn;
  }
  render_nav();
}

// --------------------------------------------------------- config setters

inline void set_config(int slot, const std::string &raw) {
  if (slot < 1 || slot > SLOTS) return;
  Card c;
  auto parts = split(raw, ';');
  if (parts.size() >= 2) {
    c.type = parts[0];
    c.entity = parts[1];
    if (parts.size() > 2) c.label = parts[2];
    if (parts.size() > 3) c.icon = parts[3];
    if (parts.size() > 4) c.page = atoi(parts[4].c_str());
    if (parts.size() > 5) c.size = atoi(parts[5].c_str());
    if (parts.size() > 6) c.period = atoi(parts[6].c_str());
    if (c.period != 1 && c.period != 6 && c.period != 24) c.period = 24;
    if (c.page < 0 || c.page >= MAX_PAGES) c.page = 0;
    if (c.size < 1 || c.size > 4) c.size = 1;
    if (c.type != "toggle" && c.type != "sensor" && c.type != "action" && c.type != "subpage" && c.type != "alarm" &&
        c.type != "chart")
      c = Card{};
  }
  g_cards[slot - 1] = c;
  History &h = g_history[slot - 1];
  if (c.type != "chart") {
    h.reset("", 0);
  } else if (h.entity != c.entity || h.period != c.period) {
    h.reset(c.entity, c.period);
    float v;
    if (parse_number(state_of(c.entity), &v)) h.add(v);
  }
  if (c.valid() && c.type != "subpage" && c.type != "alarm")
    subscribe_entity(c.entity, c.type == "sensor" || c.type == "chart");
  render_page();
}

inline void set_pages(const std::string &raw) {
  std::vector<Page> pages;
  for (const auto &entry : split(raw, ';')) {
    if (pages.size() >= (size_t) MAX_PAGES) break;
    auto f = split(entry, '|');
    Page p;
    p.title = f[0];
    if (p.title.empty()) continue;
    if (f.size() > 1) p.icon = f[1];
    p.nav = f.size() < 3 || f[2] != "0";
    pages.push_back(p);
  }
  if (pages.empty()) pages.push_back({"Головна", "home", true});
  g_pages = pages;
  g_nav_stack.clear();
  if (g_page >= (int) g_pages.size()) g_page = 0;
  rebuild_nav();
  render_page();
}

inline void set_header_entity(const std::string &entity) {
  g_header_entity = entity;
  subscribe_entity(entity, true);
  render_header();
}

// Accepts the region's "Air" sensor (binary_sensor.x_air) or the bare prefix.
inline void set_alarm_entity(const std::string &raw) {
  std::string base = raw;
  if (base.size() > 4 && base.compare(base.size() - 4, 4, "_air") == 0) base.resize(base.size() - 4);
  g_alarm_base = base;
  if (!base.empty())
    for (const auto &k : kAlarmKinds) subscribe_entity(base + k.suffix, false);
  render_alarm();
}

// Call every second: closes chart buckets and redraws visible charts.
inline void tick() {
  uint32_t now = esphome::millis();
  for (int i = 0; i < SLOTS; i++) {
    if (g_cards[i].type != "chart") continue;
    if (g_history[i].advance(now) && g_cards[i].page == g_page) render_slot(i);
  }
}

inline void set_small_font(const lv_font_t *font) { g_small_font = font; }

// ------------------------------------------------------- widget binding

inline void bind_page(lv_obj_t *title, lv_obj_t *back, lv_obj_t *empty_hint, lv_obj_t *grid) {
  g_title = title;
  g_back = back;
  g_empty_hint = empty_hint;
  g_grid = grid;
  g_page_bg = lv_obj_get_parent(grid);
  render_page();
  render_alarm();
}

inline void bind_nav(lv_obj_t *container, const lv_font_t *icon_font, const lv_font_t *text_font) {
  g_nav = container;
  lv_obj_set_layout(container, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(container, LV_FLEX_FLOW_ROW);
  g_icon_font = icon_font;
  g_text_font = text_font;
  rebuild_nav();
}

inline void bind_header(lv_obj_t *label) {
  g_header_label = label;
  render_header();
}

inline void bind(int slot, lv_obj_t *card, lv_obj_t *icon, lv_obj_t *label, lv_obj_t *value) {
  if (slot < 1 || slot > SLOTS) return;
  g_widgets[slot - 1] = Widgets{card, icon, label, value};
  render_slot(slot - 1);
}

inline void on_click(int slot) {
  if (slot < 1 || slot > SLOTS || clicks_blocked()) return;
  const Card &c = g_cards[slot - 1];
  if (!c.valid()) return;
  if (c.type == "subpage") {
    set_page(atoi(c.entity.c_str()));
    return;
  }
  if (c.type == "sensor" || c.type == "alarm") return;

  std::string domain = domain_of(c.entity);
  std::string service;
  if (c.type == "toggle") {
    if (domain == "cover") {
      service = "cover.toggle";
    } else if (domain == "lock") {
      service = is_active_state(state_of(c.entity)) ? "lock.lock" : "lock.unlock";
    } else {
      service = "homeassistant.toggle";
    }
  } else {  // action
    if (domain == "scene") service = "scene.turn_on";
    else if (domain == "script") service = "script.turn_on";
    else if (domain == "automation") service = "automation.trigger";
    else if (domain == "button") service = "button.press";
    else if (domain == "input_button") service = "input_button.press";
    else service = "homeassistant.turn_on";
  }
  call_action(service, c.entity);
}

}  // namespace panel_cards
