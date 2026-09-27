#ifdef _WIN32

  #include "src/platform/windows/buke_hid_input.h"

  #include <filesystem>
  #include <gtest/gtest.h>
  #include <string>
  #include <windows.h>

namespace {
  std::wstring test_dll_path(const wchar_t *name) {
    return (std::filesystem::path(SUNSHINE_TEST_BIN_DIR) / name).wstring();
  }

  struct fake_module_t {
    using reset_fn = void(__cdecl *)();
    using set_init_result_fn = void(__cdecl *)(bool);
    using read_events_fn = const char *(__cdecl *) ();

    explicit fake_module_t(const wchar_t *name):
        module(LoadLibraryW(test_dll_path(name).c_str())) {
      if (module) {
        reset = reinterpret_cast<reset_fn>(GetProcAddress(module, "TestReset"));
        set_init_result = reinterpret_cast<set_init_result_fn>(GetProcAddress(module, "TestSetInitResult"));
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
    set_init_result_fn set_init_result = nullptr;
    read_events_fn read_events = nullptr;
  };
}  // namespace

TEST(BukeHidInput, MissingDllDoesNotActivate) {
  EXPECT_EQ(platf::buke_hid_input_t::create_from_path(test_dll_path(L"missing-buke-hid.dll")), nullptr);
}

TEST(BukeHidInput, MissingExportDoesNotActivate) {
  fake_module_t fake(L"buke_hid_incomplete.dll");
  ASSERT_NE(fake.module, nullptr);
  EXPECT_EQ(platf::buke_hid_input_t::create_from_path(test_dll_path(L"buke_hid_incomplete.dll")), nullptr);
}

TEST(BukeHidInput, FailedInitDoesNotActivate) {
  fake_module_t fake(L"buke_hid_fake.dll");
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.set_init_result, nullptr);
  fake.set_init_result(false);
  EXPECT_EQ(platf::buke_hid_input_t::create_from_path(test_dll_path(L"buke_hid_fake.dll")), nullptr);
  fake.set_init_result(true);
}

TEST(BukeHidInput, SendsKeyboardMouseAndWholeWheelDetents) {
  fake_module_t fake(L"buke_hid_fake.dll");
  ASSERT_NE(fake.module, nullptr);
  ASSERT_NE(fake.reset, nullptr);
  ASSERT_NE(fake.read_events, nullptr);
  fake.set_init_result(true);

  auto input = platf::buke_hid_input_t::create_from_path(test_dll_path(L"buke_hid_fake.dll"));
  ASSERT_NE(input, nullptr);
  fake.reset();

  input->key('W', false);
  input->key('W', true);
  input->move_relative(3, -2);
  input->move_absolute(100, 200);
  for (int button = 1; button <= 5; ++button) {
    input->button(button, false);
    input->button(button, true);
  }
  input->wheel(60);
  input->wheel(60);
  input->wheel(60);
  input->wheel(-60);
  input->wheel(-120);

  EXPECT_STREQ(fake.read_events(), "K+:87,0;K-:87,0;R:3,-2;A:100,200;L+;L-;M+;M-;B+;B-;X1+;X1-;X2+;X2-;WU;WD;");
}

#endif
