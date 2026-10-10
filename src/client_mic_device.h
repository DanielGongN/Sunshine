/**
 * @file src/client_mic_device.h
 * @brief Virtual cable selection and shared recording endpoint gating.
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace client_mic {
  struct sink_device_t {
    std::string id;
    std::string name;
    std::string description;
    std::string adapter;
  };

  struct sink_pair_t {
    sink_device_t playback;
    sink_device_t recording;
  };

  inline std::string lowercase_device_name(std::string_view name) {
    std::string result {name};
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return result;
  }

  inline bool is_virtual_cable(const sink_device_t &device) {
    const auto adapter = lowercase_device_name(device.adapter);
    return adapter.find("virtual audio cable") != std::string::npos ||
           (adapter.find("vb-audio") != std::string::npos && adapter.find("cable") != std::string::npos);
  }

  inline std::vector<sink_pair_t> find_sink_pairs(std::vector<sink_device_t> playback, const std::vector<sink_device_t> &recording, const std::string &configured) {
    std::sort(playback.begin(), playback.end(), [](const auto &lhs, const auto &rhs) {
      return lhs.id < rhs.id;
    });

    if (!configured.empty()) {
      const std::array fields {&sink_device_t::id, &sink_device_t::name, &sink_device_t::description, &sink_device_t::adapter};
      bool found = false;
      for (auto field : fields) {
        const auto match = std::find_if(playback.begin(), playback.end(), [&](const auto &device) {
          return device.*field == configured;
        });
        if (match != playback.end()) {
          auto selected = *match;
          playback = {std::move(selected)};
          found = true;
          break;
        }
      }
      if (!found) {
        return {};
      }
    }

    std::vector<sink_pair_t> pairs;
    for (const auto &output : playback) {
      if (configured.empty() && !is_virtual_cable(output)) {
        continue;
      }

      std::vector<const sink_device_t *> matches;
      for (const auto &input : recording) {
        if (!output.adapter.empty() && lowercase_device_name(output.adapter) == lowercase_device_name(input.adapter)) {
          matches.push_back(&input);
        }
      }

      // VAC exposes multiple lines through one adapter. Prefer an exact line
      // name/description; a unique cable adapter also pairs VB-Audio Input/Output.
      std::vector<const sink_device_t *> exact;
      for (const auto *input : matches) {
        if ((!output.name.empty() && output.name == input->name) || (!output.description.empty() && output.description == input->description)) {
          exact.push_back(input);
        }
      }
      if (exact.size() == 1) {
        pairs.push_back({output, *exact.front()});
      } else if (exact.empty() && matches.size() == 1 && lowercase_device_name(output.adapter).find("vb-audio") != std::string::npos && is_virtual_cable(output)) {
        pairs.push_back({output, *matches.front()});
      }
    }
    return pairs;
  }

  class sink_gate_t {
  public:
    using mute_f = std::function<bool(bool)>;

    bool acquire(const mute_f &mute) {
      std::lock_guard lock {mutex};
      if (!writers && !mute(false)) {
        return false;
      }
      ++writers;
      return true;
    }

    bool release(const mute_f &mute) {
      std::lock_guard lock {mutex};
      if (writers) {
        --writers;
      }
      return writers || mute(true);
    }

    bool silence(const mute_f &mute) {
      std::lock_guard lock {mutex};
      return writers || mute(true);
    }

  private:
    std::mutex mutex;
    std::size_t writers {};
  };
}  // namespace client_mic
