#ifdef _WIN32

  #include "src/platform/windows/interception_input.h"
  #include "src/platform/windows/keyboard_mouse_backend.h"

  #include <filesystem>
  #include <gtest/gtest.h>
  #include <windows.h>

namespace {
  std::wstring test_dll_path(const wchar_t *name) {
    return (std::filesystem::path(SUNSHINE_TEST_BIN_DIR) / name).wstring();
  }

  struct fake_module_t {
    using reset_fn = void(__cdecl *)();
    using set_context_result_fn = void(__cdecl *)(bool);
    using set_device_availability_fn = void(__cdecl *)(bool, bool);
    using read_events_fn = const char *(__cdecl *) ();

    fake_module_t():
        module(LoadLibraryW(test_dll_path(L"interception_fake.dll").c_str())) {
      if (module) {
        reset = reinterpret_cast<reset_fn>(GetProcAddress(module, "TestReset"));
        set_context_result = reinterpret_cast<set_context_result_fn>(GetProcAddress(module, "TestSetContextResult"));
        set_device_availability = reinterpret_cast<set_device_availability_fn>(GetProcAddress(module, "TestSetDeviceAvailability"));
        read_events = reinterpret_cast<read_events_fn>(GetProcAddress(module, "TestReadEvents"));
      }
    }

    ~fake_module_t() {
      if (module) {
        FreeLibrary(module);
      }
    }

    HMODULE module;
    reset_fn reset = nullptr;
    set_context_result_fn set_context_result = nullptr;
    set_device_availability_fn set_device_availability = nullptr;
    read_events_fn read_events = nullptr;
  };

  struct buke_fake_module_t {
    using reset_fn = void(__cdecl *)();
    using read_events_fn = const char *(__cdecl *) ();

    buke_fake_module_t():
        module(LoadLibraryW(test_dll_path(L"buke_hid_fake.dll").c_str())) {
      if (module) {
        reset = reinterpret_cast<reset_fn>(GetProcAddress(module, "TestReset"));
        read_events = reinterpret_cast<read_events_fn>(GetProcAddress(module, "TestReadEvents"));
      }
    }

    ~buke_fake_module_t() {
      if (module) {
        FreeLibrary(module);
      }
    }

    HMODULE module;
    reset_fn reset = nullptr;
    read_events_fn read_events = nullptr;
  };
}  // namespace

TEST(InterceptionInput, PreferredOverBukeWhenBothAreUsable) {
  fake_module_t interception_fake;
  buke_fake_module_t buke_fake;
  ASSERT_NE(interception_fake.module, nullptr);
  ASSERT_NE(buke_fake.module, nullptr);
  ASSERT_NE(interception_fake.reset, nullptr);
  ASSERT_NE(buke_fake.reset, nullptr);
  ASSERT_NE(buke_fake.read_events, nullptr);
  interception_fake.reset();
  buke_fake.reset();

  auto backend = platf::select_keyboard_mouse_backend(test_dll_path(L"interception_fake.dll"), test_dll_path(L"buke_hid_fake.dll"));
  EXPECT_NE(backend.interception, nullptr);
  EXPECT_EQ(backend.buke_hid, nullptr);
  EXPECT_STREQ(buke_fake.read_events(), "");
}

TEST(InterceptionInput, FallsBackToBukeWhenInterceptionFails) {
  fake_module_t interception_fake;
  buke_fake_module_t buke_fake;
  ASSERT_NE(interception_fake.module, nullptr);
  ASSERT_NE(buke_fake.module, nullptr);
  ASSERT_NE(interception_fake.reset, nullptr);
  ASSERT_NE(interception_fake.set_context_result, nullptr);
  ASSERT_NE(buke_fake.reset, nullptr);
  ASSERT_NE(buke_fake.read_events, nullptr);
  interception_fake.reset();
  interception_fake.set_context_result(false);
  buke_fake.reset();

  auto backend = platf::select_keyboard_mouse_backend(test_dll_path(L"interception_fake.dll"), test_dll_path(L"buke_hid_fake.dll"));
  EXPECT_EQ(backend.interception, nullptr);
  EXPECT_NE(backend.buke_hid, nullptr);
  EXPECT_STREQ(buke_fake.read_events(), "I;");
  interception_fake.reset();
}

