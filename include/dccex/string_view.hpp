#pragma once

#include <cstddef>

namespace dccex {

// Minimal non-owning view of a character range. Freestanding replacement for
// std::string_view: no allocation, no dependency on <string_view>.
class StringView {
public:
  constexpr StringView() noexcept = default;
  constexpr StringView(const char* data, std::size_t size) noexcept
      : data_(data), size_(size) {}

  constexpr const char* data() const noexcept { return data_; }
  constexpr std::size_t size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0; }

  constexpr char operator[](std::size_t i) const noexcept { return data_[i]; }

  constexpr const char* begin() const noexcept { return data_; }
  constexpr const char* end() const noexcept { return data_ + size_; }

  friend constexpr bool operator==(StringView a, StringView b) noexcept {
    if (a.size_ != b.size_) return false;
    for (std::size_t i = 0; i < a.size_; ++i) {
      if (a.data_[i] != b.data_[i]) return false;
    }
    return true;
  }
  friend constexpr bool operator!=(StringView a, StringView b) noexcept {
    return !(a == b);
  }

private:
  const char* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace dccex
