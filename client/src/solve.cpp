// solve.cpp — see solve.h; the sandbox lives in rbx_runtime (challenge_core).
#include "solve.h"

#include "blob.h"
#include "runner.h"
#include "standardize.h"

#include <cstring>
#include <vector>

namespace rbxclient {

namespace {

// Sandbox profile bit-exact vs native (FINDINGS.md):
//   tostring(UserSettings()) byte-sum 1924, os.exit missing (+9001),
//   IsStudio false (+1024), newproxy namecall ok (+52)
// (identical to py/solve9b.py SANDBOX.)
rbxch::Options productionOptions() {
    rbxch::Options o;
    o.usersettings = "ok";
    o.us_string = "aaaaaaaaaaaaaaaaaaaQ";
    o.os_exit = "missing";
    o.studio = "false";
    o.newproxy = "ok";
    return o;
}

} // namespace

bool solveMessage(const uint8_t* msg, size_t len, const std::string& job, uint32_t& answer,
                  std::string* err) {
    Challenge ch;
    if (!extractChallenge(msg, len, ch, err))
        return false;

    std::vector<uint8_t> wire;
    if (!decodeBlob(ch.blob.data(), ch.blob.size(), wire, err))
        return false;

    Stats st;
    if (!standardize(wire, &st, err))
        return false;

    rbxch::Options o = productionOptions();
    o.job = job;
    // Note arg order: runner --u1 = wire u2 field, runner --u2 = wire u1 field
    // (reconciled against native answers; see FINDINGS.md).
    o.u1 = ch.u2;
    o.u2 = ch.u1;

    rbxch::Result r = rbxch::run(wire.data(), wire.size(), o);
    if (r.status != rbxch::Status::Ok) {
        if (err)
            *err = (r.status == rbxch::Status::LoadFailed ? "load failed: " : "runtime error: ") +
                   r.error;
        return false;
    }
    answer = r.answer;
    return true;
}

std::vector<uint8_t> buildResponse(uint32_t u2, uint32_t answer) {
    std::vector<uint8_t> resp(9);
    resp[0] = 0x9B;
    resp[1] = (uint8_t)u2;
    resp[2] = (uint8_t)(u2 >> 8);
    resp[3] = (uint8_t)(u2 >> 16);
    resp[4] = (uint8_t)(u2 >> 24);
    resp[5] = (uint8_t)answer;
    resp[6] = (uint8_t)(answer >> 8);
    resp[7] = (uint8_t)(answer >> 16);
    resp[8] = (uint8_t)(answer >> 24);
    return resp;
}

} // namespace rbxclient