TEST(InterceptionInput, UsesWindowsInputWhenBothDllsAreMissing) {
  auto backend = platf::select_keyboard_mouse_backend(test_dll_path(L"missing-interception.dll"), test_dll_path(L"missing-buke-hid.dll"));
  EXPECT_EQ(backend.interception, nullptr);
  EXPECT_EQ(backend.buke_hid, nullptr);
}

TEST(InterceptionInput, MissingDllDoesNotActivate) {
  EXPECT_EQ(platf::interception_input_t::create_from_path(test_dll_path(L"missing-interception.dll")), nullptr);
}

TEST(InterceptionInput, MissingExportDoesNotActivate) {
  EXPECT_EQ(platf::interception_input_t::create_from_path(test_dll_path(L"interception_incomplete.dll")), nullptr);
}

TEST(InterceptionInput, FailedContextDoesNotActivate) {
  fake_module_t fake;
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.reset, nullptr);
  ASSERT_NE(fake.set_context_result, nullptr);
  fake.reset();
  fake.set_context_result(false);
  EXPECT_EQ(platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll")), nullptr);
  fake.reset();
}

TEST(InterceptionInput, MissingKeyboardOrMouseDoesNotActivate) {
  fake_module_t fake;
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.reset, nullptr);
  ASSERT_NE(fake.set_device_availability, nullptr);
  fake.reset();
  fake.set_device_availability(false, true);
  EXPECT_EQ(platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll")), nullptr);
  fake.set_device_availability(true, false);
  EXPECT_EQ(platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll")), nullptr);
  fake.reset();
}

TEST(InterceptionInput, SendsKeyboardMouseButtonsAndWheels) {
  fake_module_t fake;
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.reset, nullptr);
  ASSERT_NE(fake.read_events, nullptr);
  fake.reset();

  auto input = platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll"));
  ASSERT_NE(input, nullptr);
  EXPECT_TRUE(input->key(0x11, 0));
  EXPECT_TRUE(input->key(0x11, 1));
  EXPECT_TRUE(input->key(0x48, 2));
  EXPECT_TRUE(input->move_relative(300, -260));
  for (int button = 1; button <= 5; ++button) {
    EXPECT_TRUE(input->button(button, false));
    EXPECT_TRUE(input->button(button, true));
  }
  EXPECT_TRUE(input->wheel(120, false));
  EXPECT_TRUE(input->wheel(-120, true));

  EXPECT_STREQ(fake.read_events(),
               "K:1,17,0;K:1,17,1;K:1,72,2;M:11,0,0,0,300,-260;"
               "M:11,1,0,0,0,0;M:11,2,0,0,0,0;"
               "M:11,16,0,0,0,0;M:11,32,0,0,0,0;"
               "M:11,4,0,0,0,0;M:11,8,0,0,0,0;"
               "M:11,64,0,0,0,0;M:11,128,0,0,0,0;"
               "M:11,256,0,0,0,0;M:11,512,0,0,0,0;"
               "M:11,1024,0,120,0,0;M:11,2048,0,-120,0,0;");
}

TEST(InterceptionInput, SplitsWheelOutsideSigned16BitRange) {
  fake_module_t fake;
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.reset, nullptr);
  ASSERT_NE(fake.read_events, nullptr);
  fake.reset();

  auto input = platf::interception_input_t::create_from_path(test_dll_path(L"interception_fake.dll"));
  ASSERT_NE(input, nullptr);
  EXPECT_TRUE(input->wheel(40000, false));
  EXPECT_TRUE(input->wheel(-40000, true));
  EXPECT_STREQ(fake.read_events(),
               "M:11,1024,0,32767,0,0;M:11,1024,0,7233,0,0;"
               "M:11,2048,0,-32768,0,0;M:11,2048,0,-7232,0,0;");
}

#endif
