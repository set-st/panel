#pragma once
// GET /screenshot.bmp — snapshot of what the panel currently shows (active
// screen + LVGL top layer with the header and bottom menu), as a 16-bit BMP.
//
// LVGL may only be touched from the main loop, while web requests run in the
// httpd task. The request handler therefore raises a flag and waits; poll()
// (called from an interval in screenshot.yaml) takes the snapshot in the main
// loop and signals back. Buffers live in PSRAM and are freed after sending.

#include <algorithm>
#include <atomic>

#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace screenshot {

static const char *const TAG = "screenshot";

inline std::atomic<bool> g_requested{false};
inline SemaphoreHandle_t g_done = nullptr;
inline lv_draw_buf_t *g_image = nullptr;  // RGB565, owned by the httpd side after g_done

inline void install();

// Blends an ARGB8888 snapshot of a top-layer object, located at (ox, oy) on
// the screen, over the RGB565 screen snapshot in place.
inline void compose(lv_draw_buf_t *screen, const lv_draw_buf_t *top, int32_t ox, int32_t oy) {
  for (uint32_t ty = 0; ty < top->header.h; ty++) {
    int32_t y = oy + (int32_t) ty;
    if (y < 0 || y >= (int32_t) screen->header.h) continue;
    auto *dst = reinterpret_cast<uint16_t *>(screen->data + y * screen->header.stride);
    const uint8_t *src = top->data + ty * top->header.stride;
    for (uint32_t tx = 0; tx < top->header.w; tx++, src += 4) {
      int32_t x = ox + (int32_t) tx;
      if (x < 0 || x >= (int32_t) screen->header.w) continue;
      uint32_t a = src[3];
      if (a == 0) continue;
      uint32_t b = src[0], g = src[1], r = src[2];
      if (a < 255) {
        uint16_t p = dst[x];
        uint32_t pr = (p >> 11) << 3, pg = ((p >> 5) & 0x3F) << 2, pb = (p & 0x1F) << 3;
        r = (r * a + pr * (255 - a)) / 255;
        g = (g * a + pg * (255 - a)) / 255;
        b = (b * a + pb * (255 - a)) / 255;
      }
      dst[x] = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
}

// Snapshots each visible child of the top layer separately (header, menu,
// overlays): a full-screen ARGB buffer needs 2.4 MB of contiguous PSRAM,
// which is often not available.
inline void compose_top_layer(lv_draw_buf_t *screen) {
  lv_obj_t *layer = lv_layer_top();
  uint32_t n = lv_obj_get_child_count(layer);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t *child = lv_obj_get_child(layer, i);
    if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_draw_buf_t *buf = lv_snapshot_take(child, LV_COLOR_FORMAT_ARGB8888);
    if (buf == nullptr) {
      ESP_LOGW(TAG, "Top layer object %u: snapshot failed (out of memory?)", (unsigned) i);
      continue;
    }
    // The snapshot covers the object's area extended by shadows/outlines.
    lv_area_t area;
    lv_obj_get_coords(child, &area);
    int32_t ext = ((int32_t) buf->header.w - lv_area_get_width(&area)) / 2;
    compose(screen, buf, area.x1 - ext, area.y1 - ext);
    lv_draw_buf_destroy(buf);
  }
}

// Main loop side. Registers the handler on first call (the web server is set
// up by then) and serves pending snapshot requests.
inline void poll() {
  if (g_done == nullptr) install();
  if (!g_requested.exchange(false)) return;
  lv_draw_buf_t *screen = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  if (screen != nullptr) {
    compose_top_layer(screen);
  } else {
    ESP_LOGW(TAG, "Screen snapshot failed (out of memory?)");
  }
  g_image = screen;
  xSemaphoreGive(g_done);
}

inline void put_le(uint8_t *p, uint32_t v, int n) {
  for (int i = 0; i < n; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

class Handler : public AsyncWebHandler {
 public:
  bool canHandle(AsyncWebServerRequest *request) const override {
    char buf[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->method() == HTTP_GET && request->url_to(buf) == "/screenshot.bmp";
  }

  void handleRequest(AsyncWebServerRequest *request) override {
    httpd_req_t *req = *request;
    // Drop a result left over from a request that timed out.
    if (xSemaphoreTake(g_done, 0) == pdTRUE && g_image != nullptr) {
      lv_draw_buf_destroy(g_image);
      g_image = nullptr;
    }
    g_requested = true;
    if (xSemaphoreTake(g_done, pdMS_TO_TICKS(3000)) != pdTRUE || g_image == nullptr) {
      g_requested = false;
      request->send(503, "text/plain", "Snapshot failed");
      return;
    }
    lv_draw_buf_t *img = g_image;
    g_image = nullptr;

    const uint32_t w = img->header.w, h = img->header.h, row = w * 2;
    const uint32_t pad = (4 - row % 4) % 4, offset = 14 + 40 + 12;
    const uint32_t size = offset + (row + pad) * h;
    uint8_t hdr[offset] = {'B', 'M'};
    put_le(hdr + 2, size, 4);
    put_le(hdr + 10, offset, 4);
    put_le(hdr + 14, 40, 4);                  // BITMAPINFOHEADER
    put_le(hdr + 18, w, 4);
    put_le(hdr + 22, static_cast<uint32_t>(-static_cast<int32_t>(h)), 4);  // top-down
    put_le(hdr + 26, 1, 2);                   // planes
    put_le(hdr + 28, 16, 2);                  // bpp
    put_le(hdr + 30, 3, 4);                   // BI_BITFIELDS
    put_le(hdr + 34, (row + pad) * h, 4);
    put_le(hdr + 54, 0xF800, 4);              // R mask
    put_le(hdr + 58, 0x07E0, 4);              // G mask
    put_le(hdr + 62, 0x001F, 4);              // B mask

    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(hdr), offset);
    // Send in blocks of rows; rows are 4-byte aligned when the width is even.
    constexpr uint32_t ROWS = 16;
    static const char zeros[4] = {};
    for (uint32_t y = 0; y < h && err == ESP_OK; y += ROWS) {
      uint32_t n = std::min(ROWS, h - y);
      if (pad == 0 && img->header.stride == row) {
        err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(img->data + y * row), n * row);
      } else {
        for (uint32_t i = 0; i < n && err == ESP_OK; i++) {
          err = httpd_resp_send_chunk(req, reinterpret_cast<const char *>(img->data + (y + i) * img->header.stride), row);
          if (err == ESP_OK && pad) err = httpd_resp_send_chunk(req, zeros, pad);
        }
      }
    }
    if (err == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    lv_draw_buf_destroy(img);
    ESP_LOGD(TAG, "Sent %ux%u screenshot (%s)", w, h, err == ESP_OK ? "ok" : esp_err_to_name(err));
  }
};

inline void install() {
  if (g_done != nullptr) return;
  g_done = xSemaphoreCreateBinary();
  esphome::web_server_base::global_web_server_base->add_handler(new Handler());  // NOLINT
  ESP_LOGI(TAG, "Screenshot available at /screenshot.bmp");
}

}  // namespace screenshot
