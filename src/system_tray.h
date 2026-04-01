/**
 * @file src/system_tray.h
 * @brief 系统托盘图标和通知系统的声明
 * 在任务栏显示托盘图标，提供快速访问Sunshine的菜单操作
 */
#pragma once

/**
 * @brief 系统托盘功能命名空间
 */
namespace system_tray {
  /**
   * @brief 托盘菜单回调：打开Web管理界面
   */
  void tray_open_ui_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：打开GitHub Sponsors赞助页面
   */
  void tray_donate_github_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：打开Patreon赞助页面
   */
  void tray_donate_patreon_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：打开PayPal捐赠页面
   */
  void tray_donate_paypal_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：重置显示设备配置
   */
  void tray_reset_display_device_config_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：重启 Sunshine
   */
  void tray_restart_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 托盘菜单回调：退出 Sunshine
   */
  void tray_quit_cb([[maybe_unused]] struct tray_menu *item);

  /**
   * @brief 初始化系统托盘（不启动事件循环）
   */
  int init_tray();

  /**
   * @brief 处理单次托盘事件迭代
   * @return 0 if processing was successful, non-zero otherwise.
   */
  int process_tray_events();

  /**
   * @brief Exit the system tray.
   * @return 0 after exiting the system tray.
   */
  int end_tray();

  /**
   * @brief Sets the tray icon in playing mode and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_playing(std::string app_name);

  /**
   * @brief Sets the tray icon in pausing mode (stream stopped but app running) and spawns the appropriate notification
   * @param app_name The paused application name
   */
  void update_tray_pausing(std::string app_name);

  /**
   * @brief Sets the tray icon in stopped mode (app and stream stopped) and spawns the appropriate notification
   * @param app_name The started application name
   */
  void update_tray_stopped(std::string app_name);

  /**
   * @brief Spawns a notification for PIN Pairing. Clicking it opens the PIN Web UI Page
   */
  void update_tray_require_pin();

  /**
   * @brief Initializes and runs the system tray in a separate thread.
   * @return 0 if initialization was successful, non-zero otherwise.
   */
  int init_tray_threaded();
}  // namespace system_tray
