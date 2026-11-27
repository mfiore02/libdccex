// Compiled with -ffreestanding -fno-exceptions -fno-rtti, never linked.
// The no-allocation check inspects this object's undefined symbols, so every
// library component must be included and instantiated here.

#include "dccex/framer.hpp"
#include "dccex/string_view.hpp"

namespace {

void on_message(void* ctx, dccex::StringView message) {
  auto* total = static_cast<std::size_t*>(ctx);
  *total += message.size();
}

}  // namespace

std::size_t dccex_freestanding_check(const char* data, std::size_t len) {
  static dccex::Framer<> framer;
  std::size_t total = 0;
  framer.feed(data, len, on_message, &total);
  return total + framer.dropped();
}
