// Proof obligations for the support layer: CRC-32C, digests, text validation,
// canonical serialisation, the filesystem primitives and the store lock.
//
// Each check names the defect it would catch, so a failure tells the next
// reader what invariant the implementation just broke.

#include "support/test_harness.hpp"

#include "cxf/support/crc32c.hpp"
#include "cxf/support/digest.hpp"
#include "cxf/support/fs.hpp"
#include "cxf/support/lock_file.hpp"
#include "cxf/support/serial.hpp"
#include "cxf/support/text.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cxf::Code;
using cxf::Crc32c;
using cxf::Digest;
using cxf::DigestBuilder;
using cxf::LockFile;
using cxf::Outcome;
using cxf::SerialReader;
using cxf::SerialWriter;
using cxf::Status;
namespace fs = cxf::fs;

[[nodiscard]] std::span<const std::byte> as_bytes(std::string_view text) {
  return std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                    text.size());
}

[[nodiscard]] std::vector<std::byte> to_bytes(std::string_view text) {
  std::vector<std::byte> out;
  out.reserve(text.size());
  for (char c : text) {
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
  }
  return out;
}

[[nodiscard]] std::string from_bytes(std::span<const std::byte> bytes) {
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::string hex_of_byte(std::uint8_t value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.push_back(kHex[(value >> 4u) & 0x0Fu]);
  out.push_back(kHex[value & 0x0Fu]);
  return out;
}

// ---------------------------------------------------------------------------
// CRC-32C.
// ---------------------------------------------------------------------------

CXF_TEST(unit, crc32c_known_answer) {
  // The published check value of CRC-32C over "123456789": if the reflected
  // polynomial or the final inversion is wrong, this is the check that fails.
  const std::string_view text = "123456789";
  CHECK_EQ(Crc32c::compute(as_bytes(text)), 0xE3069283u);

  // An empty input has a well-defined value (the initial state fully inverted).
  CHECK_EQ(Crc32c::compute(std::span<const std::byte>()), 0x00000000u);

  // Feeding the same bytes in different chunkings must not change the result:
  // a durable frame is written in pieces and must verify as one frame.
  Crc32c one_plus_eight;
  one_plus_eight.update(as_bytes(text).first(1));
  one_plus_eight.update(as_bytes(text).subspan(1));
  CHECK_EQ(one_plus_eight.value(), Crc32c::compute(as_bytes(text)));

  Crc32c eight_plus_one;
  eight_plus_one.update(as_bytes(text).first(8));
  eight_plus_one.update(as_bytes(text).subspan(8));
  CHECK_EQ(eight_plus_one.value(), 0xE3069283u);

  Crc32c reversed;
  reversed.update(as_bytes(text).last(4));
  reversed.update(as_bytes(text).first(5));
  CHECK(reversed.value() != 0xE3069283u);

  // A single bit anywhere in the input changes the checksum.
  std::string mutated(text);
  mutated[3] = static_cast<char>(mutated[3] ^ 0x01);
  CHECK(Crc32c::compute(as_bytes(mutated)) != 0xE3069283u);
}

// ---------------------------------------------------------------------------
// Digests.
// ---------------------------------------------------------------------------

CXF_TEST(unit, digest_parse_and_format) {
  const std::string canonical(64, 'a');
  const Outcome<Digest> parsed = Digest::parse(canonical);
  REQUIRE_OK(parsed);
  CHECK_EQ(parsed->hex(), canonical);
  CHECK_EQ(parsed->view(), std::string_view(canonical));
  CHECK_EQ(std::string(parsed->c_str()), canonical);
  CHECK(!parsed->is_zero());
  CHECK(!parsed->empty());

  // Exactly one textual spelling: length, case and alphabet are all enforced.
  CHECK_CODE(Digest::parse(""), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(63, 'a')), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(65, 'a')), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(64, 'A')), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(63, 'a') + "g"), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(63, 'a') + " "), Code::kMalformedInput);
  CHECK_CODE(Digest::parse(std::string(64, '0').substr(0, 63) + "-"), Code::kMalformedInput);

  // The default digest is empty, which is a different state from "all zero".
  const Digest empty;
  CHECK(empty.empty());
  CHECK(!empty.is_zero());

  const std::string zeros(64, '0');
  const Outcome<Digest> zero = Digest::parse(zeros);
  REQUIRE_OK(zero);
  CHECK(zero->is_zero());
  CHECK(!zero->empty());
  CHECK_EQ(zero->hex(), zeros);
}

