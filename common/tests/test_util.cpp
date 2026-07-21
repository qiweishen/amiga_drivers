/// @file test_util.cpp
/// @brief Tests for the shared time/string/byte helpers and BoundedQueue.

#include <doctest/doctest.h>

#include <cstdint>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "bounded_queue.h"
#include "byte_util.h"
#include "string_util.h"
#include "time_util.h"


TEST_CASE("TimeUtil formatting") {
	using namespace Common::TimeUtil;
	// 2021-01-02 03:04:05.678 UTC
	constexpr std::uint64_t ns = 1609556645678000000ull;
	CHECK(Iso8601Utc(ns) == "2021-01-02T03:04:05.678Z");
	CHECK(CompactUtc(ns) == "20210102T030405Z");
	CHECK(std::regex_match(CompactUtcNow(), std::regex(R"(\d{8}T\d{6}Z)")));

	CHECK(HumanBytes(512) == "512 B");
	CHECK(HumanBytes(1536) == "1.50 KiB");
	CHECK(HumanBytes(1299227607ull) == "1.21 GiB");
	CHECK(HumanDuration(3723) == "01:02:03");

	const auto a = SteadyNowUs();
	const auto b = SteadyNowUs();
	CHECK(b >= a);
}


TEST_CASE("StringUtil basics") {
	using namespace Common::StringUtil;
	CHECK(Trim("  a b \t\n") == "a b");
	CHECK(Trim("") == "");
	CHECK(ToLower("AbC1") == "abc1");
	CHECK(EqualsCi("MAIN", "main"));
	CHECK_FALSE(EqualsCi("main", "aux"));
	CHECK(StartsWithCi("SensorDefault", "sensor"));
	CHECK_FALSE(StartsWithCi("abc", "abcd"));
	CHECK(Join({ "a", "b", "c" }, '+') == "a+b+c");
	CHECK(Join({}, '+') == "");
	CHECK(ToSnakeCase("Front Left") == "front_left");
	CHECK(ToSnakeCase("FrontLeft") == "front_left");
	CHECK(ToSnakeCase("front-left") == "front_left");
	CHECK(OneLine("a\r\nb\tc") == "a  b c");
}


TEST_CASE("Split keeps empties; SplitTrim uses getline semantics") {
	using namespace Common::StringUtil;
	// Raw split (ins401 wire/CSV semantics): empties preserved, "" -> [""]
	CHECK(Split("a,b,c", ',') == std::vector<std::string>{ "a", "b", "c" });
	CHECK(Split("a,,c", ',') == std::vector<std::string>{ "a", "", "c" });
	CHECK(Split("a,", ',') == std::vector<std::string>{ "a", "" });
	CHECK(Split("", ',') == std::vector<std::string>{ "" });
	// SplitTrim (asterx reply semantics): parts trimmed, trailing separator
	// yields no empty part, "" -> []
	CHECK(SplitTrim("a, b , c", ',') == std::vector<std::string>{ "a", "b", "c" });
	CHECK(SplitTrim("a,", ',') == std::vector<std::string>{ "a" });
	CHECK(SplitTrim("", ',').empty());
}


TEST_CASE("Base64Encode matches RFC 4648 vectors") {
	using Common::StringUtil::Base64Encode;
	CHECK(Base64Encode("") == "");
	CHECK(Base64Encode("f") == "Zg==");
	CHECK(Base64Encode("fo") == "Zm8=");
	CHECK(Base64Encode("foo") == "Zm9v");
	CHECK(Base64Encode("foobar") == "Zm9vYmFy");
	CHECK(Base64Encode("user:pass") == "dXNlcjpwYXNz");
}


TEST_CASE("ByteUtil round-trips and endianness") {
	using namespace Common::ByteUtil;
	const std::uint8_t big[] = { 0x12, 0x34, 0x56, 0x78 };
	CHECK(LoadBigU16(big) == 0x1234);
	CHECK(LoadBigU32(big) == 0x12345678u);
	CHECK(LoadLittleU16(big) == 0x3412);
	CHECK(LoadLittleU32(big) == 0x78563412u);

	std::uint8_t buf[8] = {};
	StoreBigU32(buf, 0xAABBCCDDu);
	CHECK(LoadBigU32(buf) == 0xAABBCCDDu);
	StoreLittleU32(buf, 0xAABBCCDDu);
	CHECK(LoadLittleU32(buf) == 0xAABBCCDDu);
	CHECK(buf[0] == 0xDD);
	StoreBigU16(buf, 0xBEEF);
	CHECK(LoadBigU16(buf) == 0xBEEF);
	StoreLittleU16(buf, 0xBEEF);
	CHECK(LoadLittleU16(buf) == 0xBEEF);
	const std::uint8_t l64[] = { 1, 0, 0, 0, 0, 0, 0, 0x80 };
	CHECK(LoadLittleU64(l64) == 0x8000000000000001ull);
}


TEST_CASE("BoundedQueue close-and-drain keeps tail items") {
	Common::BoundedQueue<int> q(4);
	CHECK(q.try_push(1));
	CHECK(q.try_push(2));
	CHECK(q.try_push(3));
	q.close();
	CHECK_FALSE(q.try_push(4));	 // closed: pushes fail
	int v = 0;
	CHECK(q.pop(v));
	CHECK(v == 1);
	CHECK(q.pop(v));
	CHECK(v == 2);
	CHECK(q.pop(v));
	CHECK(v == 3);
	CHECK_FALSE(q.pop(v));	// closed and drained
}


TEST_CASE("BoundedQueue capacity backpressure") {
	Common::BoundedQueue<int> q(2);
	CHECK(q.try_push(1));
	CHECK(q.try_push(2));
	CHECK_FALSE(q.try_push(3));	 // full: caller keeps the item
	CHECK_FALSE(q.push_wait_for(3, std::chrono::milliseconds(10)));
	// drop-oldest pattern (NTRIP): pop one to make room for the newest
	int dropped = 0;
	CHECK(q.try_pop(dropped));
	CHECK(dropped == 1);
	CHECK(q.try_push(3));
	int v = 0;
	CHECK(q.pop(v));
	CHECK(v == 2);
	CHECK(q.try_push(4));  // slot freed
	Common::BoundedQueue<int> empty_q(1);
	CHECK_FALSE(empty_q.try_pop(v));
}


TEST_CASE("BoundedQueue push_drop_oldest evicts atomically, refuses when closed") {
	Common::BoundedQueue<int> q(2);
	bool dropped = false;
	CHECK(q.push_drop_oldest(1, dropped));
	CHECK_FALSE(dropped);
	CHECK(q.push_drop_oldest(2, dropped));
	CHECK_FALSE(dropped);
	CHECK(q.push_drop_oldest(3, dropped));	// full: evicts 1
	CHECK(dropped);
	int v = 0;
	CHECK(q.pop(v));
	CHECK(v == 2);
	q.close();
	CHECK_FALSE(q.push_drop_oldest(4, dropped));  // closed: refused
	CHECK_FALSE(dropped);
	CHECK(q.pop(v));  // close-and-drain still delivers the tail
	CHECK(v == 3);
	CHECK_FALSE(q.pop(v));
}


TEST_CASE("BoundedQueue push_blocking wakes a concurrent consumer") {
	Common::BoundedQueue<int> q(1);
	CHECK(q.try_push(1));  // fill the queue so push_blocking must wait
	std::thread consumer([&q] {
		int out = 0;
		while (q.pop(out)) {
		}
	});
	CHECK(q.push_blocking(2));	// blocks until the consumer frees a slot
	q.close();
	consumer.join();
}
