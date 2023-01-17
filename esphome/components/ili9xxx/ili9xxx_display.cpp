#include "ili9xxx_display.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace ili9xxx {

static const char *const TAG = "ili9xxx";

void ILI9XXXDisplay::setup() {
  this->setup_pins_();
  this->initialize();

  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
  if (this->buffer_color_mode_ == BITS_16) {
    this->init_internal_(this->get_buffer_length_() * 2);
    if (this->buffer_ != nullptr) {
      return;
    }
    this->buffer_color_mode_ = BITS_8_INDEXED;
  }
  this->init_internal_(this->get_buffer_length_());
  if (this->buffer_ == nullptr) {
    this->mark_failed();
  }
}

void ILI9XXXDisplay::setup_pins_() {
  this->dc_pin_->setup();  // OUTPUT
  this->dc_pin_->digital_write(false);
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();  // OUTPUT
    this->reset_pin_->digital_write(true);
  }

  if (this->backlight_pin_ != nullptr) {
    this->backlight_pin_->setup();  // OUTPUT
    this->backlight_pin_->digital_write(true);
  }
  this->spi_setup();

  this->reset_();
}

void ILI9XXXDisplay::dump_config() {
  LOG_DISPLAY("", "ili9xxx", this);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_PIN("  backlight Pin: ", this->backlight_pin_);
  LOG_UPDATE_INTERVAL(this);
}

float ILI9XXXDisplay::get_setup_priority() const { return setup_priority::HARDWARE; }

void ILI9XXXDisplay::update() {
  this->do_update_();
  this->display_();
}

void ILI9XXXDisplay::display_() {
  // we will only update the changed window to the display
  uint16_t w = this->x_high_ - this->x_low_ + 1;  // NOLINT
  uint16_t h = this->y_high_ - this->y_low_ + 1;  // NOLINT
  uint32_t start_pos = ((this->y_low_ * this->width_) + x_low_);

  // check if something was displayed
  if ((this->x_high_ < this->x_low_) || (this->y_high_ < this->y_low_)) {
    return;
  }

  set_addr_window_(this->x_low_, this->y_low_, w, h);

  ESP_LOGVV(TAG,
            "Start display(xlow:%d, ylow:%d, xhigh:%d, yhigh:%d, width:%d, "
            "heigth:%d, start_pos:%d)",
            this->x_low_, this->y_low_, this->x_high_, this->y_high_, w, h, start_pos);

  this->start_data_();
  for (uint16_t row = 0; row < h; row++) {
    uint32_t pos = start_pos + (row * width_);
    uint32_t rem = w;

    while (rem > 0) {
      uint32_t sz = buffer_to_transfer_(pos, rem);
      this->write_array(transfer_buffer_, 2 * sz);
      pos += sz;
      rem -= sz;
    }
    App.feed_wdt();
  }
  this->end_data_();

  // invalidate watermarks
  this->x_low_ = this->width_;
  this->y_low_ = this->height_;
  this->x_high_ = 0;
  this->y_high_ = 0;
}

void ILI9XXXDisplay::fill(Color color) {
  uint16_t new_color = 0;
  this->x_low_ = 0;
  this->y_low_ = 0;
  this->x_high_ = this->get_width_internal() - 1;
  this->y_high_ = this->get_height_internal() - 1;
  switch (this->buffer_color_mode_) {
    case BITS_8_INDEXED:
      new_color = display::ColorUtil::color_to_index8_palette888(color, this->palette_);
      break;
    case BITS_16:
      new_color = display::ColorUtil::color_to_565(color);
      for (uint32_t i = 0; i < this->get_buffer_length_() * 2; i = i + 2) {
        this->buffer_[i] = (uint8_t)(new_color >> 8);
        this->buffer_[i + 1] = (uint8_t) new_color;
      }
      return;
      break;
    default:
      new_color = display::ColorUtil::color_to_332(color, display::ColorOrder::COLOR_ORDER_RGB);
      break;
  }
  memset(this->buffer_, (uint8_t) new_color, this->get_buffer_length_());
}

void HOT ILI9XXXDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x >= this->get_width_internal() || x < 0 || y >= this->get_height_internal() || y < 0)
    return;

  uint32_t pos = (y * width_) + x;
  uint16_t new_color;
  bool updated = false;
  switch (this->buffer_color_mode_) {
    case BITS_8_INDEXED:
      new_color = display::ColorUtil::color_to_index8_palette888(color, this->palette_);
      break;
    case BITS_16:
      pos = pos * 2;
      new_color = display::ColorUtil::color_to_565(color, display::ColorOrder::COLOR_ORDER_RGB);
      if (this->buffer_[pos] != (uint8_t)(new_color >> 8)) {
        this->buffer_[pos] = (uint8_t)(new_color >> 8);
        updated = true;
      }
      pos = pos + 1;
      new_color = new_color & 0xFF;
      break;
    default:
      new_color = display::ColorUtil::color_to_332(color, display::ColorOrder::COLOR_ORDER_RGB);
      break;
  }

  if (this->buffer_[pos] != new_color) {
    this->buffer_[pos] = new_color;
    updated = true;
  }
  if (updated) {
    // low and high watermark may speed up drawing from buffer
    this->x_low_ = (x < this->x_low_) ? x : this->x_low_;
    this->y_low_ = (y < this->y_low_) ? y : this->y_low_;
    this->x_high_ = (x > this->x_high_) ? x : this->x_high_;
    this->y_high_ = (y > this->y_high_) ? y : this->y_high_;
    ESP_LOGVV(TAG, "=>>> pixel (x:%d, y:%d) (xl:%d, xh:%d, yl:%d, yh:%d", x, y, this->x_low_, this->x_high_,
              this->y_low_, this->y_high_);
  }
}