CXF_TEST(unit, digest_from_bytes_is_deterministic) {
  std::vector<std::byte> material(32);
  for (std::size_t i = 0; i < material.size(); ++i) {
    material[i] = static_cast<std::byte>(i * 7u + 1u);
  }
  const Digest first = Digest::from_bytes(material);
  const Digest second = Digest::from_bytes(material);
  CHECK_EQ(first, second);
  std::string expected;
  for (const std::byte b : material) {
    expected += hex_of_byte(static_cast<std::uint8_t>(b));
  }
  CHECK_EQ(first.hex(), expected);
  CHECK_EQ(first.hex().size(), std::size_t{64});

  std::vector<std::byte> changed = material;
  changed[31] = static_cast<std::byte>(static_cast<std::uint8_t>(changed[31]) ^ 0x80u);
  CHECK(Digest::from_bytes(changed) != first);

  // A non-32-byte input still produces one canonical digest, deterministically.
  const std::vector<std::byte> short_input = {std::byte{0x01}, std::byte{0x02}};
  const Digest folded_a = Digest::from_bytes(short_input);
  const Digest folded_b = Digest::from_bytes(short_input);
  CHECK_EQ(folded_a, folded_b);
  CHECK_EQ(folded_a.hex().size(), std::size_t{64});
  CHECK(folded_a != first);

  // Streaming equals one-shot, and text and byte views of the same content agree.
  DigestBuilder builder;
  builder.update(material.data(), 11);
  builder.update(std::span<const std::byte>(material).subspan(11));
  CHECK_EQ(builder.finish(), DigestBuilder::of(material));
  CHECK_EQ(DigestBuilder::of(std::string_view("cxf")), DigestBuilder::of(as_bytes("cxf")));
  CHECK(DigestBuilder::of(std::string_view("cxf")) != DigestBuilder::of(std::string_view("cxf ")));

  const Outcome<Digest> low = Digest::parse(std::string(63, '0') + "1");
  const Outcome<Digest> high = Digest::parse(std::string(63, '0') + "2");
  REQUIRE_OK(low);
  REQUIRE_OK(high);
  CHECK(*low < *high);
}

// ---------------------------------------------------------------------------
// Text validation.
// ---------------------------------------------------------------------------

CXF_TEST(unit, text_utf8_valid_forms) {
  CHECK(cxf::is_valid_utf8(""));
  CHECK(cxf::is_valid_utf8("hello"));
  CHECK(cxf::is_valid_utf8("caf\xC3\xA9"));            // U+00E9, two bytes
  CHECK(cxf::is_valid_utf8("\xE2\x82\xAC"));            // U+20AC, three bytes
  CHECK(cxf::is_valid_utf8("\xF0\x90\x8D\x88"));        // U+10348, four bytes
  CHECK(cxf::is_valid_utf8("\xEF\xBF\xBD"));            // U+FFFD, the last three-byte form
  CHECK(cxf::is_valid_utf8("tab\there"));
}

