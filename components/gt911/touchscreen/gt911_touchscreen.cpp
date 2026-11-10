#include "gt911_touchscreen.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/gpio.h"

namespace esphome::gt911 {

static const char *const TAG = "gt911.touchscreen";

static const uint8_t PRIMARY_ADDRESS = 0x5D;    // default I2C address for GT911
static const uint8_t SECONDARY_ADDRESS = 0x14;  // secondary I2C address for GT911
static const uint8_t GET_TOUCH_STATE[2] = {0x81, 0x4E};
static const uint8_t CLEAR_TOUCH_STATE[3] = {0x81, 0x4E, 0x00};
static const uint8_t GET_TOUCHES[2] = {0x81, 0x4F};
static const uint8_t GET_SWITCHES[2] = {0x80, 0x4D};
static const uint8_t GET_MAX_VALUES[2] = {0x80, 0x48};
static const size_t MAX_TOUCHES = 5;  // max number of possible touches reported
static const size_t MAX_BUTTONS = 4;  // max number of buttons scanned

#define ERROR_CHECK(err) \
  if ((err) != i2c::ERROR_OK) { \
    this->status_set_warning(LOG_STR(ESP_LOG_MSG_COMM_FAIL)); \
    return; \
  }

// Local copy of the ESPHome 2026.9 driver with a sturdier start-up for the
// Waveshare ESP32-S3-Touch-LCD-7B, where RESET goes through the CH32V003 I2C
// IO expander (see README):
//  * reset timing as in the GT911 datasheet / Waveshare demo: RESET low with
//    INT setting the address, 10 ms, RESET high, hold INT 10 ms, release INT,
//    then wait 60 ms before the first I2C transfer (upstream talks to the chip
//    right after releasing INT);
//  * the configured address is kept: a failed probe does not permanently
//    switch to the secondary address;
//  * up to SETUP_ATTEMPTS reset + probe cycles before giving up.
static const uint8_t SETUP_ATTEMPTS = 5;

void GT911Touchscreen::setup() { this->start_reset_(); }

void GT911Touchscreen::start_reset_() {
  this->setup_attempt_++;
  if (this->reset_pin_ == nullptr) {
    this->set_timeout("gt911_setup", this->setup_attempt_ == 1 ? 0 : 100, [this] { this->setup_internal_(); });
    return;
  }
  this->reset_pin_->setup();
  this->reset_pin_->digital_write(false);
  if (this->interrupt_pin_ != nullptr) {
    // INT level while RESET rises selects the address: low = 0x5D, high = 0x14.
    this->interrupt_pin_->setup();
    this->interrupt_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->interrupt_pin_->digital_write(this->address_ == SECONDARY_ADDRESS);
  }
  this->set_timeout("gt911_setup", 10, [this] {
    this->reset_pin_->digital_write(true);
    this->set_timeout("gt911_setup", 10, [this] {
      if (this->interrupt_pin_ != nullptr && this->interrupt_pin_->is_internal())
        this->interrupt_pin_->pin_mode(gpio::FLAG_INPUT);
      this->set_timeout("gt911_setup", 60, [this] { this->setup_internal_(); });
    });
  });
}

void GT911Touchscreen::setup_failed_(const LogString *message) {
  if (this->setup_attempt_ < SETUP_ATTEMPTS) {
    ESP_LOGW(TAG, "Setup attempt %u/%u failed (%s), resetting the controller again", this->setup_attempt_,
             SETUP_ATTEMPTS, LOG_STR_ARG(message));
    this->set_timeout("gt911_setup", 100, [this] { this->start_reset_(); });
    return;
  }
  this->mark_failed(message);
}

void GT911Touchscreen::setup_internal_() {
  uint8_t data[4];
  const uint8_t configured = this->address_;
  i2c::ErrorCode err = this->write(GET_SWITCHES, sizeof(GET_SWITCHES));
  if (err != i2c::ERROR_OK && this->reset_pin_ == nullptr && configured == PRIMARY_ADDRESS) {
    // Without a reset pin the address could not be selected; try the other one.
    this->address_ = SECONDARY_ADDRESS;
    err = this->write(GET_SWITCHES, sizeof(GET_SWITCHES));
    if (err != i2c::ERROR_OK)
      this->address_ = configured;
  }
  if (err == i2c::ERROR_OK)
    err = this->read(data, 1);
  if (err != i2c::ERROR_OK) {
    this->setup_failed_(LOG_STR(ESP_LOG_MSG_COMM_FAIL));
    return;
  }
  ESP_LOGD(TAG, "Switches ADDR: 0x%02X DATA: 0x%02X (attempt %u)", this->address_, data[0], this->setup_attempt_);

  // data[0] & 1 == 1  =>  controller uses falling edge  =>  active-low
  // data[0] & 1 == 0  =>  controller uses rising  edge  =>  active-high
  bool active_high = !(data[0] & 1);

  if (this->x_raw_max_ == 0 || this->y_raw_max_ == 0) {
    // no calibration? Attempt to read the max values from the touchscreen.
    err = this->write(GET_MAX_VALUES, sizeof(GET_MAX_VALUES));
    if (err == i2c::ERROR_OK)
      err = this->read(data, sizeof(data));
    if (err != i2c::ERROR_OK) {
      this->setup_failed_(LOG_STR("Calibration error"));
      return;
    }
    this->x_raw_max_ = encode_uint16(data[1], data[0]);
    this->y_raw_max_ = encode_uint16(data[3], data[2]);
    if (this->swap_x_y_)
      std::swap(this->x_raw_max_, this->y_raw_max_);
  }

  if (this->interrupt_pin_ != nullptr) {
    if (this->interrupt_pin_->is_internal()) {
      // Direct MCU pin: attach a hardware interrupt, no polling needed.
      this->attach_interrupt_(static_cast<InternalGPIOPin *>(this->interrupt_pin_),
                              active_high ? gpio::INTERRUPT_RISING_EDGE : gpio::INTERRUPT_FALLING_EDGE);
      ESP_LOGD(TAG, "Interrupt pin: hardware interrupt, active %s",
               active_high ? LOG_STR_LITERAL("HIGH") : LOG_STR_LITERAL("LOW"));
    } else {
      // IO expander pin: leave as output for configuration only.
      ESP_LOGD(TAG, "Interrupt pin: IO expander polling mode, active %s",
               active_high ? LOG_STR_LITERAL("HIGH") : LOG_STR_LITERAL("LOW"));
    }
  }
  if (this->setup_attempt_ > 1)
    ESP_LOGI(TAG, "Touchscreen ready after %u attempts", this->setup_attempt_);
  this->setup_done_ = true;
}

void GT911Touchscreen::update_touches() {
  this->skip_update_ = true;  // skip send touch events by default, set to false after successful error checks
  if (!this->setup_done_) {
    return;
  }

  i2c::ErrorCode err;
  uint8_t touch_state = 0;
  uint8_t data[MAX_TOUCHES + 1][8];  // 8 bytes each for each point, plus extra space for the key byte

  err = this->write(GET_TOUCH_STATE, sizeof(GET_TOUCH_STATE));
  ERROR_CHECK(err);
  err = this->read(&touch_state, 1);
  ERROR_CHECK(err);
  this->write(CLEAR_TOUCH_STATE, sizeof(CLEAR_TOUCH_STATE));
  uint8_t num_of_touches = touch_state & 0x07;

  if ((touch_state & 0x80) == 0 || num_of_touches > MAX_TOUCHES) {
    return;
  }

  err = this->write(GET_TOUCHES, sizeof(GET_TOUCHES));
  ERROR_CHECK(err);
  // num_of_touches is guaranteed to be 0..5. Also read the key data
  err = this->read(data[0], sizeof(data[0]) * num_of_touches + 1);
  ERROR_CHECK(err);

  this->skip_update_ = false;  // All error checks passed, send touch events
  for (uint8_t i = 0; i != num_of_touches; i++) {
    uint16_t id = data[i][0];
    uint16_t x = encode_uint16(data[i][2], data[i][1]);
    uint16_t y = encode_uint16(data[i][4], data[i][3]);
    this->add_raw_touch_position_(id, x, y);
  }
  auto keys = data[num_of_touches][0] & ((1 << MAX_BUTTONS) - 1);
  if (keys != this->button_state_) {
    this->button_state_ = keys;
    for (size_t i = 0; i != MAX_BUTTONS; i++) {
      for (auto *listener : this->button_listeners_)
        listener->update_button(i, (keys & (1 << i)) != 0);
    }
  }
}

void GT911Touchscreen::dump_config() {
  ESP_LOGCONFIG(TAG, "GT911 Touchscreen:");
  LOG_I2C_DEVICE(this);
  LOG_PIN("  Interrupt Pin: ", this->interrupt_pin_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  ESP_LOGCONFIG(TAG, "  Setup: %s after %u attempt(s)", this->setup_done_ ? "OK" : "FAILED", this->setup_attempt_);
}

}  // namespace esphome::gt911
