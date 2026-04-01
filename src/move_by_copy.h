/**
 * @file src/move_by_copy.h
 * @brief MoveByCopy工具类的声明
 * 将移动语义包装为拷贝语义，用于将只能移动的对象传入std::function等要求可拷贝的场景
 */
#pragma once

#include <utility>

/**
 * @brief 移动代替拷贝工具命名空间
 */
namespace move_by_copy_util {
  /**
   * @brief 拷贝时实际执行移动的包装器
   * 当需要将只能移动的对象（如unique_ptr）传入要求可拷贝的容器时使用
   */
  template<class T>
  class MoveByCopy {
  public:
    typedef T move_type;

  private:
    move_type _to_move;

  public:
    explicit MoveByCopy(move_type &&to_move):
        _to_move(std::move(to_move)) {
    }

    MoveByCopy(MoveByCopy &&other) = default;

    MoveByCopy(const MoveByCopy &other) {
      *this = other;
    }

    MoveByCopy &operator=(MoveByCopy &&other) = default;

    MoveByCopy &operator=(const MoveByCopy &other) {
      this->_to_move = std::move(const_cast<MoveByCopy &>(other)._to_move);

      return *this;
    }

    operator move_type() {
      return std::move(_to_move);
    }
  };

  template<class T>
  MoveByCopy<T> cmove(T &movable) {
    return MoveByCopy<T>(std::move(movable));
  }

  // Do NOT use this unless you are absolutely certain the object to be moved is no longer used by the caller
  template<class T>
  MoveByCopy<T> const_cmove(const T &movable) {
    return MoveByCopy<T>(std::move(const_cast<T &>(movable)));
  }
}  // namespace move_by_copy_util
