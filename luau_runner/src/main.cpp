// main.cpp — challenge_runner CLI: loads a decoded =challenge Luau bytecode
// program and computes the 0x9B answer for given (u1, u2, JobId).
//
// Usage:
//   challenge_runner --program=prog.bin --u1=0x12345678 --u2=0x9abcdef0 \
//                    --job=00000000-0000-0000-0000-000000000000 [options]
//
// Sandbox options (defaults chosen to mirror the production client):
//   --usersettings=error|ok     UserSettings() availability      [error]
//   --us-string=STR             tostring(UserSettings()) if ok   [UserSettings]
//   --os-exit=missing|ok|exit   os.exit behavior                 [missing]
//   --studio=false|true|error   RunService:IsStudio()            [false]
//   --newproxy=ok|error         newproxy availability            [ok]
//
// Prints: answer=<u32 hex> raw=<double>
//
// The actual sandbox/execution lives in runner.h (linked as challenge_core)
// so the C++ client can solve in-process; this file is only argument parsing.
#include "runner.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool read_file(const char* path, std::vector<char>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) {
        std::fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = std::fread(out.data(), 1, (size_t)n, f);
    std::fclose(f);
    return got == (size_t)n;
}

} // namespace

int main(int argc, char** argv) {
    rbxch::Options o;
    std::string program;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](const char* prefix) -> const char* {
            size_t n = std::strlen(prefix);
            return a.rfind(prefix, 0) == 0 ? argv[i] + n : nullptr;
        };
        if (const char* v = val("--program="))
            program = v;
        else if (const char* v = val("--u1="))
            o.u1 = (uint32_t)std::strtoul(v, nullptr, 0);
        else if (const char* v = val("--u2="))
            o.u2 = (uint32_t)std::strtoul(v, nullptr, 0);
        else if (const char* v = val("--job="))
            o.job = v;
        else if (const char* v = val("--usersettings="))
            o.usersettings = v;
        else if (const char* v = val("--us-string="))
            o.us_string = v;
        else if (const char* v = val("--os-exit="))
            o.os_exit = v;
        else if (const char* v = val("--studio="))
            o.studio = v;
        else if (const char* v = val("--newproxy="))
            o.newproxy = v;
        else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return 2;
        }
    }
    if (program.empty()) {
        std::fprintf(stderr, "missing --program\n");
        return 2;
    }

    std::vector<char> code;
    if (!read_file(program.c_str(), code)) {
        std::fprintf(stderr, "cannot read %s\n", program.c_str());
        return 2;
    }

    rbxch::Result r = rbxch::run(code.data(), code.size(), o);
    switch (r.status) {
    case rbxch::Status::LoadFailed:
        std::fprintf(stderr, "load failed: %s\n", r.error.c_str());
        return 3;
    case rbxch::Status::RuntimeError:
        std::fprintf(stderr, "runtime error: %s\n", r.error.c_str());
        return 4;
    case rbxch::Status::Ok:
        break;
    }
    std::printf("answer=%08x raw=%.17g\n", r.answer, r.raw);
    return 0;
}