CXF_TEST(unit, text_utf8_rejects_hostile_forms) {
  // Over-long encodings would let one character have two byte spellings, so a
  // content address over text would stop being canonical.
  CHECK(!cxf::is_valid_utf8(std::string("\xC0\x80", 2)));
  CHECK(!cxf::is_valid_utf8(std::string("\xC1\xBF", 2)));
  CHECK(!cxf::is_valid_utf8(std::string("\xE0\x80\xAF", 3)));
  CHECK(!cxf::is_valid_utf8(std::string("\xF0\x80\x80\xAF", 4)));
  // Surrogates are not scalar values and must never be accepted.
  CHECK(!cxf::is_valid_utf8(std::string("\xED\xA0\x80", 3)));
  CHECK(!cxf::is_valid_utf8(std::string("\xED\xBF\xBF", 3)));
  // Above U+10FFFF.
  CHECK(!cxf::is_valid_utf8(std::string("\xF4\x90\x80\x80", 4)));
  CHECK(!cxf::is_valid_utf8(std::string("\xF5\x80\x80\x80", 4)));
  CHECK(!cxf::is_valid_utf8(std::string("\xF8\x88\x80\x80\x80", 5)));
  // Truncated sequences and a lone continuation byte.
  CHECK(!cxf::is_valid_utf8(std::string("\xC3", 1)));
  CHECK(!cxf::is_valid_utf8(std::string("\xE2\x82", 2)));
  CHECK(!cxf::is_valid_utf8(std::string("\xF0\x90\x8D", 3)));
  CHECK(!cxf::is_valid_utf8(std::string("\x80", 1)));
  // An embedded NUL cannot be carried in a text field at all.
  CHECK(!cxf::is_valid_utf8(std::string("a\0b", 3)));
  CHECK(!cxf::is_valid_utf8(std::string("\0", 1)));
}

CXF_TEST(unit, text_token_and_display_rules) {
  CHECK(cxf::is_valid_token("a"));
  CHECK(cxf::is_valid_token("Node-1.rack:7_u"));
  CHECK(cxf::is_valid_token(std::string(64, 'z')));
  CHECK(!cxf::is_valid_token(""));
  CHECK(!cxf::is_valid_token(std::string(65, 'z')));
  CHECK(!cxf::is_valid_token("has space"));
  CHECK(!cxf::is_valid_token("slash/inside"));
  CHECK(!cxf::is_valid_token("hash#inside"));
  CHECK(!cxf::is_valid_token("caf\xC3\xA9"));

  CHECK(cxf::is_valid_display_text("a name"));
  CHECK(cxf::is_valid_display_text("tab\tinside"));
  CHECK(cxf::is_valid_display_text("caf\xC3\xA9"));
  CHECK(cxf::is_valid_display_text(std::string(cxf::kMaxTextFieldBytes, 'x')));
  CHECK(!cxf::is_valid_display_text(""));
  CHECK(!cxf::is_valid_display_text(std::string(cxf::kMaxTextFieldBytes + 1, 'x')));
  CHECK(!cxf::is_valid_display_text(std::string("\x01", 1)));
  CHECK(!cxf::is_valid_display_text("line\nbreak"));
  CHECK(!cxf::is_valid_display_text(std::string("\x1F", 1)));
  CHECK(!cxf::is_valid_display_text(std::string("\x7F", 1)));
  CHECK(!cxf::is_valid_display_text(std::string("\xC0\x80", 2)));

  CHECK_EQ(cxf::escape_for_output("plain"), "plain");
  CHECK_EQ(cxf::escape_for_output("a\nb\tc"), "a\\nb\\tc");
  CHECK_EQ(cxf::escape_for_output(std::string("\x01", 1)), "\\x01");
  CHECK_EQ(cxf::escape_for_output("back\\slash"), "back\\\\slash");
}

