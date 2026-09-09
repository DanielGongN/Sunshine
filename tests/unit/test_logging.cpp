/**
 * @file tests/unit/test_logging.cpp
 * @brief Test src/logging.*.
 */
#include "../tests_common.h"
#include "../tests_log_checker.h"

#include <filesystem>
#include <format>
#include <random>
#include <regex>
#include <src/logging.h>

namespace {
  std::array log_levels = {
    std::tuple("verbose", &verbose),
    std::tuple("debug", &debug),
    std::tuple("info", &info),
    std::tuple("warning", &warning),
    std::tuple("error", &error),
    std::tuple("fatal", &fatal),
  };

  constexpr auto log_file = "test_sunshine.log";
}  // namespace

struct LogLevelsTest: testing::TestWithParam<decltype(log_levels)::value_type> {};

INSTANTIATE_TEST_SUITE_P(
  Logging,
  LogLevelsTest,
  testing::ValuesIn(log_levels),
  [](const auto &info) {
    return std::string(std::get<0>(info.param));
  }
);

TEST_P(LogLevelsTest, PutMessage) {
  auto [label, plogger] = GetParam();
  ASSERT_TRUE(plogger);
  auto &logger = *plogger;

  std::random_device rand_dev;
  std::mt19937_64 rand_gen(rand_dev());
  auto test_message = std::format("{}{}", rand_gen(), rand_gen());
  BOOST_LOG(logger) << test_message;

  ASSERT_TRUE(log_checker::line_contains(log_file, test_message));
}

TEST(Logging, MakeTimestampedLogFileInsertsTimestampBeforeExtension) {
  const std::filesystem::path generated_path {logging::make_timestamped_log_file("logs/engine.log")};
  const std::regex filename_pattern {R"(engine-[0-9]{8}-[0-9]{6}-[0-9]{3}(-[0-9]+)?\.log)"};

  ASSERT_EQ(generated_path.parent_path().generic_string(), "logs");
  ASSERT_TRUE(std::regex_match(generated_path.filename().string(), filename_pattern));
}
