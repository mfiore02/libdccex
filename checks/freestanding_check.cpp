// Compiled with -ffreestanding -fno-exceptions -fno-rtti, never linked.
// The no-allocation check inspects this object's undefined symbols, so every
// library component must be included and instantiated here.

#include <cstdint>

#include "dccex/decoder.hpp"
#include "dccex/encoder.hpp"
#include "dccex/framer.hpp"
#include "dccex/string_view.hpp"
#include "dccex/tokenizer.hpp"

namespace {

struct Totals {
  std::size_t bytes = 0;
  std::int64_t sum = 0;
};

void on_message(void* ctx, dccex::StringView message) {
  auto* totals = static_cast<Totals*>(ctx);
  totals->bytes += message.size();

  dccex::Tokenizer tokens(message);
  dccex::Field field;
  while (tokens.next(field)) {
    std::int8_t i8 = 0;
    std::uint16_t u16 = 0;
    std::int32_t i32 = 0;
    std::uint32_t u32 = 0;
    if (field.to_integer(i8)) totals->sum += i8;
    if (field.to_integer(u16)) totals->sum += u16;
    if (field.to_integer(i32)) totals->sum += i32;
    if (field.to_integer(u32)) totals->sum += u32;
    if (field.equals("MAIN")) totals->sum += 1;
  }
  if (tokens.malformed()) totals->sum -= 1;
  totals->bytes += tokens.count() + tokens.rest().size();
  if (tokens.at(1, field)) totals->bytes += field.text.size();

  const dccex::Event e = dccex::decode(message);
  totals->bytes += dccex::StringView(dccex::name(e.type)).size();
  if (e.type == dccex::EventType::Unhandled) {
    totals->bytes += dccex::StringView(dccex::name(e.unhandled.reason)).size() + e.unhandled.field;
    if (e.unhandled.malformed()) totals->sum -= 1;
  }
}

}  // namespace

std::size_t dccex_freestanding_encode(char* out, std::size_t capacity, std::uint16_t cab) {
  dccex::Buffer buf(out, capacity);
  std::size_t total = 0;
  total += dccex::encode::power(buf, dccex::PowerTarget::Main, true).length;
  total += dccex::encode::join_prog_to_main(buf).length;
  total += dccex::encode::status(buf).length;
  total += dccex::encode::throttle(buf, cab, 5, true).length;
  total += dccex::encode::emergency_stop(buf, cab, true).length;
  total += dccex::encode::emergency_stop_all(buf).length;
  total += dccex::encode::query_loco(buf, cab).length;
  total += dccex::encode::function(buf, cab, 2, true).length;
  total += dccex::encode::forget_loco(buf, cab).length;
  total += dccex::encode::forget_all_locos(buf).length;
  total += dccex::encode::read_cv(buf, 29).length;
  total += dccex::encode::read_address(buf).length;
  total += dccex::encode::write_cv(buf, 3, 10).length;
  total += dccex::encode::write_address(buf, cab).length;
  const dccex::Encoded last = dccex::encode::write_cv_main(buf, cab, 3, 10);
  return total + last.length + dccex::StringView(dccex::name(last.error)).size();
}

std::int64_t dccex_freestanding_check(const char* data, std::size_t len) {
  static dccex::Framer<> framer;
  Totals totals;
  framer.feed(data, len, on_message, &totals);
  return totals.sum + static_cast<std::int64_t>(totals.bytes + framer.overflowed() + framer.abandoned());
}