CXF_TEST(unit, text_integer_parsing) {
  const Outcome<std::uint64_t> zero = cxf::parse_u64("0");
  REQUIRE_OK(zero);
  CHECK_EQ(zero.value(), 0u);
  const Outcome<std::uint64_t> maximum = cxf::parse_u64("18446744073709551615");
  REQUIRE_OK(maximum);
  CHECK_EQ(maximum.value(), UINT64_MAX);
  CHECK_CODE(cxf::parse_u64(""), Code::kFieldEmpty);
  CHECK_CODE(cxf::parse_u64("00"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64("01"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64("+1"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64(" 1"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64("1 "), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64("1a"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_u64("18446744073709551616"), Code::kFieldOutOfRange);

  const Outcome<std::int64_t> negative = cxf::parse_i64("-9223372036854775808");
  REQUIRE_OK(negative);
  CHECK_EQ(negative.value(), INT64_MIN);
  const Outcome<std::int64_t> positive = cxf::parse_i64("9223372036854775807");
  REQUIRE_OK(positive);
  CHECK_EQ(positive.value(), INT64_MAX);
  const Outcome<std::int64_t> minus_zero = cxf::parse_i64("-0");
  REQUIRE_OK(minus_zero);
  CHECK_EQ(minus_zero.value(), 0);
  CHECK_CODE(cxf::parse_i64(""), Code::kFieldEmpty);
  CHECK_CODE(cxf::parse_i64("-"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_i64("+5"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_i64("007"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_i64("9223372036854775808"), Code::kFieldOutOfRange);
  CHECK_CODE(cxf::parse_i64("-9223372036854775809"), Code::kFieldOutOfRange);

  const Outcome<std::uint64_t> hex = cxf::parse_hex_u64("ff");
  REQUIRE_OK(hex);
  CHECK_EQ(hex.value(), 0xFFu);
  const Outcome<std::uint64_t> hex_max = cxf::parse_hex_u64("ffffffffffffffff");
  REQUIRE_OK(hex_max);
  CHECK_EQ(hex_max.value(), UINT64_MAX);
  CHECK_CODE(cxf::parse_hex_u64(""), Code::kFieldEmpty);
  CHECK_CODE(cxf::parse_hex_u64("0x10"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_hex_u64("AB"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_hex_u64("g"), Code::kMalformedInput);
  CHECK_CODE(cxf::parse_hex_u64("10000000000000000"), Code::kFieldOutOfRange);

  CHECK_EQ(cxf::to_hex(0), "0");
  CHECK_EQ(cxf::to_hex(0xDEADBEEFu), "deadbeef");
  CHECK_EQ(cxf::to_dec(42), "42");
  CHECK_EQ(cxf::to_dec_signed(-7), "-7");
  CHECK(cxf::equals_ascii("abc", "abc"));
  CHECK(!cxf::equals_ascii("abc", "ABC"));
  CHECK_EQ(cxf::upper_ascii("aZ09"), "AZ09");
  CHECK_EQ(cxf::trim_ascii("  x\t\r\n"), "x");
  const std::vector<std::string_view> parts = cxf::split("a//b", '/');
  CHECK_EQ(parts.size(), std::size_t{3});
  CHECK_EQ(parts[1], std::string_view{});
}

CXF_TEST(unit, text_safe_path_component) {
  CHECK(cxf::is_safe_path_component("node-1"));
  CHECK(cxf::is_safe_path_component("fabric.log"));
  CHECK(cxf::is_safe_path_component("a b"));
  CHECK(cxf::is_safe_path_component("caf\xC3\xA9"));
  CHECK(cxf::is_safe_path_component(std::string(96, 'x')));
  CHECK(!cxf::is_safe_path_component(""));
  CHECK(!cxf::is_safe_path_component("."));
  CHECK(!cxf::is_safe_path_component(".."));
  CHECK(!cxf::is_safe_path_component("a/b"));
  CHECK(!cxf::is_safe_path_component("a\\b"));
  CHECK(!cxf::is_safe_path_component("a:b"));
  CHECK(!cxf::is_safe_path_component("trailing."));
  CHECK(!cxf::is_safe_path_component("trailing "));
  CHECK(!cxf::is_safe_path_component(std::string("\x01", 1)));
  CHECK(!cxf::is_safe_path_component(std::string(97, 'x')));
  // Reserved device names stay reserved with any extension.
  CHECK(!cxf::is_safe_path_component("CON"));
  CHECK(!cxf::is_safe_path_component("con.txt"));
  CHECK(!cxf::is_safe_path_component("NUL"));
  CHECK(!cxf::is_safe_path_component("LPT9.dat"));
  CHECK(!cxf::is_safe_path_component("com1"));
  // A name that merely starts with a reserved word is not reserved.
  CHECK(cxf::is_safe_path_component("console.log"));
}

// ---------------------------------------------------------------------------
// Canonical serialisation.
// ---------------------------------------------------------------------------

CXF_TEST(unit, serial_primitive_round_trip) {
  std::vector<std::byte> payload = {std::byte{0x00}, std::byte{0xFF}, std::byte{0x7F}};
  SerialWriter writer;
  writer.u8(0xABu);
  writer.u16(0xBEEFu);
  writer.u32(0xDEADBEEFu);
  writer.u64(0x0123456789ABCDEFull);
  writer.i64(INT64_MIN);
  writer.boolean(true);
  writer.boolean(false);
  writer.text("");
  writer.text("hello");
  writer.text(std::string(cxf::kMaxTextFieldBytes, 't'));
  writer.blob(payload);
  writer.blob(std::span<const std::byte>());
  writer.digest_text(std::string(64, 'a'));

  SerialReader reader(writer.span());
  CHECK_EQ(reader.remaining(), writer.size());
  CHECK_EQ(reader.offset(), std::size_t{0});

  const Outcome<std::uint8_t> u8 = reader.u8();
  REQUIRE_OK(u8);
  CHECK_EQ(u8.value(), 0xABu);
  const Outcome<std::uint16_t> u16 = reader.u16();
  REQUIRE_OK(u16);
  CHECK_EQ(u16.value(), 0xBEEFu);
  const Outcome<std::uint32_t> u32 = reader.u32();
  REQUIRE_OK(u32);
  CHECK_EQ(u32.value(), 0xDEADBEEFu);
  const Outcome<std::uint64_t> u64 = reader.u64();
  REQUIRE_OK(u64);
  CHECK_EQ(u64.value(), 0x0123456789ABCDEFull);
  const Outcome<std::int64_t> i64 = reader.i64();
  REQUIRE_OK(i64);
  CHECK_EQ(i64.value(), INT64_MIN);
  const Outcome<bool> yes = reader.boolean();
  REQUIRE_OK(yes);
  CHECK_EQ(yes.value(), true);
  const Outcome<bool> no = reader.boolean();
  REQUIRE_OK(no);
  CHECK_EQ(no.value(), false);
  const Outcome<std::string> empty_text = reader.text();
  REQUIRE_OK(empty_text);
  CHECK_EQ(empty_text.value(), std::string{});
  const Outcome<std::string> text = reader.text();
  REQUIRE_OK(text);
  CHECK_EQ(text.value(), std::string("hello"));
  const Outcome<std::string> long_text = reader.text();
  REQUIRE_OK(long_text);
  CHECK_EQ(long_text->size(), cxf::kMaxTextFieldBytes);
  const Outcome<std::vector<std::byte>> blob = reader.blob();
  REQUIRE_OK(blob);
  CHECK_EQ(blob->size(), payload.size());
  CHECK(std::equal(blob->begin(), blob->end(), payload.begin()));
  const Outcome<std::vector<std::byte>> empty_blob = reader.blob();
  REQUIRE_OK(empty_blob);
  CHECK(empty_blob->empty());
  const Outcome<std::string> digest = reader.digest_text();
  REQUIRE_OK(digest);
  CHECK_EQ(digest.value(), std::string(64, 'a'));

  CHECK(reader.exhausted());
  CHECK_EQ(reader.remaining(), std::size_t{0});
  CHECK_CODE(reader.expect_exhausted("round trip"), Code::kOk);
}

CXF_TEST(unit, serial_bounds_and_hostile_lengths) {
  // A declared text length beyond the documented bound is refused before any
  // allocation happens.
  {
    SerialWriter writer;
    writer.text(std::string(cxf::kMaxTextFieldBytes + 1, 'x'));
    SerialReader reader(writer.span());
    CHECK_CODE(reader.text(), Code::kFieldTooLong);
  }
  // A declared length larger than the bytes actually present is a truncation,
  // never a short read that returns partial text.
  {
    SerialWriter writer;
    writer.u32(4);
    writer.u8('a');
    writer.u8('b');
    SerialReader reader(writer.span());
    CHECK_CODE(reader.text(), Code::kMalformedInput);
  }
  {
    SerialWriter writer;
    writer.u32(1);
    SerialReader reader(writer.span());
    CHECK_CODE(reader.text(), Code::kMalformedInput);
  }
  // A blob beyond the bound is refused too.
  {
    SerialWriter writer;
    writer.blob(std::vector<std::byte>(cxf::kMaxBlobFieldBytes + 1, std::byte{0x5A}));
    SerialReader reader(writer.span());
    CHECK_CODE(reader.blob(), Code::kFieldTooLong);
  }
  // Text that is not well-formed UTF-8 is refused even when the length is fine.
  {
    SerialWriter writer;
    writer.u32(2);
    writer.u8(0xC3);
    writer.u8(0x28);
    SerialReader reader(writer.span());
    CHECK_CODE(reader.text(), Code::kFieldInvalidUtf8);
  }
  // A boolean field is exactly one byte with the value 0 or 1.
  {
    SerialWriter writer;
    writer.u8(2);
    SerialReader reader(writer.span());
    CHECK_CODE(reader.boolean(), Code::kFieldOutOfRange);
  }
  // A digest field must be the 32 raw bytes; a poisoned length fails closed.
  {
    SerialWriter writer;
    writer.digest_text("not-a-digest");
    SerialReader reader(writer.span());
    CHECK_CODE(reader.digest_text(), Code::kMalformedInput);
  }
  {
    SerialWriter writer;
    writer.u32(31);
    writer.u8(0);
    SerialReader reader(writer.span());
    CHECK_CODE(reader.digest_text(), Code::kMalformedInput);
  }
  // Trailing bytes after a complete record are always an error.
  {
    SerialWriter writer;
    writer.u8(1);
    writer.u8(2);
    SerialReader reader(writer.span());
    const Outcome<std::uint8_t> first = reader.u8();
    REQUIRE_OK(first);
    CHECK_CODE(reader.expect_exhausted("trailing"), Code::kTrailingBytes);
    const Outcome<std::uint8_t> second = reader.u8();
    REQUIRE_OK(second);
    CHECK_CODE(reader.expect_exhausted("trailing"), Code::kOk);
  }
  // Reading past the end never advances the offset or fabricates a value.
  {
    SerialWriter writer;
    writer.u8(7);
    SerialReader reader(writer.span());
    CHECK_CODE(reader.u32(), Code::kMalformedInput);
    CHECK_CODE(reader.i64(), Code::kMalformedInput);
    CHECK_CODE(reader.u16(), Code::kMalformedInput);
    CHECK_EQ(reader.remaining(), std::size_t{1});
    CHECK_EQ(reader.offset(), std::size_t{0});
  }
}

CXF_TEST(unit, serial_endian_helpers) {
  std::byte buffer[8] = {};
  cxf::put_be16(buffer, 0x0102u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[0]), 0x01u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[1]), 0x02u);
  CHECK_EQ(cxf::get_be16(buffer), 0x0102u);

  cxf::put_be32(buffer, 0x01020304u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[0]), 0x01u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[3]), 0x04u);
  CHECK_EQ(cxf::get_be32(buffer), 0x01020304u);

  cxf::put_be64(buffer, 0x0102030405060708ull);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[0]), 0x01u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[7]), 0x08u);
  CHECK_EQ(cxf::get_be64(buffer), 0x0102030405060708ull);

  cxf::put_le32(buffer, 0x01020304u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[0]), 0x04u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[3]), 0x01u);
  CHECK_EQ(cxf::get_le32(buffer), 0x01020304u);

  cxf::put_le64(buffer, 0x0102030405060708ull);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[0]), 0x08u);
  CHECK_EQ(static_cast<std::uint8_t>(buffer[7]), 0x01u);
  CHECK_EQ(cxf::get_le64(buffer), 0x0102030405060708ull);
}

