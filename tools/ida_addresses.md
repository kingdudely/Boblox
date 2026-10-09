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
