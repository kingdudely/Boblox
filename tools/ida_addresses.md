# IDA address map — libroblox.so md5 892d0d3b04222497b4e69c2bc145ec6c (2.739.691)
# Runtime address = load_base + VA. Imagebase 0.

## RbxTransport transport layer
0x631BD58  send (NetStream send entry; app=rdx, chan=rcx, ns=r8)
0x631A38A  openSendChannel
0x63242AA  client openSendChannel
0x633D2E0  writeOpenUnreliable
0x6346AD4  OpenUnreliableChannelControl parser
0x632FA16  stream-header parser
0x632127A  onRecvCompletion (payload at r8, size rcx)
0x6321C80  recv state machine
0x6321040  resumeStreamRecv
0x6389464  Reset requested log path

## Join / session layer (client game connection)
0x32A0270  connection-accepted handler (sendEarlyAuthData + join sequencer)
0x32AC2D6  received-message dispatcher (cases 0x92/0x93/0x98/0x9A/0x9B...)
0x32A1C4C  sibling join path
0x329D040  isRbxTransportClient predicate (byte_7457938/byte_7430058)
0x35B024B  v9 constant = 0x63E25F26
0x35B00E5  OLLVM const-mixer
0x1DAE7B0  PCG random u32 (state qword_7CDBCB0)
0x69FB2B0  xxhash32(data,len,seed)
0x69F8696  raw append
0x69F895F  LEB128 unsigned append (inner fields)
0x69F8BAD  string append: LEB(len)+bytes
0x69F921C  read u32
0x69F9A5E  read string (u32 len + bytes)
0x69F84AC  skip N bytes
0x69F955A  read u8

## Message builders (client -> server)
0x329C8A8  0x90 client-info            (calls 0x33B55C2 for extra tail)
0x329D0B0  0x8A join request (ticket + session JSON + magic 0xC001CAFE)
0x329E134  0x92-nonce + triggers 8A+8F and post-join setup
0x329F040  0x8F
0x329F45C  0x92b/8A/8F batch (post queue)
0x32AECE2  0x9B challenge response [9b][u32][u32]
0x34F4204  0xA7 queue msg [a7][u16 count][16B records]
0x34F1F08  A7 queue filler (script hashes)
0x329DEE6  A7 send wrapper
0x33B55C2  session-data serializer (called from 0x8A builder tail)

## Challenge layer
0x435D53A  GenericChallengeService lookup ("challenge")
0x435D892  challenge parse/verify
0x2706206  blob transform wrapper
0x44AC4C4  blob transform core (flyweight assign)
0x2557590  flyweight init
0x32A7A2C  async task for challenge

## Constants
dword_71941A0 = 0x71375635  (compile-time seed "S")
0xC001CAFE  magic trailer of 0x8A (raw LE bytes fe ca 01 c0)
0x0BADF00D  second LEB value offset in 0x8A (v31 - 0x0BADF00D)
wire stream header: [06 01 app u8][chan u32be]
ctrl messages: OpenReliable [01 app chan u32be] / OpenUnreliable [02 app chan u32be wire u32be]
ctrl chan id 0x6374726C ('ctrl'), server ctrl stream header [06 01 00 FFFFFFFF]

## Replication dispatch (same md5; DB rebase differs per machine)
# Old VA (mocktail IDB) -> new VA (rr IDB). Tables verified identical.
INNER-DISP  0x3439982 -> 0x346CC44  (switch wire-1: 0-7,19,21 + tick35)
OUTER-DISP  0x32B3BA4 -> 0x32E4734  (21 dedicated cases + default->inner)
TRIPLE-03   0x3404858 -> 0x34374B2  ([ref][u16 prop][value])
VALUE-SW    0x340A3E4 -> 0x343D064  (72 value-tag cases)
TAG22-RD    (3x f32)  -> 0x36492A6  (tag 22 = 12B, all paths)
HDL-11      0x349BAF4 -> 0x34CF360  (0x11; guard + body call, no pre-read)
BODY-11     0x32BA84E -> 0x32EB3EA  (version-gated appearance data)
OUTER-11    (case 17) -> 0x34CF360
OUTER-0B    (case 11) -> 0x35702A2  (N-head 353C434-equivalent; TBD verify)
OUTER-0D    (case 13) -> 0x3572C38
OUTER-25    (case 37) -> 0x3572E0C
OUTER-10    (case 16) -> 0x34BD29A
OUTER-1B    (case 27) -> 0x3524514
OUTER-20    (case 32) -> 0x35F7F58
OUTER-16    (case 22) -> 0x34BFEC8
OUTER-29    (case 41) -> 0x35ADA3E
INNER-01    (case 0)  -> 0x34B4D02
INNER-02    (case 1)  -> 0x34C8F44  (spawn)
INNER-04    (case 3)  -> 0x34B9676
INNER-07    (case 6)  -> 0x34B5B68  (event)
INNER-13    (case 18) -> 0x34C4180
INNER-15    (case 20) -> 0x34A88F4

## Updated client 0.744.0.7441076 (md5 30ecaff9ec5b77a0a0faa7cf17573a3a)
# Runtime address = load_base + VA. Imagebase 0.
0x63F224E  outer 0x83 item dispatcher
0x651A030  inner item dispatcher
0x64CC280  0x03 prop triple reader (ctx+12480 -> schema object)
0x64D8394  prop loop ([u8 delta][value]* [ff])
0x64E9D50  value switch (72 tags)
0x65C578A  0x0B/0x25 batch head (zstd entry batch)
0x65C8292  0x25 body
0x65C80C8  0x25 element sentinel loop
0x6660A8C  0x25 element values
0x651756E  blob reader
0x6421A44  guid ref (lo 0/0xFFFFFFFF null)
0x63DC94E  connection event handler (message dispatch entry)
0x63D8F36  join broker (0x92 + 0x97 join-data task)
0x64F4A24  replicator creation
0x3407364  RbxTransport dummy-client send framer
0x340C96C  RbxTransport processPayload
# Schema object (from ctx+12480):
#   prop table   obj+208 / obj+216, stride 32, tag at entry+9
#   class table  obj+184 / obj+192, stride 224
#     entry+0 u16 class id, +2 u16 member_start, +4 u16 member_end
# 636 classes; member ranges tile 0..10301 exactly.