// ---------------------------------------------------------------------------
// Filesystem primitives.
// ---------------------------------------------------------------------------

CXF_TEST(unit, fs_validate_path) {
  CHECK_CODE(fs::validate_path("C:/store/journal.cxf"), Code::kOk);
  CHECK_CODE(fs::validate_path("C:\\store\\journal.cxf"), Code::kOk);
  CHECK_CODE(fs::validate_path("relative/store"), Code::kOk);
  CHECK_CODE(fs::validate_path(""), Code::kFieldEmpty);
  CHECK_CODE(fs::validate_path("a:b"), Code::kMalformedInput);
  CHECK_CODE(fs::validate_path("C:x"), Code::kMalformedInput);
  CHECK_CODE(fs::validate_path("C:/store/file:stream"), Code::kMalformedInput);
  CHECK_CODE(fs::validate_path(".."), Code::kMalformedInput);
  CHECK_CODE(fs::validate_path("a/../b"), Code::kMalformedInput);
  CHECK_CODE(fs::validate_path("a\\..\\b"), Code::kMalformedInput);
  // A NUL byte is not valid UTF-8, and the path validator checks that first.
  CHECK_CODE(fs::validate_path(std::string("a\0b", 3)), Code::kFieldInvalidUtf8);
  const std::string enormous(32001, 'x');
  CHECK_CODE(fs::validate_path(enormous), Code::kFieldTooLong);

  CHECK_EQ(fs::filename("C:/store/journal.cxf"), std::string_view("journal.cxf"));
  CHECK_EQ(fs::filename("plain"), std::string_view("plain"));
  CHECK_EQ(fs::parent("C:/store/journal.cxf"), std::string("C:/store"));
  CHECK_EQ(fs::parent("plain"), std::string{});
  CHECK_EQ(fs::join("C:/store", "journal.cxf"), std::string("C:/store/journal.cxf"));
  CHECK_EQ(fs::join("C:/store/", "journal.cxf"), std::string("C:/store/journal.cxf"));
  CHECK_EQ(fs::join("", "journal.cxf"), std::string("journal.cxf"));
}

