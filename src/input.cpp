/**
 * @file src/input.cpp
 * @brief Definitions for gamepad, keyboard, and mouse input handling.
 */
#include <cstdint>
extern "C" {
#include <moonlight-common-c/src/Input.h>
#include <moonlight-common-c/src/Limelight.h>
}

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

// lib includes
#include <boost/endian/buffers.hpp>

// local includes
#include "config.h"
#include "globals.h"
#include "input.h"
#include "logging.h"
#include "platform/common.h"
#include "thread_pool.h"
#include "utility.h"

// Win32 WHEEL_DELTA constant
#ifndef WHEEL_DELTA
constexpr int WHEEL_DELTA = 120;
#endif

using namespace std::literals;

namespace input {

  constexpr auto MAX_GAMEPADS = std::min((std::size_t) platf::MAX_GAMEPADS, sizeof(std::int16_t) * 8);
  constexpr auto VKEY_SHIFT = 0x10;
  constexpr auto VKEY_LSHIFT = 0xA0;
  constexpr auto VKEY_RSHIFT = 0xA1;
  constexpr auto VKEY_CONTROL = 0x11;
  constexpr auto VKEY_LCONTROL = 0xA2;
  constexpr auto VKEY_RCONTROL = 0xA3;
  constexpr auto VKEY_MENU = 0x12;
  constexpr auto VKEY_LMENU = 0xA4;
  constexpr auto VKEY_RMENU = 0xA5;

  enum class button_state_e {
    NONE,  ///< No button state
    DOWN,  ///< Button is down
    UP  ///< Button is up
  };

  template<std::size_t N>
  int alloc_id(std::bitset<N> &gamepad_mask) {
    for (int x = 0; x < gamepad_mask.size(); ++x) {
      if (!gamepad_mask[x]) {
        gamepad_mask[x] = true;
        return x;
      }
    }

    return -1;
  }

  template<std::size_t N>
  void free_id(std::bitset<N> &gamepad_mask, int id) {
    gamepad_mask[id] = false;
  }

  typedef uint32_t key_press_id_t;

  key_press_id_t make_kpid(uint16_t vk, uint8_t flags) {
    return (key_press_id_t) vk << 8 | flags;
  }

  uint16_t vk_from_kpid(key_press_id_t kpid) {
    return kpid >> 8;
  }

  uint8_t flags_from_kpid(key_press_id_t kpid) {
    return kpid & 0xFF;
  }

  /**
   * @brief Convert a little-endian netfloat to a native endianness float.
   * @param f Netfloat value.
   * @return The native endianness float value.
   */
  float from_netfloat(netfloat f) {
    return boost::endian::endian_load<float, sizeof(float), boost::endian::order::little>(f);
  }

  /**
   * @brief Convert a little-endian netfloat to a native endianness float and clamps it.
   * @param f Netfloat value.
   * @param min The minimium value for clamping.
   * @param max The maximum value for clamping.
   * @return Clamped native endianess float value.
   */
  float from_clamped_netfloat(netfloat f, float min, float max) {
    return std::clamp(from_netfloat(f), min, max);
  }

  static task_pool_util::TaskPool::task_id_t key_press_repeat_id {};
  static std::unordered_map<key_press_id_t, bool> key_press {};
  static std::array<std::uint8_t, 5> mouse_press {};

  static platf::input_t platf_input;
  std::bitset<platf::MAX_GAMEPADS> gamepadMask {};
  std::bitset<platf::MAX_GAMEPADS> preinitializedGamepadMask {};

  constexpr auto INPUT_STATS_LOG_INTERVAL = std::chrono::seconds(1);
  constexpr auto INPUT_GAMEPAD_LOG_INTERVAL = std::chrono::seconds(1);
  constexpr std::size_t INPUT_QUEUE_WARN_PACKETS = 128;
  constexpr std::size_t INPUT_DRAIN_MAX_MESSAGES = 256;
  constexpr auto SYNTHETIC_HOME_HOLD = 100ms;
  constexpr auto CLICK_GAMEPAD_HOLD = 200ms;

  struct input_stats_t {
    std::chrono::steady_clock::time_point last_stats_log;
    std::chrono::steady_clock::time_point last_gamepad_log;
    std::uint64_t queued_messages {};
    std::uint64_t processed_messages {};
    std::uint64_t queued_bytes {};
    std::uint64_t processed_bytes {};
    std::uint64_t keyboard_packets {};
    std::uint64_t mouse_move_packets {};
    std::uint64_t mouse_button_packets {};
    std::uint64_t scroll_packets {};
    std::uint64_t unicode_packets {};
    std::uint64_t gamepad_packets {};
    std::uint64_t touch_packets {};
    std::uint64_t motion_packets {};
    std::uint64_t battery_packets {};
    std::uint64_t arrival_packets {};
    std::uint64_t unknown_packets {};
    std::size_t queue_peak {};
    bool has_last_gamepad {};
    std::uint16_t last_gamepad_controller {};
    std::uint16_t last_gamepad_active_mask {};
    std::uint32_t last_gamepad_buttons {};
    std::uint8_t last_gamepad_lt {};
    std::uint8_t last_gamepad_rt {};
    std::int16_t last_gamepad_lsx {};
    std::int16_t last_gamepad_lsy {};
    std::int16_t last_gamepad_rsx {};
    std::int16_t last_gamepad_rsy {};
  };

  static input_stats_t input_stats;
  static std::mutex input_stats_lock;

