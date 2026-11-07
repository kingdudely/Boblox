// unit_join.cpp — RakNet join-message helpers that are pure functions.
//
// The 0x8a ticket is sealed with a 16-char key built as
// "%d%d%d%d" % (placeId, -1733811612, tail2, 594292182) truncated to 16. The
// format is SIGNED, and that is not cosmetic: every place id at or above 2^31
// prints negative, and formatting it unsigned produced a different key for
// exactly those places. Their tickets were unopenable, so the RCC answered the
// join step with 0x15/0x0106 instead of the 0x9B challenge — a silent "this
// game never lets me in" that hit the largest places first (Da Hood 2788229376,
// Blox Fruits 2753915549, Pet Sim 99 3260590327, DOORS 6516141723). The key
// strings below are the ones those live joins actually sent.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "raknet/join_msg.h"

TEST_CASE("0x8a key: place ids below 2^31 keep their own digits") {
    CHECK(rbxmsg::key8a(1818u, 5u) == "1818-17338116125");
    CHECK(rbxmsg::key8a(155615604u, 5u) == "155615604-173381");
    CHECK(rbxmsg::key8a(606849621u, 5u) == "606849621-173381");
}

TEST_CASE("0x8a key: place ids at or above 2^31 print signed") {
    // 2788229376 - 2^32 = -1506737920 (Da Hood)
    CHECK(rbxmsg::key8a(2788229376u, 5u) == "-1506737920-1733");
    // 2753915549 - 2^32 = -1541051747 (Blox Fruits)
    CHECK(rbxmsg::key8a(2753915549u, 5u) == "-1541051747-1733");
    // 3260590327 - 2^32 = -1034376969 (Pet Sim 99)
    CHECK(rbxmsg::key8a(3260590327u, 5u) == "-1034376969-1733");
    // 6516141723 wraps to 2221174427 as u32, hence -2073792869 (DOORS)
    CHECK(rbxmsg::key8a(6516141723u, 5u) == "-2073792869-1733");
}

TEST_CASE("0x8a key is always 16 bytes") {
    for (uint32_t p : {1818u, 606849621u, 2147483648u, 4294967295u})
        CHECK(rbxmsg::key8a(p, 0u).size() == 16);
}
