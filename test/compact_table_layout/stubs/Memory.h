#pragma once

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

template <typename T, std::enable_if_t<!std::is_array_v<T>, int> = 0, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T, std::enable_if_t<std::is_array_v<T>, int> = 0>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  using Element = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