  std::string_view input_packet_type_name(std::uint32_t magic) {
    switch (magic) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return "MOUSE_MOVE_REL"sv;
      case MOUSE_MOVE_ABS_MAGIC:
        return "MOUSE_MOVE_ABS"sv;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
        return "MOUSE_BUTTON_DOWN"sv;
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        return "MOUSE_BUTTON_UP"sv;
      case SCROLL_MAGIC_GEN5:
        return "SCROLL"sv;
      case SS_HSCROLL_MAGIC:
        return "HSCROLL"sv;
      case KEY_DOWN_EVENT_MAGIC:
        return "KEY_DOWN"sv;
      case KEY_UP_EVENT_MAGIC:
        return "KEY_UP"sv;
      case UTF8_TEXT_EVENT_MAGIC:
        return "UTF8_TEXT"sv;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return "MULTI_CONTROLLER"sv;
      case SS_TOUCH_MAGIC:
        return "TOUCH"sv;
      case SS_PEN_MAGIC:
        return "PEN"sv;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        return "CONTROLLER_ARRIVAL"sv;
      case SS_CONTROLLER_TOUCH_MAGIC:
        return "CONTROLLER_TOUCH"sv;
      case SS_CONTROLLER_MOTION_MAGIC:
        return "CONTROLLER_MOTION"sv;
      case SS_CONTROLLER_BATTERY_MAGIC:
        return "CONTROLLER_BATTERY"sv;
      default:
        return "UNKNOWN"sv;
    }
  }

  void log_input_stats_locked(std::chrono::steady_clock::time_point now) {
    if (input_stats.last_stats_log.time_since_epoch().count() != 0 && now - input_stats.last_stats_log < INPUT_STATS_LOG_INTERVAL) {
      return;
    }

    input_stats.last_stats_log = now;
    BOOST_LOG(info) << "[input][stats] queued="sv << input_stats.queued_messages
                    << " processed="sv << input_stats.processed_messages
                    << " queue_peak="sv << input_stats.queue_peak
                    << " bytes(queued/processed)="sv << input_stats.queued_bytes << "/"sv << input_stats.processed_bytes
                    << " keyboard="sv << input_stats.keyboard_packets
                    << " mouse_move="sv << input_stats.mouse_move_packets
                    << " mouse_button="sv << input_stats.mouse_button_packets
                    << " scroll="sv << input_stats.scroll_packets
                    << " unicode="sv << input_stats.unicode_packets
                    << " gamepad="sv << input_stats.gamepad_packets
                    << " touch="sv << input_stats.touch_packets
                    << " motion="sv << input_stats.motion_packets
                    << " battery="sv << input_stats.battery_packets
                    << " arrival="sv << input_stats.arrival_packets
                    << " unknown="sv << input_stats.unknown_packets;
  }

  void note_input_queued(std::size_t bytes, std::size_t queue_size) {
    std::lock_guard<std::mutex> lg(input_stats_lock);
    ++input_stats.queued_messages;
    input_stats.queued_bytes += bytes;
    input_stats.queue_peak = std::max(input_stats.queue_peak, queue_size);
    if (queue_size == INPUT_QUEUE_WARN_PACKETS) {
      BOOST_LOG(warning) << "[input][queue] queued input backlog reached "sv << queue_size << " packets"sv;
    }
    log_input_stats_locked(std::chrono::steady_clock::now());
  }

  void note_input_processed(std::uint32_t magic, std::size_t bytes, std::size_t queue_size_after_batch, std::size_t batched_entries) {
    std::lock_guard<std::mutex> lg(input_stats_lock);
    ++input_stats.processed_messages;
    input_stats.processed_bytes += bytes;
    input_stats.queue_peak = std::max(input_stats.queue_peak, queue_size_after_batch);

    switch (magic) {
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        ++input_stats.keyboard_packets;
        break;
      case MOUSE_MOVE_REL_MAGIC_GEN5:
      case MOUSE_MOVE_ABS_MAGIC:
        ++input_stats.mouse_move_packets;
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        ++input_stats.mouse_button_packets;
        break;
      case SCROLL_MAGIC_GEN5:
      case SS_HSCROLL_MAGIC:
        ++input_stats.scroll_packets;
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        ++input_stats.unicode_packets;
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        ++input_stats.gamepad_packets;
        break;
      case SS_TOUCH_MAGIC:
      case SS_PEN_MAGIC:
      case SS_CONTROLLER_TOUCH_MAGIC:
        ++input_stats.touch_packets;
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        ++input_stats.motion_packets;
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        ++input_stats.battery_packets;
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        ++input_stats.arrival_packets;
        break;
      default:
        ++input_stats.unknown_packets;
        BOOST_LOG(warning) << "[input][unknown] magic=0x"sv << util::hex(magic).to_string_view()
                           << " type="sv << input_packet_type_name(magic)
                           << " payload_bytes="sv << bytes
                           << " queue_after_batch="sv << queue_size_after_batch
                           << " batched="sv << batched_entries;
        break;
    }

    log_input_stats_locked(std::chrono::steady_clock::now());
  }

  void note_gamepad_packet(PNV_MULTI_CONTROLLER_PACKET packet, std::size_t queue_size_after_batch, std::size_t batched_entries) {
    auto now = std::chrono::steady_clock::now();
    auto controller = static_cast<std::uint16_t>(packet->controllerNumber);
    auto active_mask = static_cast<std::uint16_t>(packet->activeGamepadMask);
    auto button_flags = static_cast<std::uint32_t>(static_cast<std::uint16_t>(packet->buttonFlags)) |
                        (static_cast<std::uint32_t>(static_cast<std::uint16_t>(packet->buttonFlags2)) << 16);
    auto lt = static_cast<std::uint8_t>(packet->leftTrigger);
    auto rt = static_cast<std::uint8_t>(packet->rightTrigger);
    auto lsx = static_cast<std::int16_t>(packet->leftStickX);
    auto lsy = static_cast<std::int16_t>(packet->leftStickY);
    auto rsx = static_cast<std::int16_t>(packet->rightStickX);
    auto rsy = static_cast<std::int16_t>(packet->rightStickY);

    std::lock_guard<std::mutex> lg(input_stats_lock);
    auto buttons_changed = !input_stats.has_last_gamepad ||
                           input_stats.last_gamepad_controller != controller ||
                           input_stats.last_gamepad_active_mask != active_mask ||
                           input_stats.last_gamepad_buttons != button_flags;
    auto timed_log = input_stats.last_gamepad_log.time_since_epoch().count() == 0 || now - input_stats.last_gamepad_log >= INPUT_GAMEPAD_LOG_INTERVAL;

    if (buttons_changed || timed_log) {
      input_stats.last_gamepad_log = now;
      BOOST_LOG(info) << "[input][gamepad] controller="sv << controller
                      << " active_mask=0x"sv << util::hex(active_mask).to_string_view()
                      << " buttons=0x"sv << util::hex(button_flags).to_string_view()
                      << " lt="sv << static_cast<std::uint32_t>(lt)
                      << " rt="sv << static_cast<std::uint32_t>(rt)
                      << " ls=("sv << lsx << ","sv << lsy << ")"sv
                      << " rs=("sv << rsx << ","sv << rsy << ")"sv
                      << " queue_after_batch="sv << queue_size_after_batch
                      << " batched="sv << batched_entries;
    }

    input_stats.has_last_gamepad = true;
    input_stats.last_gamepad_controller = controller;
    input_stats.last_gamepad_active_mask = active_mask;
    input_stats.last_gamepad_buttons = button_flags;
    input_stats.last_gamepad_lt = lt;
    input_stats.last_gamepad_rt = rt;
    input_stats.last_gamepad_lsx = lsx;
    input_stats.last_gamepad_lsy = lsy;
    input_stats.last_gamepad_rsx = rsx;
    input_stats.last_gamepad_rsy = rsy;
  }

  bool has_gamepad_index(int id) {
    return id >= 0 && static_cast<std::size_t>(id) < gamepadMask.size();
  }

  bool is_preinitialized_gamepad(int id) {
    return config::sunshine.middleware.gamepad_preinit && has_gamepad_index(id) && preinitializedGamepadMask[id];
  }

  void free_gamepad(platf::input_t &platf_input, int id, const platf::feedback_queue_t &feedback_queue, bool preinitialized) {
    // Reset gamepad state to neutral
    platf::gamepad_update(platf_input, id, platf::gamepad_state_t {});

    // Pre-created gamepads are persistent and must not be removed from ViGEm.
    if (preinitialized) {
      platf::free_gamepad(platf_input, id, feedback_queue);
      return;
    }

    platf::free_gamepad(platf_input, id, feedback_queue);

    free_id(gamepadMask, id);
  }

  bool same_gamepad_state(const platf::gamepad_state_t &lhs, const platf::gamepad_state_t &rhs) {
    return lhs.buttonFlags == rhs.buttonFlags &&
           lhs.lt == rhs.lt &&
           lhs.rt == rhs.rt &&
           lhs.lsX == rhs.lsX &&
           lhs.lsY == rhs.lsY &&
           lhs.rsX == rhs.rsX &&
           lhs.rsY == rhs.rsY;
  }

  bool detail::is_changed_active_gamepad_state(
    bool active,
    const platf::gamepad_state_t &old_state,
    const platf::gamepad_state_t &new_state
  ) {
    return active && !same_gamepad_state(old_state, new_state);
  }

  struct gamepad_t {
    gamepad_t():
        gamepad_state {},
        back_timeout_id {},
        home_release_id {},
        feedback_queue {},
        id {-1},
        preinitialized {},
        generation {},
        back_button_state {button_state_e::NONE},
        synthetic_home_button_down {} {
    }

    ~gamepad_t() {
      task_pool.cancel(back_timeout_id);
      task_pool.cancel(home_release_id);
      if (id >= 0) {
        task_pool.push([id = this->id, feedback_queue = this->feedback_queue, preinitialized = this->preinitialized]() {
          free_gamepad(platf_input, id, feedback_queue, preinitialized);
        });
      }
    }

    platf::gamepad_state_t gamepad_state;

    thread_pool_util::ThreadPool::task_id_t back_timeout_id;
    thread_pool_util::ThreadPool::task_id_t home_release_id;

    platf::feedback_queue_t feedback_queue;

    int id;

    bool preinitialized;

    std::uint64_t generation;

    // When emulating the HOME button, we may need to artificially release the back button.
    // Afterwards, the gamepad state on sunshine won't match the state on Moonlight.
    // To prevent Sunshine from sending erroneous input data to the active application,
    // Sunshine forces the button to be in a specific state until the gamepad state matches that of
    // Moonlight once more.
    button_state_e back_button_state;

    // Keep a synthetic HOME press visible while its delayed release is pending.
    bool synthetic_home_button_down;
  };

  struct input_t {
    enum shortkey_e {
      CTRL = 0x1,  ///< Control key
      ALT = 0x2,  ///< Alt key
      SHIFT = 0x4,  ///< Shift key
      SHORTCUT = CTRL | ALT | SHIFT  ///< Shortcut combination
    };

    input_t(
      safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event,
      platf::feedback_queue_t feedback_queue
    ):
        shortcutFlags {},
        gamepads(MAX_GAMEPADS),
        client_context {platf::allocate_client_input_context(platf_input)},
        touch_port_event {std::move(touch_port_event)},
        feedback_queue {std::move(feedback_queue)},
        input_queue_drain_active {},
        last_input_time {std::chrono::steady_clock::now()},
        touch_port {{0, 0, 0, 0}, 0, 0, 1.0f, 1.0f, 0, 0},
        accumulated_vscroll_delta {},
        accumulated_hscroll_delta {} {
    }

    // Keep track of alt+ctrl+shift key combo
    int shortcutFlags;

    std::vector<gamepad_t> gamepads;
    std::unique_ptr<platf::client_input_t> client_context;

    safe::mail_raw_t::event_t<input::touch_port_t> touch_port_event;
    platf::feedback_queue_t feedback_queue;

    std::list<std::vector<uint8_t>> input_queue;
    std::mutex input_queue_lock;
    bool input_queue_drain_active;

    std::atomic<std::chrono::steady_clock::time_point> last_input_time;

    input::touch_port_t touch_port;

    int32_t accumulated_vscroll_delta;
    int32_t accumulated_hscroll_delta;
  };

  bool has_client_gamepad_index(const input_t &input, int id) {
    return id >= 0 && static_cast<std::size_t>(id) < input.gamepads.size();
  }

  void cancel_pending_gamepad_timers(gamepad_t &gamepad) {
    task_pool.cancel(gamepad.back_timeout_id);
    task_pool.cancel(gamepad.home_release_id);
    gamepad.back_timeout_id = nullptr;
    gamepad.home_release_id = nullptr;
    gamepad.back_button_state = button_state_e::NONE;
    gamepad.synthetic_home_button_down = false;
    ++gamepad.generation;
  }

  bool is_expected_gamepad(const gamepad_t &gamepad, int expected_id, std::uint64_t expected_generation) {
    return gamepad.id == expected_id && gamepad.generation == expected_generation;
  }

  /**
   * @brief Apply shortcut based on VKEY
   * @param keyCode The VKEY code
   * @return 0 if no shortcut applied, > 0 if shortcut applied.
   */
  inline int apply_shortcut(short keyCode) {
    constexpr auto VK_F1 = 0x70;
    constexpr auto VK_F13 = 0x7C;

    BOOST_LOG(debug) << "Apply Shortcut: 0x"sv << util::hex((std::uint8_t) keyCode).to_string_view();

    if (keyCode >= VK_F1 && keyCode <= VK_F13) {
      mail::man->event<int>(mail::switch_display)->raise(keyCode - VK_F1);
      return 1;
    }

    switch (keyCode) {
      case 0x4E /* VKEY_N */:
        display_cursor = !display_cursor;
        return 1;
    }

    return 0;
  }

  void print(PNV_REL_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin relative mouse move packet--"sv << std::endl
      << "deltaX ["sv << util::endian::big(packet->deltaX) << ']' << std::endl
      << "deltaY ["sv << util::endian::big(packet->deltaY) << ']' << std::endl
      << "--end relative mouse move packet--"sv;
  }

  void print(PNV_ABS_MOUSE_MOVE_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin absolute mouse move packet--"sv << std::endl
      << "x      ["sv << util::endian::big(packet->x) << ']' << std::endl
      << "y      ["sv << util::endian::big(packet->y) << ']' << std::endl
      << "width  ["sv << util::endian::big(packet->width) << ']' << std::endl
      << "height ["sv << util::endian::big(packet->height) << ']' << std::endl
      << "--end absolute mouse move packet--"sv;
  }

  void print(PNV_MOUSE_BUTTON_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse button packet--"sv << std::endl
      << "action ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "button ["sv << util::hex(packet->button).to_string_view() << ']' << std::endl
      << "--end mouse button packet--"sv;
  }

  void print(PNV_SCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse scroll packet--"sv << std::endl
      << "scrollAmt1 ["sv << util::endian::big(packet->scrollAmt1) << ']' << std::endl
      << "--end mouse scroll packet--"sv;
  }

  void print(PSS_HSCROLL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin mouse hscroll packet--"sv << std::endl
      << "scrollAmount ["sv << util::endian::big(packet->scrollAmount) << ']' << std::endl
      << "--end mouse hscroll packet--"sv;
  }

  void print(PNV_KEYBOARD_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin keyboard packet--"sv << std::endl
      << "keyAction ["sv << util::hex(packet->header.magic).to_string_view() << ']' << std::endl
      << "keyCode ["sv << util::hex(packet->keyCode).to_string_view() << ']' << std::endl
      << "modifiers ["sv << util::hex(packet->modifiers).to_string_view() << ']' << std::endl
      << "flags ["sv << util::hex(packet->flags).to_string_view() << ']' << std::endl
      << "--end keyboard packet--"sv;
  }

  void print(PNV_UNICODE_PACKET packet) {
    std::string text(packet->text, util::endian::big(packet->header.size) - sizeof(packet->header.magic));
    BOOST_LOG(debug)
      << "--begin unicode packet--"sv << std::endl
      << "text ["sv << text << ']' << std::endl
      << "--end unicode packet--"sv;
  }

  void print(PNV_MULTI_CONTROLLER_PACKET packet) {
    // Moonlight spams controller packet even when not necessary
    BOOST_LOG(verbose)
      << "--begin controller packet--"sv << std::endl
      << "controllerNumber ["sv << packet->controllerNumber << ']' << std::endl
      << "activeGamepadMask ["sv << util::hex(packet->activeGamepadMask).to_string_view() << ']' << std::endl
      << "buttonFlags ["sv << util::hex((uint32_t) packet->buttonFlags | (packet->buttonFlags2 << 16)).to_string_view() << ']' << std::endl
      << "leftTrigger ["sv << util::hex(packet->leftTrigger).to_string_view() << ']' << std::endl
      << "rightTrigger ["sv << util::hex(packet->rightTrigger).to_string_view() << ']' << std::endl
      << "leftStickX ["sv << packet->leftStickX << ']' << std::endl
      << "leftStickY ["sv << packet->leftStickY << ']' << std::endl
      << "rightStickX ["sv << packet->rightStickX << ']' << std::endl
      << "rightStickY ["sv << packet->rightStickY << ']' << std::endl
      << "--end controller packet--"sv;
  }

  /**
   * @brief Prints a touch packet.
   * @param packet The touch packet.
   */
  void print(PSS_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin touch packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "--end touch packet--"sv;
  }

  /**
   * @brief Prints a pen packet.
   * @param packet The pen packet.
   */
  void print(PSS_PEN_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin pen packet--"sv << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "toolType ["sv << util::hex(packet->toolType).to_string_view() << ']' << std::endl
      << "penButtons ["sv << util::hex(packet->penButtons).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressureOrDistance ["sv << from_netfloat(packet->pressureOrDistance) << ']' << std::endl
      << "contactAreaMajor ["sv << from_netfloat(packet->contactAreaMajor) << ']' << std::endl
      << "contactAreaMinor ["sv << from_netfloat(packet->contactAreaMinor) << ']' << std::endl
      << "rotation ["sv << (uint32_t) packet->rotation << ']' << std::endl
      << "tilt ["sv << (uint32_t) packet->tilt << ']' << std::endl
      << "--end pen packet--"sv;
  }

  /**
   * @brief Prints a controller arrival packet.
   * @param packet The controller arrival packet.
   */
  void print(PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller arrival packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "type ["sv << util::hex(packet->type).to_string_view() << ']' << std::endl
      << "capabilities ["sv << util::hex(packet->capabilities).to_string_view() << ']' << std::endl
      << "supportedButtonFlags ["sv << util::hex(packet->supportedButtonFlags).to_string_view() << ']' << std::endl
      << "--end controller arrival packet--"sv;
  }

  /**
   * @brief Prints a controller touch packet.
   * @param packet The controller touch packet.
   */
  void print(PSS_CONTROLLER_TOUCH_PACKET packet) {
    BOOST_LOG(debug)
      << "--begin controller touch packet--"sv << std::endl
      << "controllerNumber ["sv << (uint32_t) packet->controllerNumber << ']' << std::endl
      << "eventType ["sv << util::hex(packet->eventType).to_string_view() << ']' << std::endl
      << "pointerId ["sv << util::hex(packet->pointerId).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "pressure ["sv << from_netfloat(packet->pressure) << ']' << std::endl
      << "--end controller touch packet--"sv;
  }

  /**
   * @brief Prints a controller motion packet.
   * @param packet The controller motion packet.
   */
  void print(PSS_CONTROLLER_MOTION_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller motion packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "motionType ["sv << util::hex(packet->motionType).to_string_view() << ']' << std::endl
      << "x ["sv << from_netfloat(packet->x) << ']' << std::endl
      << "y ["sv << from_netfloat(packet->y) << ']' << std::endl
      << "z ["sv << from_netfloat(packet->z) << ']' << std::endl
      << "--end controller motion packet--"sv;
  }

  /**
   * @brief Prints a controller battery packet.
   * @param packet The controller battery packet.
   */
  void print(PSS_CONTROLLER_BATTERY_PACKET packet) {
    BOOST_LOG(verbose)
      << "--begin controller battery packet--"sv << std::endl
      << "controllerNumber ["sv << util::hex(packet->controllerNumber).to_string_view() << ']' << std::endl
      << "batteryState ["sv << util::hex(packet->batteryState).to_string_view() << ']' << std::endl
      << "batteryPercentage ["sv << util::hex(packet->batteryPercentage).to_string_view() << ']' << std::endl
      << "--end controller battery packet--"sv;
  }

  void print(void *payload) {
    auto header = (PNV_INPUT_HEADER) payload;

    switch (util::endian::little(header->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        print((PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        print((PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        print((PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        print((PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        print((PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        print((PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        print((PNV_UNICODE_PACKET) payload);
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        print((PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
      case SS_TOUCH_MAGIC:
        print((PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        print((PSS_PEN_PACKET) payload);
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        print((PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        print((PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        print((PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        print((PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
    }
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_REL_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto delta_x = util::endian::big(packet->deltaX);
    auto delta_y = util::endian::big(packet->deltaY);
    if (delta_x != 0 || delta_y != 0) {
      update_input_time(input);
    }

    platf::move_mouse(platf_input, delta_x, delta_y);
  }

  /**
   * @brief Converts client coordinates on the specified surface into screen coordinates.
   * @param input The input context.
   * @param val The cartesian coordinate pair to convert.
   * @param size The size of the client's surface containing the value.
   * @return The host-relative coordinate pair if a touchport is available.
   */
  std::optional<std::pair<float, float>> client_to_touchport(std::shared_ptr<input_t> &input, const std::pair<float, float> &val, const std::pair<float, float> &size) {
    auto &touch_port_event = input->touch_port_event;
    auto &touch_port = input->touch_port;
    if (touch_port_event->peek()) {
      touch_port = *touch_port_event->pop();
    }
    if (!touch_port) {
      BOOST_LOG(verbose) << "Ignoring early absolute input without a touch port"sv;
      return std::nullopt;
    }

    auto scalarX = touch_port.width / size.first;
    auto scalarY = touch_port.height / size.second;

    float x = std::clamp(val.first, 0.0f, size.first) * scalarX;
    float y = std::clamp(val.second, 0.0f, size.second) * scalarY;

    auto offsetX = touch_port.client_offsetX;
    auto offsetY = touch_port.client_offsetY;

    x = std::clamp(x, offsetX, (size.first * scalarX) - offsetX);
    y = std::clamp(y, offsetY, (size.second * scalarY) - offsetY);

    /*
    x and y here below have the coordinates of the surface of the streaming resolution,
    and are dependent on how that comes configured from the client (scalar_inv is calculated
    from the proportion of that and the device's **physical** size).
    */
    x = (x - offsetX) * touch_port.scalar_inv;
    y = (y - offsetY) * touch_port.scalar_inv;

    /*
    This final operation is a bit weird and has been brought about with lots of trial and error. A better
    way to do this may exist.

    Basically, this is what makes the touchscreen map to the coordinates inputtino expects properly.
    Since inputtino's dimensions are now logical (because scaling breaks everything otherwise), using the previous
    x and y coordinates would be incorrect when screens are scaled, because the touch port is smaller (or larger)
    by a factor (that factor is touch_port.scalar_tpcoords), and that factor must be used to account for that difference
    when moving the cursor. Otherwise, it will move either slower or faster than your finger proportionally to
    scalar_tpcoords, and be offset *inversely* proportionally to scalar_tpcoords. So you must account for both differences
    by multiplying and dividing.
    */
    float final_x = (x + touch_port.offset_x * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    float final_y = (y + touch_port.offset_y * touch_port.scalar_tpcoords) / touch_port.scalar_tpcoords;
    return std::pair {final_x, final_y};
  }

  /**
   * @brief Multiply a polar coordinate pair by a cartesian scaling factor.
   * @param r The radial coordinate.
   * @param angle The angular coordinate (radians).
   * @param scalar The scalar cartesian coordinate pair.
   * @return The scaled radial coordinate.
   */
  float multiply_polar_by_cartesian_scalar(float r, float angle, const std::pair<float, float> &scalar) {
    // Convert polar to cartesian coordinates
    float x = r * std::cos(angle);
    float y = r * std::sin(angle);

    // Scale the values
    x *= scalar.first;
    y *= scalar.second;

    // Convert the result back to a polar radial coordinate
    return std::sqrt(std::pow(x, 2) + std::pow(y, 2));
  }

  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar) {
    // If the rotation is unknown, we'll just scale both axes equally by using
    // a 45-degree angle for our scaling calculations
    float angle = rotation == LI_ROT_UNKNOWN ? (M_PI / 4) : (rotation * (M_PI / 180));

    // If we have a major but not a minor axis, treat the touch as circular
    float major = val.first;
    float minor = val.second != 0.0f ? val.second : val.first;

    // The minor axis is perpendicular to major axis so the angle must be rotated by 90 degrees
    return {multiply_polar_by_cartesian_scalar(major, angle, scalar), multiply_polar_by_cartesian_scalar(minor, angle + (M_PI / 2), scalar)};
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_ABS_MOUSE_MOVE_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    float x = util::endian::big(packet->x);
    float y = util::endian::big(packet->y);

    // Prevent divide by zero
    // Don't expect it to happen, but just in case
    if (!packet->width || !packet->height) {
      BOOST_LOG(warning) << "Moonlight passed invalid dimensions"sv;

      return;
    }

    auto width = (float) util::endian::big(packet->width);
    auto height = (float) util::endian::big(packet->height);

    auto tpcoords = client_to_touchport(input, {x, y}, {width, height});
    if (!tpcoords) {
      return;
    }

    auto &touch_port = input->touch_port;

    int touch_port_dim_x;
    int touch_port_dim_y;
    if (touch_port.env_logical_width != 0 && touch_port.env_logical_height != 0) {
      touch_port_dim_x = touch_port.env_logical_width;
      touch_port_dim_y = touch_port.env_logical_height;
    } else {
      touch_port_dim_x = touch_port.env_width;
      touch_port_dim_y = touch_port.env_height;
    }

    platf::touch_port_t abs_port {
      touch_port.offset_x,
      touch_port.offset_y,
      touch_port_dim_x,
      touch_port_dim_y
    };

    update_input_time(input);
    platf::abs_mouse(platf_input, abs_port, tpcoords->first, tpcoords->second);
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_MOUSE_BUTTON_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5;
    auto button = util::endian::big(packet->button);
    if (button > 0 && button < mouse_press.size()) {
      if (mouse_press[button] != release) {
        // button state is already what we want
        return;
      }

      mouse_press[button] = !release;
    }

    update_input_time(input);
    platf::button_mouse(platf_input, button, release);
  }

  short map_keycode(short keycode) {
    auto it = config::input.keybindings.find(keycode);
    if (it != std::end(config::input.keybindings)) {
      return it->second;
    }

    return keycode;
  }

  /**
   * @brief Update flags for keyboard shortcut combo's
   */
  inline void update_shortcutFlags(int *flags, short keyCode, bool release) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
        if (release) {
          *flags &= ~input_t::SHIFT;
        } else {
          *flags |= input_t::SHIFT;
        }
        break;
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
        if (release) {
          *flags &= ~input_t::CTRL;
        } else {
          *flags |= input_t::CTRL;
        }
        break;
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        if (release) {
          *flags &= ~input_t::ALT;
        } else {
          *flags |= input_t::ALT;
        }
        break;
    }
  }

  bool is_modifier(uint16_t keyCode) {
    switch (keyCode) {
      case VKEY_SHIFT:
      case VKEY_LSHIFT:
      case VKEY_RSHIFT:
      case VKEY_CONTROL:
      case VKEY_LCONTROL:
      case VKEY_RCONTROL:
      case VKEY_MENU:
      case VKEY_LMENU:
      case VKEY_RMENU:
        return true;
      default:
        return false;
    }
  }

  void send_key_and_modifiers(uint16_t key_code, bool release, uint8_t flags, uint8_t synthetic_modifiers) {
    if (!release) {
      // Press any synthetic modifiers required for this key
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, false, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, false, flags);
      }
    }

    platf::keyboard_update(platf_input, map_keycode(key_code), release, flags);

    if (!release) {
      // Raise any synthetic modifier keys we pressed
      if (synthetic_modifiers & MODIFIER_SHIFT) {
        platf::keyboard_update(platf_input, VKEY_SHIFT, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_CTRL) {
        platf::keyboard_update(platf_input, VKEY_CONTROL, true, flags);
      }
      if (synthetic_modifiers & MODIFIER_ALT) {
        platf::keyboard_update(platf_input, VKEY_MENU, true, flags);
      }
    }
  }

  void repeat_key(uint16_t key_code, uint8_t flags, uint8_t synthetic_modifiers) {
    // If key no longer pressed, stop repeating
    if (!key_press[make_kpid(key_code, flags)]) {
      key_press_repeat_id = nullptr;
      return;
    }

    send_key_and_modifiers(key_code, false, flags, synthetic_modifiers);

    key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_period, key_code, flags, synthetic_modifiers).task_id;
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_KEYBOARD_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    auto release = util::endian::little(packet->header.magic) == KEY_UP_EVENT_MAGIC;
    auto keyCode = packet->keyCode & 0x00FF;

    // Set synthetic modifier flags if the keyboard packet is requesting modifier
    // keys that are not current pressed.
    uint8_t synthetic_modifiers = 0;
    if (!release && !is_modifier(keyCode)) {
      if (!(input->shortcutFlags & input_t::SHIFT) && (packet->modifiers & MODIFIER_SHIFT)) {
        synthetic_modifiers |= MODIFIER_SHIFT;
      }
      if (!(input->shortcutFlags & input_t::CTRL) && (packet->modifiers & MODIFIER_CTRL)) {
        synthetic_modifiers |= MODIFIER_CTRL;
      }
      if (!(input->shortcutFlags & input_t::ALT) && (packet->modifiers & MODIFIER_ALT)) {
        synthetic_modifiers |= MODIFIER_ALT;
      }
    }

    auto &pressed = key_press[make_kpid(keyCode, packet->flags)];
    if (!pressed) {
      if (!release) {
        // A new key has been pressed down, we need to check for key combo's
        // If a key-combo has been pressed down, don't pass it through
        if (input->shortcutFlags == input_t::SHORTCUT && apply_shortcut(keyCode) > 0) {
          update_input_time(input);
          return;
        }

        if (key_press_repeat_id) {
          task_pool.cancel(key_press_repeat_id);
        }

        if (config::input.key_repeat_delay.count() > 0) {
          key_press_repeat_id = task_pool.pushDelayed(repeat_key, config::input.key_repeat_delay, keyCode, packet->flags, synthetic_modifiers).task_id;
        }
      } else {
        // Already released
        return;
      }
    } else if (!release) {
      // Already pressed down key
      return;
    }

    pressed = !release;

    update_input_time(input);
    send_key_and_modifiers(keyCode, release, packet->flags, synthetic_modifiers);

    update_shortcutFlags(&input->shortcutFlags, map_keycode(keyCode), release);
  }

  /**
   * @brief Called to pass a vertical scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PNV_SCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto scroll_amount = util::endian::big(packet->scrollAmt1);
    if (scroll_amount != 0) {
      update_input_time(input);
    }

    if (config::input.high_resolution_scrolling) {
      platf::scroll(platf_input, scroll_amount);
    } else {
      input->accumulated_vscroll_delta += scroll_amount;
      auto full_ticks = input->accumulated_vscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::scroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_vscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  /**
   * @brief Called to pass a horizontal scroll message the platform backend.
   * @param input The input context pointer.
   * @param packet The scroll packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_HSCROLL_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    auto scroll_amount = util::endian::big(packet->scrollAmount);
    if (scroll_amount != 0) {
      update_input_time(input);
    }

    if (config::input.high_resolution_scrolling) {
      platf::hscroll(platf_input, scroll_amount);
    } else {
      input->accumulated_hscroll_delta += scroll_amount;
      auto full_ticks = input->accumulated_hscroll_delta / WHEEL_DELTA;
      if (full_ticks) {
        // Send any full ticks that have accumulated and store the rest
        platf::hscroll(platf_input, full_ticks * WHEEL_DELTA);
        input->accumulated_hscroll_delta -= full_ticks * WHEEL_DELTA;
      }
    }
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_UNICODE_PACKET packet) {
    if (!config::input.keyboard) {
      return;
    }

    int size = util::endian::big(packet->header.size) - sizeof(packet->header.magic);
    if (size <= 0) {
      return;
    }

    update_input_time(input);
    platf::unicode(platf_input, packet->text, size);
  }

  /**
   * @brief Called to pass a controller arrival message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller arrival packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_ARRIVAL_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (!has_client_gamepad_index(*input, packet->controllerNumber)) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    if (input->gamepads[packet->controllerNumber].id >= 0) {
      BOOST_LOG(warning) << "ControllerNumber already allocated ["sv << packet->controllerNumber << ']';
      return;
    }

    platf::gamepad_arrival_t arrival {
      packet->type,
      util::endian::little(packet->capabilities),
      util::endian::little(packet->supportedButtonFlags),
    };

    // Reuse the pre-created gamepad when the controller number matches a pre-initialized slot.
    if (is_preinitialized_gamepad(packet->controllerNumber)) {
      if (platf::alloc_gamepad(platf_input, {packet->controllerNumber, packet->controllerNumber}, arrival, input->feedback_queue)) {
        return;
      }

      input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
      input->gamepads[packet->controllerNumber].feedback_queue = input->feedback_queue;
      input->gamepads[packet->controllerNumber].preinitialized = true;
      ++input->gamepads[packet->controllerNumber].generation;
      return;  // Pre-created gamepads already have metadata from vigem_t::init().
    }

    auto id = alloc_id(gamepadMask);
    if (id < 0) {
      return;
    }

    // Allocate a new gamepad
    if (platf::alloc_gamepad(platf_input, {id, packet->controllerNumber}, arrival, input->feedback_queue)) {
      free_id(gamepadMask, id);
      return;
    }

    input->gamepads[packet->controllerNumber].id = id;
    input->gamepads[packet->controllerNumber].feedback_queue = input->feedback_queue;
    input->gamepads[packet->controllerNumber].preinitialized = false;
    ++input->gamepads[packet->controllerNumber].generation;
  }

  /**
   * @brief Normalizes coordinates to monitor-local logical touch dimensions.
   * @param touch_port The current touch port metadata.
   * @param coords The in/out coordinate pair to normalize.
   * @return The monitor-local touch port, or std::nullopt if dimensions are invalid.
   */
  std::optional<platf::touch_port_t> monitor_touch_port(const input::touch_port_t &touch_port, std::pair<float, float> &coords) {
    const float monitor_logical_w = (touch_port.width * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    const float monitor_logical_h = (touch_port.height * touch_port.scalar_inv) / touch_port.scalar_tpcoords;
    if (monitor_logical_w <= 0.0f || monitor_logical_h <= 0.0f) {
      BOOST_LOG(warning) << "Ignoring touch/pen input due to invalid logical touch dimensions"sv;
      return std::nullopt;
    }

    coords.first = (coords.first - touch_port.offset_x) / monitor_logical_w;
    coords.second = (coords.second - touch_port.offset_y) / monitor_logical_h;

    return platf::touch_port_t {
      touch_port.offset_x,
      touch_port.offset_y,
      static_cast<int>(monitor_logical_w),
      static_cast<int>(monitor_logical_h)
    };
  }

  /**
   * @brief Called to pass a touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_TOUCH_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    // Convert the client normalized coordinates to touchport coordinates
    auto coords = client_to_touchport(input, {from_clamped_netfloat(packet->x, 0.0f, 1.0f) * 65535.f, from_clamped_netfloat(packet->y, 0.0f, 1.0f) * 65535.f}, {65535.f, 65535.f});
    if (!coords) {
      return;
    }

    auto &touch_port = input->touch_port;

    auto abs_port = monitor_touch_port(touch_port, *coords);
    if (!abs_port) {
      return;
    }

    // Normalize rotation value to 0-359 degree range
    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    // Normalize the contact area based on the touchport
    auto contact_area = scale_client_contact_area(
      {from_clamped_netfloat(packet->contactAreaMajor, 0.0f, 1.0f) * 65535.f,
       from_clamped_netfloat(packet->contactAreaMinor, 0.0f, 1.0f) * 65535.f},
      rotation,
      {abs_port->width / 65535.f, abs_port->height / 65535.f}
    );

    platf::touch_input_t touch {
      packet->eventType,
      rotation,
      util::endian::little(packet->pointerId),
      coords->first,
      coords->second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      contact_area.first,
      contact_area.second,
    };

    update_input_time(input);
    platf::touch_update(input->client_context.get(), *abs_port, touch);
  }

  /**
   * @brief Called to pass a pen message to the platform backend.
   * @param input The input context pointer.
   * @param packet The pen packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_PEN_PACKET packet) {
    if (!config::input.mouse) {
      return;
    }

    // Convert the client normalized coordinates to touchport coordinates
    auto coords = client_to_touchport(input, {from_clamped_netfloat(packet->x, 0.0f, 1.0f) * 65535.f, from_clamped_netfloat(packet->y, 0.0f, 1.0f) * 65535.f}, {65535.f, 65535.f});
    if (!coords) {
      return;
    }

    auto &touch_port = input->touch_port;

    auto abs_port = monitor_touch_port(touch_port, *coords);
    if (!abs_port) {
      return;
    }

    // Normalize rotation value to 0-359 degree range
    auto rotation = util::endian::little(packet->rotation);
    if (rotation != LI_ROT_UNKNOWN) {
      rotation %= 360;
    }

    // Normalize the contact area based on the touchport
    auto contact_area = scale_client_contact_area(
      {from_clamped_netfloat(packet->contactAreaMajor, 0.0f, 1.0f) * 65535.f,
       from_clamped_netfloat(packet->contactAreaMinor, 0.0f, 1.0f) * 65535.f},
      rotation,
      {abs_port->width / 65535.f, abs_port->height / 65535.f}
    );

    platf::pen_input_t pen {
      packet->eventType,
      packet->toolType,
      packet->penButtons,
      packet->tilt,
      rotation,
      coords->first,
      coords->second,
      from_clamped_netfloat(packet->pressureOrDistance, 0.0f, 1.0f),
      contact_area.first,
      contact_area.second,
    };

    update_input_time(input);
    platf::pen_update(input->client_context.get(), *abs_port, pen);
  }

  /**
   * @brief Called to pass a controller touch message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller touch packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_TOUCH_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (!has_client_gamepad_index(*input, packet->controllerNumber)) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_touch_t touch {
      {gamepad.id, packet->controllerNumber},
      packet->eventType,
      util::endian::little(packet->pointerId),
      from_clamped_netfloat(packet->x, 0.0f, 1.0f),
      from_clamped_netfloat(packet->y, 0.0f, 1.0f),
      from_clamped_netfloat(packet->pressure, 0.0f, 1.0f),
    };

    update_input_time(input);
    platf::gamepad_touch(platf_input, touch);
  }

  /**
   * @brief Called to pass a controller motion message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller motion packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_MOTION_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (!has_client_gamepad_index(*input, packet->controllerNumber)) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_motion_t motion {
      {gamepad.id, packet->controllerNumber},
      packet->motionType,
      from_netfloat(packet->x),
      from_netfloat(packet->y),
      from_netfloat(packet->z),
    };

    platf::gamepad_motion(platf_input, motion);
  }

  /**
   * @brief Called to pass a controller battery message to the platform backend.
   * @param input The input context pointer.
   * @param packet The controller battery packet.
   */
  void passthrough(std::shared_ptr<input_t> &input, PSS_CONTROLLER_BATTERY_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (!has_client_gamepad_index(*input, packet->controllerNumber)) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';
      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    platf::gamepad_battery_t battery {
      {gamepad.id, packet->controllerNumber},
      packet->batteryState,
      packet->batteryPercentage
    };

    platf::gamepad_battery(platf_input, battery);
  }

  void passthrough(std::shared_ptr<input_t> &input, PNV_MULTI_CONTROLLER_PACKET packet) {
    if (!config::input.controller) {
      return;
    }

    if (!has_client_gamepad_index(*input, packet->controllerNumber)) {
      BOOST_LOG(warning) << "ControllerNumber out of range ["sv << packet->controllerNumber << ']';

      return;
    }

    auto &gamepad = input->gamepads[packet->controllerNumber];

    // If this is an event for a new gamepad, create the gamepad now. Ideally, the client would
    // send a controller arrival instead of this but it's still supported for legacy clients.
    if ((packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id < 0) {
      // Reuse the pre-created gamepad when the controller number matches a pre-initialized slot.
      if (is_preinitialized_gamepad(packet->controllerNumber)) {
        if (platf::alloc_gamepad(platf_input, {packet->controllerNumber, (uint8_t) packet->controllerNumber}, {}, input->feedback_queue)) {
          return;
        }

        input->gamepads[packet->controllerNumber].id = packet->controllerNumber;
        input->gamepads[packet->controllerNumber].feedback_queue = input->feedback_queue;
        input->gamepads[packet->controllerNumber].preinitialized = true;
        ++input->gamepads[packet->controllerNumber].generation;
      } else {
        auto id = alloc_id(gamepadMask);
        if (id < 0) {
          return;
        }

        if (platf::alloc_gamepad(platf_input, {id, (uint8_t) packet->controllerNumber}, {}, input->feedback_queue)) {
          free_id(gamepadMask, id);
          return;
        }

        gamepad.id = id;
        gamepad.feedback_queue = input->feedback_queue;
        gamepad.preinitialized = false;
        ++gamepad.generation;
      }
    } else if (!(packet->activeGamepadMask & (1 << packet->controllerNumber)) && gamepad.id >= 0) {
      // If this is the final event for a gamepad being removed, free the gamepad and return.
      cancel_pending_gamepad_timers(gamepad);
      free_gamepad(platf_input, gamepad.id, gamepad.feedback_queue, gamepad.preinitialized);
      gamepad.feedback_queue.reset();
      gamepad.preinitialized = false;
      gamepad.id = -1;
      return;
    }

    // If this gamepad has not been initialized, ignore it.
    // This could happen when platf::alloc_gamepad fails
    if (gamepad.id < 0) {
      BOOST_LOG(warning) << "ControllerNumber ["sv << packet->controllerNumber << "] not allocated"sv;
      return;
    }

    std::uint16_t bf = packet->buttonFlags;
    std::uint32_t bf2 = packet->buttonFlags2;
    platf::gamepad_state_t gamepad_state {
      bf | (bf2 << 16),
      packet->leftTrigger,
      packet->rightTrigger,
      packet->leftStickX,
      packet->leftStickY,
      packet->rightStickX,
      packet->rightStickY
    };

    auto bf_new = gamepad_state.buttonFlags;
    switch (gamepad.back_button_state) {
      case button_state_e::UP:
        if (!(platf::BACK & bf_new)) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags &= ~platf::BACK;
        break;
      case button_state_e::DOWN:
        if (platf::BACK & bf_new) {
          gamepad.back_button_state = button_state_e::NONE;
        }
        gamepad_state.buttonFlags |= platf::BACK;
        break;
      case button_state_e::NONE:
        break;
    }

    if (gamepad.synthetic_home_button_down) {
      gamepad_state.buttonFlags |= platf::HOME;
    }

    auto button_flags_changed = gamepad_state.buttonFlags ^ gamepad.gamepad_state.buttonFlags;
    bf_new = gamepad_state.buttonFlags;

    auto active = (packet->activeGamepadMask & (1 << packet->controllerNumber)) != 0;
    if (!detail::is_changed_active_gamepad_state(active, gamepad.gamepad_state, gamepad_state)) {
      return;
    }

    update_input_time(input);

    if (platf::BACK & button_flags_changed) {
      if (platf::BACK & bf_new) {
        // Don't emulate home button if timeout < 0
        if (config::input.back_button_timeout >= 0ms && !gamepad.back_timeout_id) {
          auto f = [input, controller = static_cast<int>(packet->controllerNumber), expected_gamepad_id = gamepad.id, expected_generation = gamepad.generation]() {
            if (!has_client_gamepad_index(*input, controller)) {
              return;
            }

            auto &gamepad = input->gamepads[controller];
            if (!is_expected_gamepad(gamepad, expected_gamepad_id, expected_generation)) {
              return;
            }

            auto &state = gamepad.gamepad_state;

            // Force the back button up
            gamepad.back_button_state = button_state_e::UP;
            state.buttonFlags &= ~platf::BACK;
            platf::gamepad_update(platf_input, gamepad.id, state);

            // Press Home button
            state.buttonFlags |= platf::HOME;
            gamepad.synthetic_home_button_down = true;
            platf::gamepad_update(platf_input, gamepad.id, state);

            gamepad.back_timeout_id = nullptr;

            task_pool.cancel(gamepad.home_release_id);
            gamepad.home_release_id = task_pool.pushDelayed([input, controller, expected_gamepad_id, expected_generation]() {
                                                 if (!has_client_gamepad_index(*input, controller)) {
                                                   return;
                                                 }

                                                 auto &gamepad = input->gamepads[controller];
                                                 if (!is_expected_gamepad(gamepad, expected_gamepad_id, expected_generation)) {
                                                   return;
                                                 }

                                                 // Release Home button
                                                 auto &state = gamepad.gamepad_state;
                                                 state.buttonFlags &= ~platf::HOME;
                                                 gamepad.synthetic_home_button_down = false;
                                                 platf::gamepad_update(platf_input, gamepad.id, state);

                                                 gamepad.home_release_id = nullptr;
                                               },
                                                            SYNTHETIC_HOME_HOLD)
                                        .task_id;
          };

          gamepad.back_timeout_id = task_pool.pushDelayed(std::move(f), config::input.back_button_timeout).task_id;
        }
      } else if (gamepad.back_timeout_id) {
        task_pool.cancel(gamepad.back_timeout_id);
        gamepad.back_timeout_id = nullptr;
      }
    }

    platf::gamepad_update(platf_input, gamepad.id, gamepad_state);

    gamepad.gamepad_state = gamepad_state;
  }

  enum class batch_result_e {
    batched,  ///< This entry was batched with the source entry
    not_batchable,  ///< Not eligible to batch but continue attempts to batch
    terminate_batch,  ///< Stop trying to batch with this entry
  };

  /**
   * @brief Batch two relative mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_REL_MOUSE_MOVE_PACKET dest, PNV_REL_MOUSE_MOVE_PACKET src) {
    short deltaX;
    short deltaY;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->deltaX), util::endian::big(src->deltaX), &deltaX)) {
      return batch_result_e::terminate_batch;
    }
    if (__builtin_add_overflow(util::endian::big(dest->deltaY), util::endian::big(src->deltaY), &deltaY)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of deltas
    dest->deltaX = util::endian::big(deltaX);
    dest->deltaY = util::endian::big(deltaY);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two absolute mouse messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_ABS_MOUSE_MOVE_PACKET dest, PNV_ABS_MOUSE_MOVE_PACKET src) {
    // Batching must only happen if the reference width and height don't change
    if (dest->width != src->width || dest->height != src->height) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest absolute position
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two vertical scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_SCROLL_PACKET dest, PNV_SCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmt1), util::endian::big(src->scrollAmt1), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmt1 = util::endian::big(scrollAmt);
    dest->scrollAmt2 = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two horizontal scroll messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_HSCROLL_PACKET dest, PSS_HSCROLL_PACKET src) {
    short scrollAmt;

    // Batching is safe as long as the result doesn't overflow a 16-bit integer
    if (__builtin_add_overflow(util::endian::big(dest->scrollAmount), util::endian::big(src->scrollAmount), &scrollAmt)) {
      return batch_result_e::terminate_batch;
    }

    // Take the sum of delta
    dest->scrollAmount = util::endian::big(scrollAmt);
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller state messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_MULTI_CONTROLLER_PACKET dest, PNV_MULTI_CONTROLLER_PACKET src) {
    // Do not allow batching if the active controllers change
    if (dest->activeGamepadMask != src->activeGamepadMask) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Do not allow batching if the button state changes on this controller
    if (dest->buttonFlags != src->buttonFlags || dest->buttonFlags2 != src->buttonFlags2) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_TOUCH_PACKET dest, PSS_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE && src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two pen messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_PEN_PACKET dest, PSS_PEN_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same type
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Do not allow batching if the button state changes
    if (dest->penButtons != src->penButtons) {
      return batch_result_e::terminate_batch;
    }

    // Do not batch beyond tool changes
    if (dest->toolType != src->toolType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller touch messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_TOUCH_PACKET dest, PSS_CONTROLLER_TOUCH_PACKET src) {
    // Only batch hover or move events
    if (dest->eventType != LI_TOUCH_EVENT_MOVE && dest->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Don't batch beyond state changing events
    if (src->eventType != LI_TOUCH_EVENT_MOVE && src->eventType != LI_TOUCH_EVENT_HOVER) {
      return batch_result_e::terminate_batch;
    }

    // Batched events must be the same pointer ID
    if (dest->pointerId != src->pointerId) {
      return batch_result_e::not_batchable;
    }

    // The pointer must be in the same state
    if (dest->eventType != src->eventType) {
      return batch_result_e::terminate_batch;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two controller motion messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PSS_CONTROLLER_MOTION_PACKET dest, PSS_CONTROLLER_MOTION_PACKET src) {
    // We can only batch entries for the same controller, but allow batching attempts to continue
    // in case we have more packets for this controller later in the queue.
    if (dest->controllerNumber != src->controllerNumber) {
      return batch_result_e::not_batchable;
    }

    // Batched events must be the same sensor
    if (dest->motionType != src->motionType) {
      return batch_result_e::not_batchable;
    }

    // Take the latest state
    *dest = *src;
    return batch_result_e::batched;
  }

  /**
   * @brief Batch two input messages.
   * @param dest The original packet to batch into.
   * @param src A later packet to attempt to batch.
   * @return The status of the batching operation.
   */
  batch_result_e batch(PNV_INPUT_HEADER dest, PNV_INPUT_HEADER src) {
    // We can only batch if the packet types are the same
    if (dest->magic != src->magic) {
      return batch_result_e::terminate_batch;
    }

    // We can only batch certain message types
    switch (util::endian::little(dest->magic)) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        return batch_result_e::terminate_batch;
      case MOUSE_MOVE_ABS_MAGIC:
        return batch_result_e::terminate_batch;
      case SCROLL_MAGIC_GEN5:
        return batch((PNV_SCROLL_PACKET) dest, (PNV_SCROLL_PACKET) src);
      case SS_HSCROLL_MAGIC:
        return batch((PSS_HSCROLL_PACKET) dest, (PSS_HSCROLL_PACKET) src);
      case MULTI_CONTROLLER_MAGIC_GEN5:
        return batch((PNV_MULTI_CONTROLLER_PACKET) dest, (PNV_MULTI_CONTROLLER_PACKET) src);
      case SS_TOUCH_MAGIC:
        return batch((PSS_TOUCH_PACKET) dest, (PSS_TOUCH_PACKET) src);
      case SS_PEN_MAGIC:
        return batch((PSS_PEN_PACKET) dest, (PSS_PEN_PACKET) src);
      case SS_CONTROLLER_TOUCH_MAGIC:
        return batch((PSS_CONTROLLER_TOUCH_PACKET) dest, (PSS_CONTROLLER_TOUCH_PACKET) src);
      case SS_CONTROLLER_MOTION_MAGIC:
        return batch((PSS_CONTROLLER_MOTION_PACKET) dest, (PSS_CONTROLLER_MOTION_PACKET) src);
      default:
        // Not a batchable message type
        return batch_result_e::terminate_batch;
    }
  }

  /**
   * @brief Called on a thread pool thread to process one input message.
   * @param input The input context pointer.
   * @return `true` if a queued input message was processed.
   */
  bool passthrough_next_message(std::shared_ptr<input_t> input) {
    // 'entry' backs the 'payload' pointer, so they must remain in scope together
    std::vector<uint8_t> entry;
    PNV_INPUT_HEADER payload;
    std::size_t queue_size_after_batch = 0;
    std::size_t batched_entries = 0;

    // Lock the input queue while batching, but release it before sending
    // the input to the OS. This avoids potentially lengthy lock contention
    // in the control stream thread while input is being processed by the OS.
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);

      // If all entries have already been processed, nothing to do
      if (input->input_queue.empty()) {
        return false;
      }

      // Pop off the first entry, which we will send
      entry = input->input_queue.front();
      payload = (PNV_INPUT_HEADER) entry.data();
      input->input_queue.pop_front();

      // Try to batch with remaining items on the queue
      auto i = input->input_queue.begin();
      while (i != input->input_queue.end()) {
        auto &batchable_entry = *i;
        auto batchable_payload = (PNV_INPUT_HEADER) batchable_entry.data();

        auto batch_result = batch(payload, batchable_payload);
        if (batch_result == batch_result_e::terminate_batch) {
          // Stop batching
          break;
        } else if (batch_result == batch_result_e::batched) {
          // Erase this entry since it was batched
          i = input->input_queue.erase(i);
          ++batched_entries;
        } else {
          // We couldn't batch this entry, but try to batch later entries.
          i++;
        }
      }

      queue_size_after_batch = input->input_queue.size();
    }

    // Print the final input packet
    input::print((void *) payload);

    auto magic = util::endian::little(payload->magic);
    note_input_processed(magic, entry.size(), queue_size_after_batch, batched_entries);
    if (magic == MULTI_CONTROLLER_MAGIC_GEN5) {
      note_gamepad_packet((PNV_MULTI_CONTROLLER_PACKET) payload, queue_size_after_batch, batched_entries);
    }

    // Send the batched input to the OS
    switch (magic) {
      case MOUSE_MOVE_REL_MAGIC_GEN5:
        passthrough(input, (PNV_REL_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_MOVE_ABS_MAGIC:
        passthrough(input, (PNV_ABS_MOUSE_MOVE_PACKET) payload);
        break;
      case MOUSE_BUTTON_DOWN_EVENT_MAGIC_GEN5:
      case MOUSE_BUTTON_UP_EVENT_MAGIC_GEN5:
        passthrough(input, (PNV_MOUSE_BUTTON_PACKET) payload);
        break;
      case SCROLL_MAGIC_GEN5:
        passthrough(input, (PNV_SCROLL_PACKET) payload);
        break;
      case SS_HSCROLL_MAGIC:
        passthrough(input, (PSS_HSCROLL_PACKET) payload);
        break;
      case KEY_DOWN_EVENT_MAGIC:
      case KEY_UP_EVENT_MAGIC:
        passthrough(input, (PNV_KEYBOARD_PACKET) payload);
        break;
      case UTF8_TEXT_EVENT_MAGIC:
        passthrough(input, (PNV_UNICODE_PACKET) payload);
        break;
      case MULTI_CONTROLLER_MAGIC_GEN5:
        passthrough(input, (PNV_MULTI_CONTROLLER_PACKET) payload);
        break;
      case SS_TOUCH_MAGIC:
        passthrough(input, (PSS_TOUCH_PACKET) payload);
        break;
      case SS_PEN_MAGIC:
        passthrough(input, (PSS_PEN_PACKET) payload);
        break;
      case SS_CONTROLLER_ARRIVAL_MAGIC:
        passthrough(input, (PSS_CONTROLLER_ARRIVAL_PACKET) payload);
        break;
      case SS_CONTROLLER_TOUCH_MAGIC:
        passthrough(input, (PSS_CONTROLLER_TOUCH_PACKET) payload);
        break;
      case SS_CONTROLLER_MOTION_MAGIC:
        passthrough(input, (PSS_CONTROLLER_MOTION_PACKET) payload);
        break;
      case SS_CONTROLLER_BATTERY_MAGIC:
        passthrough(input, (PSS_CONTROLLER_BATTERY_PACKET) payload);
        break;
      default:
        break;
    }

    return true;
  }

  /**
   * @brief Called on a thread pool thread to drain queued input messages.
   * @param input The input context pointer.
   */
  void passthrough_drain_messages(std::shared_ptr<input_t> input) {
    auto max_messages = INPUT_DRAIN_MAX_MESSAGES;
    std::size_t processed_messages = 0;
    while (processed_messages < max_messages) {
      if (!passthrough_next_message(input)) {
        bool should_continue = false;
        {
          std::lock_guard<std::mutex> lg(input->input_queue_lock);
          if (input->input_queue.empty()) {
            input->input_queue_drain_active = false;
          } else {
            should_continue = true;
          }
        }

        if (!should_continue) {
          return;
        }

        continue;
      }

      ++processed_messages;
    }

    bool should_reschedule = false;
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      if (input->input_queue.empty()) {
        input->input_queue_drain_active = false;
      } else {
        should_reschedule = true;
      }
    }

    if (should_reschedule) {
      task_pool.pushDelayed(passthrough_drain_messages, 0ms, input);
    }
  }

  /**
   * @brief Called on the control stream thread to queue an input message.
   * @param input The input context pointer.
   * @param input_data The input message.
   */
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data) {
    auto input_data_size = input_data.size();
    std::size_t queue_size = 0;
    bool should_schedule_drain = false;
    {
      std::lock_guard<std::mutex> lg(input->input_queue_lock);
      input->input_queue.push_back(std::move(input_data));
      queue_size = input->input_queue.size();
      if (!input->input_queue_drain_active) {
        input->input_queue_drain_active = true;
        should_schedule_drain = true;
      }
    }
    note_input_queued(input_data_size, queue_size);
    if (should_schedule_drain) {
      task_pool.push(passthrough_drain_messages, input);
    }
  }

  void reset(std::shared_ptr<input_t> &input) {
    task_pool.cancel(key_press_repeat_id);

    // Ensure input is synchronous, by using the task_pool
    task_pool.push([]() {
      for (int x = 0; x < mouse_press.size(); ++x) {
        if (mouse_press[x]) {
          platf::button_mouse(platf_input, x, true);
          mouse_press[x] = false;
        }
      }

      for (auto &kp : key_press) {
        if (!kp.second) {
          // already released
          continue;
        }
        platf::keyboard_update(platf_input, vk_from_kpid(kp.first) & 0x00FF, true, flags_from_kpid(kp.first));
        key_press[kp.first] = false;
      }
    });
  }

  class deinit_t: public platf::deinit_t {
  public:
    ~deinit_t() override {
      platf_input.reset();
    }
  };

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init() {
    platf_input = platf::input();

    return std::make_unique<deinit_t>();
  }

  bool probe_gamepads() {
    auto input = static_cast<platf::input_t *>(platf_input.get());
    const auto gamepads = platf::supported_gamepads(input);
    for (auto &gamepad : gamepads) {
      if (gamepad.is_enabled && gamepad.name != "auto") {
        return false;
      }
    }
    return true;
  }

  std::shared_ptr<input_t> alloc(safe::mail_t mail) {
    auto input = std::make_shared<input_t>(
      mail->event<input::touch_port_t>(mail::touch_port),
      mail->queue<platf::gamepad_feedback_msg_t>(mail::gamepad_feedback)
    );

    // Workaround to ensure new frames will be captured when a client connects
    task_pool.pushDelayed([]() {
      platf::move_mouse(platf_input, 1, 1);
      platf::move_mouse(platf_input, -1, -1);
    },
                          100ms);

    return input;
  }

  void click_gamepad(int gamepad_nr, std::uint32_t button_flag) {
    // Check if the gamepad slot exists before dispatching
    if (!has_gamepad_index(gamepad_nr) || !gamepadMask[gamepad_nr]) {
      BOOST_LOG(warning) << "click_gamepad: slot "sv << gamepad_nr << " not allocated, dropping button=0x"sv
                         << util::hex(button_flag).to_string_view();
      return;
    }

    BOOST_LOG(info) << "click_gamepad: dispatching to slot "sv << gamepad_nr
                    << " button=0x"sv << util::hex(button_flag).to_string_view();

    task_pool.push([gamepad_nr, button_flag]() {
      if (!has_gamepad_index(gamepad_nr) || !gamepadMask[gamepad_nr]) {
        BOOST_LOG(warning) << "click_gamepad: slot "sv << gamepad_nr << " no longer allocated, dropping button=0x"sv
                           << util::hex(button_flag).to_string_view();
        return;
      }

      BOOST_LOG(info) << "click_gamepad: pressing button=0x"sv << util::hex(button_flag).to_string_view()
                      << " on slot "sv << gamepad_nr;

      platf::gamepad_state_t press_state {button_flag, 0, 0, 0, 0, 0, 0};
      platf::gamepad_update(platf_input, gamepad_nr, press_state);

      // Hold long enough for browser Gamepad API (~60Hz polling) to detect.
      // 200ms = ~12 frames, well above single-frame jitter.
      task_pool.pushDelayed([gamepad_nr, button_flag]() {
        if (!has_gamepad_index(gamepad_nr) || !gamepadMask[gamepad_nr]) {
          BOOST_LOG(warning) << "click_gamepad: slot "sv << gamepad_nr << " no longer allocated before release, button=0x"sv
                             << util::hex(button_flag).to_string_view();
          return;
        }

        BOOST_LOG(info) << "click_gamepad: releasing button=0x"sv << util::hex(button_flag).to_string_view()
                        << " on slot "sv << gamepad_nr;

        platf::gamepad_state_t release_state {};
        platf::gamepad_update(platf_input, gamepad_nr, release_state);

        BOOST_LOG(info) << "click_gamepad: button sequence complete for slot "sv << gamepad_nr;
      },
                            CLICK_GAMEPAD_HOLD);
    });
  }

  void update_input_time(const std::shared_ptr<input_t> &input) {
    input->last_input_time.store(std::chrono::steady_clock::now(), std::memory_order_release);
  }

  std::chrono::steady_clock::time_point get_last_input_time(const std::shared_ptr<input_t> &input) {
    return input->last_input_time.load(std::memory_order_acquire);
  }
}  // namespace input
