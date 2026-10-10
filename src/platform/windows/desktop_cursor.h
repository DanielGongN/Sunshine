/**
 * @file src/platform/windows/desktop_cursor.h
 * @brief Maps relative mouse movement to absolute virtual-desktop coordinates.
 */
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>

namespace platf::detail {
  struct desktop_cursor_bounds_t {
    int left {};
    int top {};
    int width {};
    int height {};

    bool operator==(const desktop_cursor_bounds_t &) const = default;

    [[nodiscard]] bool valid() const {
      return width > 0 && height > 0;
    }
  };

  struct desktop_cursor_position_t {
    int x;
    int y;
    int normalized_x;
    int normalized_y;
  };

  class desktop_cursor_mapper_t {
  public:
    using clock_t = std::chrono::steady_clock;

    [[nodiscard]] bool needs_sync(const desktop_cursor_bounds_t &bounds, clock_t::time_point now) const {
      return !initialized_ || bounds_ != bounds || now - last_event_ >= std::chrono::milliseconds {100};
    }

    void sync(const desktop_cursor_bounds_t &bounds, int x, int y) {
      if (!bounds.valid()) {
        reset();
        return;
      }

      bounds_ = bounds;
      target_x_ = std::clamp(static_cast<double>(x), static_cast<double>(bounds.left), right(bounds));
      target_y_ = std::clamp(static_cast<double>(y), static_cast<double>(bounds.top), bottom(bounds));
      initialized_ = true;
    }

    [[nodiscard]] std::optional<desktop_cursor_position_t> advance(const desktop_cursor_bounds_t &bounds, double delta_x, double delta_y, clock_t::time_point now) {
      if (!initialized_ || !bounds.valid() || bounds_ != bounds) {
        return std::nullopt;
      }

      target_x_ = std::clamp(target_x_ + delta_x, static_cast<double>(bounds.left), right(bounds));
      target_y_ = std::clamp(target_y_ + delta_y, static_cast<double>(bounds.top), bottom(bounds));
      last_event_ = now;

      const int pixel_x = static_cast<int>(std::lround(target_x_));
      const int pixel_y = static_cast<int>(std::lround(target_y_));
      return desktop_cursor_position_t {
        pixel_x,
        pixel_y,
        normalize(pixel_x - static_cast<double>(bounds.left), bounds.width),
        normalize(pixel_y - static_cast<double>(bounds.top), bounds.height),
      };
    }

    void reset() {
      initialized_ = false;
    }

  private:
    [[nodiscard]] static double right(const desktop_cursor_bounds_t &bounds) {
      return static_cast<double>(bounds.left) + bounds.width - 1;
    }

    [[nodiscard]] static double bottom(const desktop_cursor_bounds_t &bounds) {
      return static_cast<double>(bounds.top) + bounds.height - 1;
    }

    [[nodiscard]] static int normalize(double offset, int extent) {
      if (extent <= 1) {
        return 0;
      }
      return static_cast<int>(std::lround(offset * 65535.0 / (extent - 1)));
    }

    desktop_cursor_bounds_t bounds_ {};
    double target_x_ {};
    double target_y_ {};
    clock_t::time_point last_event_ {};
    bool initialized_ {};
  };
}  // namespace platf::detail
