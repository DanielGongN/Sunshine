#ifdef _WIN32
  #include "src/platform/windows/desktop_cursor.h"

  #include <chrono>
  #include <gtest/gtest.h>

namespace {
  using mapper_t = platf::detail::desktop_cursor_mapper_t;
  using bounds_t = platf::detail::desktop_cursor_bounds_t;

  TEST(DesktopCursorMapper, AccumulatesMovementAndFractions) {
    mapper_t mapper;
    const bounds_t bounds {0, 0, 1920, 1080};
    const auto now = mapper_t::clock_t::now();
    ASSERT_TRUE(mapper.needs_sync(bounds, now));
    mapper.sync(bounds, 100, 100);

    const auto first = mapper.advance(bounds, 0.4, 0.4, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->x, 100);
    EXPECT_EQ(first->y, 100);

    const auto second = mapper.advance(bounds, 0.4, 0.4, now + std::chrono::milliseconds {1});
    ASSERT_TRUE(second);
    EXPECT_EQ(second->x, 101);
    EXPECT_EQ(second->y, 101);

    const auto third = mapper.advance(bounds, 5.0, -2.0, now + std::chrono::milliseconds {2});
    ASSERT_TRUE(third);
    EXPECT_EQ(third->x, 106);
    EXPECT_EQ(third->y, 99);
  }

  TEST(DesktopCursorMapper, NormalizesVirtualDesktopEdges) {
    mapper_t mapper;
    const bounds_t bounds {-100, -50, 201, 101};
    const auto now = mapper_t::clock_t::now();
    mapper.sync(bounds, -100, -50);

    const auto first = mapper.advance(bounds, 0, 0, now);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->normalized_x, 0);
    EXPECT_EQ(first->normalized_y, 0);

    const auto last = mapper.advance(bounds, 200, 100, now);
    ASSERT_TRUE(last);
    EXPECT_EQ(last->x, 100);
    EXPECT_EQ(last->y, 50);
    EXPECT_EQ(last->normalized_x, 65535);
    EXPECT_EQ(last->normalized_y, 65535);
  }

  TEST(DesktopCursorMapper, ClampsMovementAndResyncsAfterIdleOrDisplayChange) {
    mapper_t mapper;
    const bounds_t bounds {-1920, 0, 3840, 1080};
    const auto now = mapper_t::clock_t::now();
    mapper.sync(bounds, -5, 1070);

    const auto moved = mapper.advance(bounds, 3000, 100, now);
    ASSERT_TRUE(moved);
    EXPECT_EQ(moved->x, 1919);
    EXPECT_EQ(moved->y, 1079);
    EXPECT_FALSE(mapper.needs_sync(bounds, now + std::chrono::milliseconds {99}));
    EXPECT_TRUE(mapper.needs_sync(bounds, now + std::chrono::milliseconds {100}));

    const bounds_t changed {0, 0, 1920, 1080};
    EXPECT_TRUE(mapper.needs_sync(changed, now));
    EXPECT_FALSE(mapper.advance(changed, 1, 1, now));
    mapper.sync(changed, 50, 60);
    const auto after_sync = mapper.advance(changed, 1, 2, now);
    ASSERT_TRUE(after_sync);
    EXPECT_EQ(after_sync->x, 51);
    EXPECT_EQ(after_sync->y, 62);

    mapper.reset();
    EXPECT_TRUE(mapper.needs_sync(changed, now));
    EXPECT_FALSE(mapper.advance(changed, 1, 1, now));
  }

  TEST(DesktopCursorMapper, RejectsInvalidDesktopBounds) {
    mapper_t mapper;
    const bounds_t invalid {0, 0, 0, 1080};
    const auto now = mapper_t::clock_t::now();
    mapper.sync(invalid, 0, 0);
    EXPECT_FALSE(mapper.advance(invalid, 1, 1, now));
  }
}  // namespace
#endif
