// send.cpp — upstream builders (see send.h).
#include "send.h"

namespace rbx {
namespace send {

std::vector<uint8_t> heartbeat() { return {0xA6, 0x00}; }

} // namespace send
} // namespace rbx