CXF_TEST(unit, fs_atomic_write_publishes_exact_bytes) {
  cxf::test::TempDirectory directory("fs-atomic");
  REQUIRE(directory.ok());
  const std::string path = directory.child("payload.bin");
  const std::string payload = "payload-\x01\x02\xFF-bytes";

  REQUIRE_OK(fs::write_file_atomic(path, to_bytes(payload)));
  CHECK(cxf::test::path_exists(path));
  const Outcome<std::vector<std::byte>> read = fs::read_file(path, 4096);
  REQUIRE_OK(read);
  CHECK_EQ(from_bytes(read.value()), payload);
  const Outcome<std::uint64_t> size = fs::file_size(path);
  REQUIRE_OK(size);
  CHECK_EQ(size.value(), static_cast<std::uint64_t>(payload.size()));

  // The second publish replaces the first, and no staging file is left behind.
  const std::string replacement = "second generation";
  REQUIRE_OK(fs::write_file_atomic(path, to_bytes(replacement)));
  const Outcome<std::vector<std::byte>> reread = fs::read_file(path, 4096);
  REQUIRE_OK(reread);
  CHECK_EQ(from_bytes(reread.value()), replacement);
  for (const std::string& name : cxf::test::list_directory_names(directory.path())) {
    CHECK(!cxf::test::contains_substring(name, "stage"));
  }

  // An empty file is a legitimate publication.
  const std::string empty_path = directory.child("empty.bin");
  REQUIRE_OK(fs::write_file_atomic(empty_path, std::span<const std::byte>()));
  const Outcome<std::vector<std::byte>> empty_read = fs::read_file(empty_path, 4096);
  REQUIRE_OK(empty_read);
  CHECK(empty_read->empty());

  // A read bound smaller than the file is refused rather than truncated.
  CHECK_CODE(fs::read_file(path, 4), Code::kFieldTooLong);
  CHECK_CODE(fs::read_file(directory.child("absent.bin"), 4096), Code::kStorageUnavailable);
}

