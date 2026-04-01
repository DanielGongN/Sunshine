/**
 * @file globals.cpp
 * @brief 全局变量的定义和初始化
 */
// 本地项目头文件
#include "globals.h"

safe::mail_t mail::man;                        // 全局邮件管理器实例（进程级消息总线）
thread_pool_util::ThreadPool task_pool;         // 全局线程池实例（用于延迟任务调度等）
bool display_cursor = true;                     // 默认显示鼠标光标

#ifdef _WIN32
nvprefs::nvprefs_interface nvprefs_instance;    // NVIDIA控制面板设置管理实例
#endif
