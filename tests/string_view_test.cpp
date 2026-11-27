#include <doctest/doctest.h>

#include "dccex/string_view.hpp"

TEST_SUITE("string_view") {

TEST_CASE("a default view is empty") {
  constexpr dccex::StringView v;
  CHECK(v.empty());
  CHECK(v.size() == 0);
}

TEST_CASE("a view can be made from a C string") {
  const char* s = "MAIN";
  const dccex::StringView v(s);
  CHECK(v.size() == 4);
  CHECK(v.data() == s);
}

TEST_CASE("a view of a null C string is empty") {
  const char* s = nullptr;
  const dccex::StringView v(s);
  CHECK(v.empty());
}

TEST_CASE("C string views are usable at compile time") {
  constexpr dccex::StringView v("PROG");
  static_assert(v.size() == 4, "length computed at compile time");
  CHECK(v[0] == 'P');
}

TEST_CASE("views compare by content") {
  const char a[] = "MAIN";
  const char b[] = "MAIN";
  CHECK(dccex::StringView(a) == dccex::StringView(b));
  CHECK(dccex::StringView(a) != dccex::StringView("PROG"));
  CHECK(dccex::StringView("MAI") != dccex::StringView("MAIN"));
  CHECK(dccex::StringView() == dccex::StringView(""));
}

}  // TEST_SUITE