CXF_TEST(unit, fs_directory_listing_and_removal) {
  cxf::test::TempDirectory directory("fs-listing");
  REQUIRE(directory.ok());
  REQUIRE(cxf::test::write_file_text(directory.child("b.txt"), "b"));
  REQUIRE(cxf::test::write_file_text(directory.child("a.txt"), "a"));
  REQUIRE(cxf::test::write_file_text(directory.child("c.txt"), "c"));
  REQUIRE(cxf::test::ensure_directory(directory.child("nested/deep")));

  const Outcome<std::vector<std::string>> names = fs::list_directory(directory.path());
  REQUIRE_OK(names);
  CHECK_EQ(names->size(), std::size_t{4});
  CHECK_EQ((*names)[0], std::string("a.txt"));
  CHECK_EQ((*names)[1], std::string("b.txt"));
  CHECK_EQ((*names)[2], std::string("c.txt"));
  CHECK_EQ((*names)[3], std::string("nested"));
  for (const std::string& name : *names) {
    CHECK(name != ".");
    CHECK(name != "..");
  }

  const Outcome<bool> directory_exists = fs::is_directory(directory.child("nested/deep"));
  REQUIRE_OK(directory_exists);
  CHECK(directory_exists.value());
  const Outcome<bool> file_is_directory = fs::is_directory(directory.child("a.txt"));
  REQUIRE_OK(file_is_directory);
  CHECK(!file_is_directory.value());

  // Removal is idempotent: a crash-recovery path may remove twice.
  CHECK_CODE(fs::remove_file(directory.child("a.txt")), Code::kOk);
  CHECK(!cxf::test::path_exists(directory.child("a.txt")));
  CHECK_CODE(fs::remove_file(directory.child("a.txt")), Code::kOk);

  // A missing path is reported, not reported as size zero.
  CHECK(!fs::file_size(directory.child("absent.txt")).ok());
  CHECK(cxf::test::ensure_directory(directory.child("nested/deep")));
  CHECK(cxf::test::remove_tree(directory.child("nested")));
}

