// raknet_protocol_test.cpp — the RakNet wire layer (raknet/protocol.h) and
// the 0xA6 input/ControlState framing (raknet/input.h).
//
// Every layout here is a native-captured fact from FINDINGS.md; the tests pin
// the encoders/decoders so a refactor cannot silently change a byte on the
// wire. No socket, no keys, no live session.
#include "raknet/ack83.h"
#include "raknet/input.h"
#include "raknet/join_msg.h"
#include "raknet/protocol.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace rbxclient;
using namespace rbxclient::raknet;

static int g_fail = 0;

static void check(bool ok, const std::string& what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) ++g_fail;
}

static std::string hex(const std::vector<uint8_t>& v) {
  std::string s;
  char b[4];
  for (uint8_t c : v) {
    snprintf(b, sizeof b, "%02x", c);
    s += b;
  }
  return s;
}

static std::vector<uint8_t> bytes(std::initializer_list<uint8_t> l) {
  return std::vector<uint8_t>(l);
}

int main() {
  // ---- 1. datagram headers ------------------------------------------------
  {
    const uint8_t data[] = {0x81, 0x05, 0x00, 0x00, 0x40, 0x08, 0x00};
    DatagramHeader h;
    check(parse_datagram_header(data, sizeof data, &h), "0x81 parses as a data datagram");
    check(h.kind == DatagramKind::Data && h.datagram_id == 5 && h.body == 4,
          "data datagram id is u24 LE and the body starts at 4");
    const uint8_t frag[] = {0x83, 0x01, 0x00, 0x00};
    check(parse_datagram_header(frag, sizeof frag, &h) &&
              h.kind == DatagramKind::Data && h.datagram_id == 1,
          "0x83 is a data datagram too (split-fragment form)");
    const uint8_t ack[] = {0xc0, 0x00, 0x01, 0x01, 0x07, 0x00, 0x00};
    check(parse_datagram_header(ack, sizeof ack, &h) && h.kind == DatagramKind::Ack &&
              h.body == 1,
          "0xc0 parses as an ACK with no datagram id");
    const uint8_t sack[] = {0xd0, 0x00, 0x01, 0x01, 0x07, 0x00, 0x00};
    check(parse_datagram_header(sack, sizeof sack, &h) && h.kind == DatagramKind::Ack,
          "0xd0 is the server ACK flag form");
    const uint8_t nak[] = {0xa0, 0x00, 0x01, 0x01, 0x07, 0x00, 0x00};
    check(parse_datagram_header(nak, sizeof nak, &h) && h.kind == DatagramKind::Nak,
          "0xa0 parses as a NAK");
    const uint8_t shortdg[] = {0x81, 0x00};
    check(!parse_datagram_header(shortdg, sizeof shortdg, &h),
          "a truncated data header is rejected");
    uint8_t out[8];
    check(build_datagram_header(out, sizeof out, kFlagData, 0x030201, true) == 4 &&
              out[0] == 0x81 && out[1] == 0x01 && out[2] == 0x02 && out[3] == 0x03,
          "header builder writes the u24 datagram id little-endian");
  }

  // ---- 2. message codec ---------------------------------------------------
  {
    const uint8_t payload[] = {0x83, 0x23, 0x01, 0x02};
    MessageSpec m;
    m.reliability = Reliability::ReliableOrdered;
    m.message_number = 0x010203;
    m.ordering_index = 0x040506;
    m.channel = 2;
    m.data = payload;
    m.size = sizeof payload;
    const std::vector<uint8_t> msg = build_message(m);
    // rel 0x60, bits 0x0020, msgno 03 02 01, ord 06 05 04, chan 02, payload.
    check(hex(msg) == "6000200302010605040283230102",
          "reliable-ordered message header is byte-exact");
    WalkResult w = walk_messages(msg.data(), msg.size(),
                                 [&](const MessageView& v) {
                                   check(v.reliability == Reliability::ReliableOrdered &&
                                             v.message_number == 0x010203 &&
                                             v.ordering_index == 0x040506 &&
                                             v.channel == 2,
                                         "walk_messages recovers the header fields");
                                   check(v.size == sizeof payload &&
                                             v.data[0] == 0x83 && v.data[3] == 0x02,
                                         "walk_messages returns the payload untouched");
                                   return true;
                                 });
    check(w.ok && w.messages == 1 && w.consumed == msg.size(),
          "one complete message walks exact-close");
  }

  // ---- 3. split header ----------------------------------------------------
  {
    const uint8_t payload[] = {0x97, 0xaa};
    MessageSpec m;
    m.reliability = Reliability::ReliableOrdered;
    m.split = true;
    m.message_number = 9;
    m.ordering_index = 3;
    m.split_count = 11;
    m.split_id = 2;
    m.split_index = 7;
    m.data = payload;
    m.size = sizeof payload;
    const std::vector<uint8_t> msg = build_message(m);
    // 0x70 (reliable-ordered + split), bits, msgno, ord, chan, count u32 BE,
    // split id u16 BE, index u32 BE.
    check(hex(msg) == "700010090000030000000000000b00020000000797aa",
          "split fragment carries count/id/index in network order");
    WalkResult w = walk_messages(msg.data(), msg.size(), [](const MessageView& v) {
      return v.split && v.split_count == 11 && v.split_id == 2 && v.split_index == 7;
    });
    check(w.ok && w.messages == 1, "split header round-trips");
  }

  // ---- 4. malformed messages never emit ----------------------------------
  {
    const uint8_t reserved[] = {0x21, 0x00, 0x08, 0x00};
    WalkResult w = walk_messages(reserved, sizeof reserved, [](const MessageView&) {
      return true;
    });
    check(!w.ok && w.bad_offset == 0, "reserved low reliability bits are rejected");
    const uint8_t truncated[] = {0x70, 0xff, 0xff};
    w = walk_messages(truncated, sizeof truncated, [](const MessageView&) { return true; });
    check(!w.ok && w.messages == 0, "a length field with no payload is rejected");
    const uint8_t empty[] = {0x60, 0x00, 0x08};
    w = walk_messages(empty, sizeof empty, [](const MessageView&) { return true; });
    check(!w.ok, "a reliable message with no message number is rejected");
  }

  // ---- 5. ACK / NAK -------------------------------------------------------
  {
    const std::vector<uint8_t> ack = build_ack_datagram(DatagramKind::Ack, {7, 1, 2, 3});
    check(hex(ack) == "c000020001000003000001070000",
          "ACK coalesces 1-3 into a range and 7 into a single");
    std::vector<AckRecord> recs;
    check(parse_ack_datagram(ack.data(), ack.size(), &recs) && recs.size() == 2 &&
              recs[0].lo == 1 && recs[0].hi == 3 && recs[1].single() &&
              recs[1].lo == 7,
          "our ACK form parses back");
    // The RCC's form: one single record, then bare u24 ids.
    const std::vector<uint8_t> server = bytes({0xd0, 0x00, 0x01, 0x01, 0x05, 0x00,
                                               0x00, 0x06, 0x00, 0x00});
    check(parse_ack_datagram(server.data(), server.size(), &recs) && recs.size() == 2 &&
              recs[0].lo == 5 && recs[1].lo == 6,
          "the server's trailing-u24 ACK form is understood");
    std::vector<uint32_t> ids;
    expand_acks(recs, &ids);
    check(ids == std::vector<uint32_t>({5, 6}), "expand_acks lists the ids");
    const std::vector<uint8_t> nak = build_ack_datagram(DatagramKind::Nak, {9});
    check(hex(nak) == "a0000101090000", "NAK uses flag 0xa0 and the same records");
    check(build_ack_datagram(DatagramKind::Data, {1}).empty(),
          "only ACK/NAK kinds can be built");
  }

  // ---- 6. packet vocabulary ----------------------------------------------
  {
    check(std::string(packet_name(0x13)) == "ID_REMOTE_SYSTEM_REQUIRES_PUBLIC_KEY",
          "0x13 is named by its stock id and documented as Roblox's 0x0b");
    check(std::string(packet_name(0x83)) == "APP_DATA", "0x83 names the data container");
    check(std::string(packet_name(0xa6)) == "APP_INPUT", "0xa6 names the input stream");
    check(packet_name(0x77) == nullptr, "an unassigned id has no invented name");
    check(is_internal_packet(0x7b) && !is_internal_packet(0x90),
          "internal/app classification");
    check(std::string(app_subtype_name(0x23)) == "clock", "0x83/0x23 is the clock item");
    check(app_subtype_name(0xfe) == nullptr, "unknown 0x83 subtype stays unnamed");
  }

  // ---- 7. census ----------------------------------------------------------
  {
    Census c;
    DatagramHeader h;
    const uint8_t data[] = {0x81, 0x01, 0x00, 0x00};
    parse_datagram_header(data, sizeof data, &h);
    c.note_datagram(h);
    c.note_app(0x83, true, 0x23);
    c.note_app(0x83, true, 0x02);
    c.note_internal(0x00);
    c.note_unknown(0x77, false);
    const std::string s = c.summary();
    check(s.find("dgrams=1") != std::string::npos &&
              s.find("app=83:2") != std::string::npos &&
              s.find("sub=02:1,23:1") != std::string::npos &&
              s.find("unknown=77:1") != std::string::npos &&
              s.find("unk_internal=1") != std::string::npos,
          "census reports every seen id and every unknown separately");
  }

  // ---- 8. bit stream ------------------------------------------------------
  {
    BitWriter w;
    w.put_bits(0b101, 3);
    w.put_u16_be(0xBEEF);
    w.put_u24_le(0x030201);
    w.put_u32_le(0x11223344);
    w.put_u64_be(0x0102030405060708ull);
    w.put_string("rbx");
    w.align();
    BitReader r(w.bytes().data(), w.bytes().size());
    uint64_t v = 0;
    check(r.get_bits(3, &v) && v == 0b101, "3-bit field round-trips");
    uint16_t u16 = 0;
    check(r.get_u16_be(&u16) && u16 == 0xBEEF, "u16 BE round-trips");
    uint32_t u32 = 0;
    check(r.get_u24_le(&u32) && u32 == 0x030201, "u24 LE round-trips");
    check(r.get_u32_le(&u32) && u32 == 0x11223344, "u32 LE round-trips");
    uint64_t u64 = 0;
    check(r.get_u64_be(&u64) && u64 == 0x0102030405060708ull, "u64 BE round-trips");
    std::string str;
    check(r.get_string(&str) && str == "rbx", "string is u16 BE length + bytes");
    check(r.align() && r.ok() && r.remaining_bits() == 0,
          "reader consumed exactly the aligned stream");
    const uint8_t too_short[] = {0x00};
    BitReader bad(too_short, 1);
    check(!bad.get_bits(9, &v) && !bad.ok(), "over-read fails closed");
  }

  // ---- 9. handshake reply1 ------------------------------------------------
  {
    std::vector<uint8_t> r1 = {0x7e, 0x00, 0xff, 0xff, 0x00, 0xfe, 0xfe, 0xfe,
                               0xfe, 0xfd, 0xfd, 0xfd, 0xfd, 0x12, 0x34, 0x56,
                               0x78, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                               0x08, 0x00, 0x04, 0xb0};
    Reply1 r;
    check(parse_reply1(r1.data(), r1.size(), &r) && r.guid == 0x0807060504030201ull &&
              r.mtu == 1200 && !r.security,
          "Reply1 guid/mtu decode from the native layout");
    r1[0] = 0x7d;
    check(!parse_reply1(r1.data(), r1.size(), &r), "a non-Reply1 id is rejected");
  }

  // ---- 10. 0xA6 input framing --------------------------------------------
  {
    const std::vector<uint8_t> f1 = input::routeFrame(1, 0x14c);
    check(hex(f1) == "a6020100014c0100004c0100000000040400",
          "the captured count=2 route frame is reproduced byte-for-byte");
    const std::vector<uint8_t> built = input::buildFrame(0x14c, 2, {}, false);
    check(built == f1, "buildFrame(count=2) equals the captured frame");
    const std::vector<uint8_t> f4 = input::routeFrame(3, 0x158);
    check(f4.size() == 34 && hex(f4).rfind("a604010010030064d0b7e6d7ff03e0", 0) == 0,
          "the count=4 frame keeps the native camera blob header");

    const std::vector<uint8_t> block = input::controlBlock(0x01, 0x01);
    check(hex(block) == "01000000000000000002000101",
          "one bool slot ControlState block is 13 bytes");
    const std::vector<uint8_t> with = input::withControl(f1, block);
    check(with.size() == f1.size() - 1 + block.size(),
          "withControl replaces the final nActions byte, not appends one");

    input::StreamState st;
    const std::vector<uint8_t> a = input::nextFrame(&st);
    const std::vector<uint8_t> b = input::nextFrame(&st);
    const std::vector<uint8_t> c = input::nextFrame(&st);
    const std::vector<uint8_t> d = input::nextFrame(&st);
    const std::vector<uint8_t> e = input::nextFrame(&st);
    check(a[1] == 1 && b[1] == 2 && c[1] == 3, "stream opens with counts 1,2,3");
    check(d[1] == 4 && e[1] == 4, "then settles on the count=4 steady state");
    check(st.step == 0x14c + 5 * 4, "step advances by 4 per message");

    // The 37-byte steady frame the native holds once the world is up. Golden
    // taken from the same-job 700 s capture (48,013 of 48,041 a6 frames).
    const std::vector<uint8_t> steady = input::steadyFrame(0x13c, 0x15104c);
    check(steady.size() == 37, "steady frame is 37 bytes");
    check(hex(steady) ==
              "a6040100" "10d9a47d9bbfd2a47d5527896f21"
              "3c010000" "4c101500" "0000" "040400" "040400" "040400",
          "steady frame reproduces the native 37-byte shape exactly");
  }

  // ---- 10b. 0x83/0x05 position-liveness frame ---------------------------
  {
    // Feed the captured clock/position back in: the frame must come out
    // byte-identical to the native's 45-byte report.
    float x = 0, z = 0, v = 0;
    const uint32_t bx = 0x4231f3b4, bz = 0x42286198, bv = 0x44833aa0;
    memcpy(&x, &bx, 4);
    memcpy(&z, &bz, 4);
    memcpy(&v, &bv, 4);
    const std::vector<uint8_t> ack = ack83::ack05(0x10dc, x, z, v, false);
    check(ack.size() == 45, "0x83/0x05 unreliable frame is 45 bytes");
    check(hex(ack) ==
              "830516dc10000000000000dc10000000000000b4f3314298612842"
              "000000000000000001" "db1000" "00" "a03a8344" "00",
          "0x83/0x05 reproduces the captured frame byte-for-byte");
    const std::vector<uint8_t> ackr = ack83::ack05(0x10dc, x, z, v, true);
    check(ackr.size() == 50, "reliable 0x83/0x05 is 50 bytes");
    check(hex(ackr).rfind(hex(ack).substr(0, 88), 0) == 0,
          "the reliable copy shares the 44-byte body");
    // The clock is a u24: 1,000 units/s, wrapping at 16.7 s of uptime.
    const std::vector<uint8_t> wrap = ack83::ack05(0xffffff, 0, 0, 0, false);
    check(wrap.size() == 45 && wrap[3] == 0xff && wrap[4] == 0xff &&
              wrap[5] == 0xff,
          "a u24 clock keeps only its low 24 bits");
  }

  // ---- 10c. 0x83/0x06 timing record -------------------------------------
  {
    // Byte-for-byte against a record lifted from the 700 s same-job native
    // capture (r700d), which is the shape 1,708 of that run's 3,091 records
    // share:
    //   83 06 01 67e557 0000000000 67e557 00
    //   3f1b0000 8e1b0000 ef1b0000 09000000 0e000000 96e4ffff
    //   00000000 ffffffff 01 00
    const std::vector<uint8_t> t =
        ack83::ack06(0x57e567, 0x1b3f, 0x1b8e, 0x1bef, 9, 14, -0x1b6a);
    check(t.size() == 49, "0x83/0x06 record is 49 bytes");
    check(hex(t) ==
              "830601" "67e557" "0000000000" "67e557" "00"
              "3f1b0000" "8e1b0000" "ef1b0000"
              "09000000" "0e000000" "96e4ffff"
              "00000000" "ffffffff" "01" "00",
          "0x83/0x06 reproduces the captured record byte-for-byte");
    // The process clock is read back at two widths: u64 at offset 3 and its
    // low 24 bits at offset 11. A u24 keeps only the low three bytes.
    const std::vector<uint8_t> wrap =
        ack83::ack06(0x1234567, 0, 0, 0, 0, 0, 0);
    check(wrap.size() == 49 && wrap[3] == 0x67 && wrap[4] == 0x45 &&
              wrap[5] == 0x23 && wrap[6] == 0x00,
          "a u24 process clock keeps only its low 24 bits");
  }

  // ---- 11. complete app-id coverage -------------------------------------
  {
    bool every_id = true;
    for (int raw = 0x81; raw <= 0xa9; ++raw) {
      if (!app_packet_info(uint8_t(raw))) every_id = false;
    }
    check(every_id, "every app id 0x81..0xa9 has a coverage entry");
    check(app_packet_info(0x83)->direction == AppDirection::Both &&
              app_can_receive(0x83) && app_can_send(0x83),
          "0x83 is bidirectional and has both codecs");
    check(app_packet_info(0xa1)->direction == AppDirection::Both &&
              app_can_receive(0xa1) && app_can_send(0xa1),
          "0xa1 camera/state is bidirectional");
    check(app_packet_info(0x8a)->direction == AppDirection::ClientToServer &&
              app_can_send(0x8a) && !app_can_receive(0x8a),
          "0x8a ticket is client-to-server with a builder");
    check(app_can_receive(0x93) && !app_can_send(0x93),
          "0x93 network settings has a receive decoder");
    check(app_packet_info(0xa4)->direction == AppDirection::Both &&
              !app_can_receive(0xa4) && !app_can_send(0xa4),
          "0xa4 is named but its codec is still unrecovered");
    for (uint8_t id : {0x87, 0x88, 0x8b, 0x8c}) {
      const AppPacketInfo* chat = app_packet_info(id);
      check(chat && std::string(chat->name) == "APP_CHAT" &&
                chat->direction == AppDirection::ServerToClient &&
                app_can_receive(id) && !app_can_send(id),
            "native chat id has a receive codec and no send builder");
    }
    check(!is_app_message(0xa0) && is_app_message(0xa9),
          "0xa0 NAK is excluded and 0xa9 compressed is in range");
    const std::string manifest = app_manifest();
    check(manifest.find("0x81") != std::string::npos &&
              manifest.find("0xa9") != std::string::npos &&
              manifest.find("(unobserved)") != std::string::npos,
          "manifest prints the whole range and marks unobserved ids");
  }

  // ---- 12. unknown app ids are counted, not hidden ----------------------
  {
    Census c;
    c.note_app(0xa5);
    const std::string s = c.summary();
    check(s.find("unk_app=1") != std::string::npos &&
              s.find("unknown=a5:1") != std::string::npos,
          "an unnamed app id increments unk_app and unknown");
    Census chat;
    chat.note_app(0x87);
    const std::string chat_summary = chat.summary();
    check(chat_summary.find("unk_app=0") != std::string::npos &&
              chat_summary.find("app=87:1") != std::string::npos &&
              chat_summary.find("unknown=") == std::string::npos,
          "a named and decoded chat app id is counted as known");
    check(std::string(app_direction_name(AppDirection::Both)) == "both",
          "direction names are stable");
  }

  // ---- 13. outbound handshake/app ack builders -------------------------
  {
    check(rbxmsg::build96() == std::vector<uint8_t>({0x96, 0x00}),
          "0x96 idle handshake is two bytes");
    check(rbxmsg::build96f() ==
              std::vector<uint8_t>({0x96, 0x01, 0x08, 0x00, 0x00, 0x00}),
          "0x96 flag form carries the u32 8 capability");
    check(rbxmsg::build99() ==
              std::vector<uint8_t>({0x99, 0x02, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00}),
          "0x99 capability report is the 14-byte zero form");
    check(rbxmsg::buildMarkerAck(0x12345678) ==
              std::vector<uint8_t>({0x04, 0x78, 0x56, 0x34, 0x12}),
          "0x04 marker ack echoes the u32 LE");
    const std::vector<uint8_t> a1 = rbxmsg::buildA1Camera(0x11223344, 0x0512b6);
    check(a1.size() == 44 && a1[0] == 0xa1 &&
              hex(std::vector<uint8_t>(a1.begin(), a1.begin() + 5)) ==
                  "a144332211",
          "0xA1 camera has the 44-byte frame and LE clock");
    check(a1[5] == 0x01 && a1[5 + 12] == 0x01 && a1[5 + 16] == 0x00 &&
              a1[5 + 13] == 0xb6 && a1[5 + 14] == 0x12 && a1[5 + 15] == 0x05,
          "0xA1 body preserves its captured fields and writes the u24 ref");
    const std::vector<uint8_t> a1b = rbxmsg::buildA1Camera(0, 0x010203);
    check(a1b[5 + 13] == 0x03 && a1b[5 + 14] == 0x02 && a1b[5 + 15] == 0x01,
          "0xA1 ref overwrite is little-endian");
    check(rbxmsg::buildA1Camera(0, 0x1000000).empty(),
          "0xA1 rejects a ref that does not fit the u24 slot");
    const uint8_t c2s[] = {0x83, 0x8a, 0x8f, 0x90, 0x92, 0x96,
                           0x99, 0x9b, 0xa1, 0xa6, 0xa7, 0xa8};
    bool all_send = true;
    for (uint8_t id : c2s)
      if (!app_can_send(id)) all_send = false;
    check(all_send, "every observed client-to-server app id has a builder");
  }

  // ---- 14. 0x93 network settings ---------------------------------------
  {
    const std::vector<uint8_t> m = bytes({0x93, 0x00, 0x01, 0x00, 0x03, 0x00,
                                          'a', 'b', 'c', 0x04, 0x00, 't',
                                          'r', 'u', 'e'});
    NetworkSettings s;
    std::string err;
    check(parseNetworkSettings(m.data(), m.size(), &s, &err) &&
              s.flags == 0 && s.settings.size() == 1 &&
              s.settings[0].name == "abc" && s.settings[0].value == "true",
          "0x93 count/name/value layout decodes");
    std::vector<uint8_t> stripped(m.begin() + 1, m.end());
    check(parseNetworkSettings(stripped.data(), stripped.size(), &s, &err) &&
              s.settings.size() == 1 && s.flags == 0,
          "0x93 live payload without the app id decodes");
    std::vector<uint8_t> bad = m;
    bad.push_back(0);
    check(!parseNetworkSettings(bad.data(), bad.size(), &s, &err),
          "0x93 trailing bytes fail exact-close");
    bad = m;
    bad[5] = 0xff; // value length overruns
    check(!parseNetworkSettings(bad.data(), bad.size(), &s, &err),
          "0x93 truncated value fails");
  }

  // ---- 15. 0xA9 LZ4 compressed app packet --------------------------------
  {
    // LZ4 block for "aaaaa": token 0x10 (1 literal, match length 4),
    // literal 'a', then a 1-byte back-reference.
    const std::vector<uint8_t> m =
        bytes({0xa9, 0x05, 0x10, 0x61, 0x01, 0x00});
    std::vector<uint8_t> out;
    std::string err;
    check(decompressCompressedApp(m.data(), m.size(), &out, &err) &&
              std::string(out.begin(), out.end()) == "aaaaa",
          "0xA9 LZ4 block decompresses with the native header");

    std::vector<uint8_t> hashed = m;
    hashed.push_back(0x00);
    hashed.push_back(0x00);
    check(decompressCompressedApp(hashed.data(), hashed.size(), &out, &err) &&
              std::string(out.begin(), out.end()) == "aaaaa",
          "0xA9 optional trailing u16 hash is tolerated");

    std::vector<uint8_t> stamp = {0x1b, 0, 0, 0, 0, 0, 0, 0, 0};
    stamp.insert(stamp.end(), m.begin(), m.end());
    check(decompressCompressedApp(stamp.data(), stamp.size(), &out, &err) &&
              std::string(out.begin(), out.end()) == "aaaaa",
          "0xA9 inside a 0x1B timestamp envelope decompresses");

    std::vector<uint8_t> truncated = m;
    truncated.pop_back();
    check(!decompressCompressedApp(truncated.data(), truncated.size(), &out, &err),
          "0xA9 truncated LZ4 block fails closed");
    std::vector<uint8_t> trailing = m;
    trailing.push_back(0x00);
    check(!decompressCompressedApp(trailing.data(), trailing.size(), &out, &err),
          "0xA9 rejects a non-hash trailing byte");
    check(app_can_receive(0xa9) && !app_can_send(0xa9),
          "0xA9 is a receive-decoded server wrapper");
  }

  // ---- 16. native chat receive -------------------------------------------
  {
    // sub_63C8D81 / sub_63CABA0: ULEB low + u32LE high, then u32LE message,
    // then up to two optional bytes. 0x8c inserts a target pair after the
    // sender; 0x8b has no sender pair at all.
    const std::vector<uint8_t> chat87 = bytes(
        {0x87, 0xb4, 0x24, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
         0x00, 'h', 'i', 0x01, 0x00});
    AppChat chat;
    std::string err;
    check(parseAppChat(chat87.data(), chat87.size(), &chat, &err) &&
              chat.id == 0x87 && chat.has_sender &&
              chat.sender_low == 0x1234 && chat.sender_high == 1 &&
              !chat.has_target && chat.message == "hi" &&
              chat.has_flag0 && chat.flag0 && chat.has_flag1 && !chat.flag1,
          "0x87 sender pair, message, and two optional bytes decode");

    const std::vector<uint8_t> chat88 = bytes(
        {0x88, 0x2a, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    check(parseAppChat(chat88.data(), chat88.size(), &chat, &err) &&
              chat.sender_low == 42 && chat.sender_high == 1 &&
              chat.message.empty() && !chat.has_flag0 && !chat.has_flag1,
          "0x88 accepts an empty message with no optional bytes");

    const std::vector<uint8_t> chat8b = bytes(
        {0x8b, 0x03, 0x00, 0x00, 0x00, 's', 'y', 's', 0x01});
    check(parseAppChat(chat8b.data(), chat8b.size(), &chat, &err) &&
              chat.id == 0x8b && !chat.has_sender &&
              chat.message == "sys" && chat.has_flag0 && chat.flag0 &&
              !chat.has_flag1,
          "0x8b skips the sender pair and accepts one optional byte");

    const std::vector<uint8_t> chat8c = bytes(
        {0x8c, 0x2a, 0x01, 0x00, 0x00, 0x00, 0x2b, 0x00, 0x00, 0x00,
         0x00, 0x02, 0x00, 0x00, 0x00, 'h', 'i', 0x00, 0x01});
    check(parseAppChat(chat8c.data(), chat8c.size(), &chat, &err) &&
              chat.id == 0x8c && chat.has_sender && chat.has_target &&
              chat.sender_low == 42 && chat.target_low == 43 &&
              chat.target_high == 0 && chat.message == "hi" &&
              chat.has_flag0 && !chat.flag0 && chat.has_flag1 && chat.flag1,
          "0x8c decodes sender and target pairs separately");

    std::vector<uint8_t> bad = chat87;
    bad[7] = 0x09; // message length overruns the body
    check(!parseAppChat(bad.data(), bad.size(), &chat, &err),
          "chat message length overrun fails closed");
    bad = chat87;
    bad.push_back(0x00);
    check(!parseAppChat(bad.data(), bad.size(), &chat, &err),
          "chat rejects a third trailing byte");
    bad = chat87;
    bad[0] = 0x89;
    check(!parseAppChat(bad.data(), bad.size(), &chat, &err),
          "chat parser rejects a non-chat id");

    std::vector<uint8_t> oversized(1011, 'x');
    oversized[0] = 0x87;
    check(!parseAppChat(oversized.data(), oversized.size(), &chat, &err),
          "chat body over the native 1010-byte limit fails closed");
  }

  // ---- 17. 0x86 instance-pair stream ------------------------------------
  {
    // Live 189707 vectors: version byte then whole 11-byte records. The 13B
    // form has one record; the 24B form has two (the byte after 0x86 is a
    // version, not a count).
    const std::vector<uint8_t> one = bytes(
        {0x86, 0x01, 0xa7, 0x9a, 0x2e, 0x00, 0x01, 0x12,
         0x9f, 0x2e, 0x00, 0x04, 0x00});
    const std::vector<uint8_t> two = bytes(
        {0x86, 0x01, 0x7a, 0x9b, 0x2e, 0x00, 0x01, 0x02, 0x9c, 0x2e,
         0x00, 0x01, 0x01, 0xd2, 0x9b, 0x2e, 0x00, 0x01, 0x02, 0x9c,
         0x2e, 0x00, 0x01, 0x00});
    std::vector<AppInstancePair> pairs;
    std::string err;
    check(parseAppInstancePairs(one.data(), one.size(), &pairs, &err) &&
              pairs.size() == 1 && pairs[0].ref_a == 0x2e9aa7 &&
              pairs[0].ref_kind == 1 && pairs[0].ref_b == 0x2e9f12 &&
              pairs[0].state == 4 && pairs[0].flag == 0,
          "0x86 one-record body decodes its two refs and state bytes");
    check(parseAppInstancePairs(two.data(), two.size(), &pairs, &err) &&
              pairs.size() == 2 && pairs[1].ref_a == 0x2e9bd2 &&
              pairs[1].ref_b == 0x2e9c02 && pairs[1].state == 1 &&
              pairs[1].flag == 0,
          "0x86 24B body is two records, not one counted record");
    std::vector<uint8_t> stripped(one.begin() + 1, one.end());
    check(parseAppInstancePairs(stripped.data(), stripped.size(), &pairs, &err) &&
              pairs.size() == 1,
          "0x86 accepts the live sink form without the app id");
    std::vector<uint8_t> bad = one;
    bad[1] = 0x02;
    check(!parseAppInstancePairs(bad.data(), bad.size(), &pairs, &err),
          "0x86 rejects an unknown version");
    bad = one;
    bad.pop_back();
    check(!parseAppInstancePairs(bad.data(), bad.size(), &pairs, &err),
          "0x86 rejects a partial record");
    bad = one;
    bad[6] = 0x02;
    check(!parseAppInstancePairs(bad.data(), bad.size(), &pairs, &err),
          "0x86 rejects an unknown reference kind");
    check(app_can_receive(0x86) && !app_can_send(0x86),
          "0x86 is now a receive-decoded server message");
  }

  printf("%s (%d failure%s)\n", g_fail ? "FAILED" : "passed", g_fail,
         g_fail == 1 ? "" : "s");
  return g_fail ? 1 : 0;
}