// should return the total size: return this->get_width_internal() * this->get_height_internal() * 2 // 16bit color
// values per bit is huge
uint32_t ILI9XXXDisplay::get_buffer_length_() { return this->get_width_internal() * this->get_height_internal(); }

void ILI9XXXDisplay::command(uint8_t value) {
  this->start_command_();
  this->write_byte(value);
  this->end_command_();
}

void ILI9XXXDisplay::data(uint8_t value) {
  this->start_data_();
  this->write_byte(value);
  this->end_data_();
}

void ILI9XXXDisplay::send_command(uint8_t command_byte, const uint8_t *data_bytes, uint8_t num_data_bytes) {
  this->command(command_byte);  // Send the command byte
  this->start_data_();
  this->write_array(data_bytes, num_data_bytes);
  this->end_data_();
}

uint8_t ILI9XXXDisplay::read_command(uint8_t command_byte, uint8_t index) {
  uint8_t data = 0x10 + index;
  this->send_command(0xD9, &data, 1);  // Set Index Register
  uint8_t result;
  this->start_command_();
  this->write_byte(command_byte);
  this->start_data_();
  do {
    result = this->read_byte();
  } while (index--);
  this->end_data_();
  return result;
}

void ILI9XXXDisplay::start_command_() {
  this->dc_pin_->digital_write(false);
  this->enable();
}
void ILI9XXXDisplay::start_data_() {
  this->dc_pin_->digital_write(true);
  this->enable();
}

void ILI9XXXDisplay::end_command_() { this->disable(); }
void ILI9XXXDisplay::end_data_() { this->disable(); }

void ILI9XXXDisplay::reset_() {
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->digital_write(false);
    delay(10);
    this->reset_pin_->digital_write(true);
    delay(10);
  }
}

void ILI9XXXDisplay::init_lcd_(const uint8_t *init_cmd) {
  uint8_t cmd, x, num_args;
  const uint8_t *addr = init_cmd;
  while ((cmd = progmem_read_byte(addr++)) > 0) {
    x = progmem_read_byte(addr++);
    num_args = x & 0x7F;
    send_command(cmd, addr, num_args);
    addr += num_args;
    if (x & 0x80)
      delay(150);  // NOLINT
  }
}

void ILI9XXXDisplay::set_addr_window_(uint16_t x1, uint16_t y1, uint16_t w, uint16_t h) {
  uint16_t x2 = (x1 + w - 1), y2 = (y1 + h - 1);
  this->command(ILI9XXX_CASET);  // Column address set
  this->start_data_();
  this->write_byte(x1 >> 8);
  this->write_byte(x1);
  this->write_byte(x2 >> 8);
  this->write_byte(x2);
  this->end_data_();
  this->command(ILI9XXX_PASET);  // Row address set
  this->start_data_();
  this->write_byte(y1 >> 8);
  this->write_byte(y1);
  this->write_byte(y2 >> 8);
  this->write_byte(y2);
  this->end_data_();
  this->command(ILI9XXX_RAMWR);  // Write to RAM
}

void ILI9XXXDisplay::invert_display_(bool invert) { this->command(invert ? ILI9XXX_INVON : ILI9XXX_INVOFF); }

int ILI9XXXDisplay::get_width_internal() { return this->width_; }
int ILI9XXXDisplay::get_height_internal() { return this->height_; }

uint32_t ILI9XXXDisplay::buffer_to_transfer_(uint32_t pos, uint32_t sz) {
  uint8_t *src = buffer_ + pos;
  uint8_t *dst = transfer_buffer_;
  uint16_t color;
  Color col;

  if (sz > sizeof(transfer_buffer_) / 2) {
    sz = sizeof(transfer_buffer_) / 2;
  }

  for (uint32_t i = 0; i < sz; ++i) {
    switch (this->buffer_color_mode_) {
      case BITS_8_INDEXED:
        color =
            display::ColorUtil::color_to_565(display::ColorUtil::index8_to_color_palette888(*src++, this->palette_));
        break;
      case BITS_16:
        *dst++ = (uint8_t) *src++;
        *dst++ = (uint8_t) *src++;
        continue;
        break;
      default:
        color = display::ColorUtil::color_to_565(display::ColorUtil::rgb332_to_color(*src++));
        break;
    }
    *dst++ = (uint8_t)(color >> 8);
    *dst++ = (uint8_t) color;
  }

  return sz;
}

//   M5Stack display
void ILI9XXXM5Stack::initialize() {
  this->init_lcd_(INITCMD_M5STACK);
  this->width_ = 320;
  this->height_ = 240;
  this->invert_display_(true);
}
//   24_TFT display
void ILI9XXXILI9341::initialize() {
  this->init_lcd_(INITCMD_ILI9341);
  this->width_ = 240;
  this->height_ = 320;
}
//   24_TFT rotated display
void ILI9XXXILI9342::initialize() {
  this->init_lcd_(INITCMD_ILI9341);
  this->width_ = 320;
  this->height_ = 240;
}

//   35_TFT display
void ILI9XXXILI9481::initialize() {
  this->init_lcd_(INITCMD_ILI9481);
  this->width_ = 480;
  this->height_ = 320;
}

//   35_TFT display
void ILI9XXXILI9486::initialize() {
  this->init_lcd_(INITCMD_ILI9486);
  this->width_ = 480;
  this->height_ = 320;
}
//    40_TFT display
void ILI9XXXILI9488::initialize() {
  this->init_lcd_(INITCMD_ILI9488);
  this->width_ = 480;
  this->height_ = 320;
}
//    40_TFT display
void ILI9XXXST7796::initialize() {
  this->init_lcd_(INITCMD_ST7796);
  this->width_ = 480;
  this->height_ = 320;
}

}  // namespace ili9xxx
}  // namespace esphome