// ---------------------------------------------------------------------------
// The store lock.
// ---------------------------------------------------------------------------

CXF_TEST(unit, lock_file_excludes_a_second_holder) {
  cxf::test::TempDirectory directory("lock-exclusion");
  REQUIRE(directory.ok());
  const std::string lock_path = directory.child("store.lock");

  auto first = LockFile::acquire(lock_path, "unit-holder");
  REQUIRE_OK(first);
  CHECK(first->held());
  CHECK_EQ(first->path(), lock_path);

  const Outcome<std::string> info = first->holder_info();
  REQUIRE_OK(info);
  CHECK(cxf::test::contains_substring(info.value(), "unit-holder"));
  CHECK(cxf::test::contains_substring(info.value(), "pid="));

  // The holder text is informational and readable while the lock is held.
  const Outcome<std::string> peeked = LockFile::peek_holder(lock_path);
  REQUIRE_OK(peeked);
  CHECK(cxf::test::contains_substring(peeked.value(), "unit-holder"));
  CHECK(cxf::test::contains_substring(peeked.value(), "acquired="));

  // The exclusion is enforced by the operating system, so a second acquire by
  // this very process is refused.
  const Outcome<LockFile> second = LockFile::acquire(lock_path, "second-holder");
  CHECK_CODE(second, Code::kStorageLocked);

  CHECK_CODE(first->release(), Code::kOk);
  CHECK(!first->held());
  CHECK_CODE(first->release(), Code::kOk);

  auto third = LockFile::acquire(lock_path, "third-holder");
  REQUIRE_OK(third);
  CHECK(third->held());
  CHECK_CODE(third->release(), Code::kOk);
}

CXF_TEST(unit, lock_file_reports_a_missing_directory) {
  cxf::test::TempDirectory directory("lock-missing");
  REQUIRE(directory.ok());
  const Outcome<LockFile> lock = LockFile::acquire(directory.child("absent/store.lock"), "unit");
  REQUIRE(!lock.ok());
  CHECK(cxf::test::is_storage_failure(lock.code()));
}

CXF_TEST(unit, lock_file_holder_of_a_missing_file_is_an_error) {
  cxf::test::TempDirectory directory("lock-peek");
  REQUIRE(directory.ok());
  CHECK(!LockFile::peek_holder(directory.child("absent.lock")).ok());
}

}  // namespace
