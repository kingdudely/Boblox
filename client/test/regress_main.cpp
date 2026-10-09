// regress_main.cpp — C++ regression for the autonomous challenge path.
//
// Mirrors tools/regress9b.py: for every captured dataset, solve the wire
// challenge message with solveMessage() and compare against the answer the
// NATIVE itself sent. No native/gdb/oracle involved.
//
// Usage: regress9b [root]     (root defaults to /home/john/RobloxInBrowser)
#include "solve.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char* kDatasets[] = {"run/comb_1", "run/comb_2", "run/comb_3", "run/full", "run/rounds/r1"};

bool slurp(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// First file in dir matching prefix+suffix (mirrors glob("wire_chal*.bin")).
fs::path firstMatch(const fs::path& dir, const std::string& prefix, const std::string& suffix) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && n.size() >= suffix.size() &&
            n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0)
            return e.path();
    }
    return {};
}

std::string slurpText(const fs::path& p) {
    std::ifstream f(p);
    if (!f)
        return {};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
        s.pop_back();
    return s;
}

} // namespace

int main(int argc, char** argv) {
    std::string root = argc > 1 ? argv[1] : "/home/john/RobloxInBrowser";
    int total = 0, fails = 0;

    for (const char* rel : kDatasets) {
        fs::path d = fs::path(root) / rel;
        fs::path chalP = firstMatch(d, "wire_chal", ".bin");
        fs::path ansP = firstMatch(d, "wire_ans", ".bin");
        fs::path jobP = d / "jobid.txt";
        if (chalP.empty() || ansP.empty() || !fs::exists(jobP)) {
            printf("%-36s SKIP (missing capture)\n", d.c_str());
            continue;
        }

        std::vector<uint8_t> msg, ansRaw;
        if (!slurp(chalP, msg) || !slurp(ansP, ansRaw) || ansRaw.size() < 9) {
            printf("%-36s SKIP (unreadable capture)\n", d.c_str());
            continue;
        }
        std::string job = slurpText(jobP);
        uint32_t nativeAns = (uint32_t)ansRaw[5] | ((uint32_t)ansRaw[6] << 8) |
                             ((uint32_t)ansRaw[7] << 16) | ((uint32_t)ansRaw[8] << 24);

        total++;
        auto t0 = std::chrono::steady_clock::now();
        uint32_t got = 0;
        std::string err;
        bool ok = rbxclient::solveMessage(msg.data(), msg.size(), job, got, &err);
        auto dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                      .count();

        if (!ok) {
            printf("%-36s ERROR %s\n", d.c_str(), err.c_str());
            fails++;
            continue;
        }
        bool match = got == nativeAns;
        if (!match)
            fails++;
        printf("%-36s native=0x%08x got=0x%08x %s (%.0fms)\n", d.c_str(), nativeAns, got,
               match ? "MATCH" : "FAIL", dt);
    }

    printf("\n%d/%d match\n", total - fails, total);
    return fails ? 1 : 0;
}
