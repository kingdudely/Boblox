// api_audit.cpp — see api_audit.h.
#include "instance/api_audit.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace rbx {
namespace audit {
namespace {

using Counts = std::map<std::string, uint64_t>;

std::mutex g_mu;
Counts g_missing_globals;
Counts g_missing_members;
Counts g_stub_calls;
Counts g_fallback_calls;
Counts g_event_connects;
Counts g_event_fires;

const char* output_path() {
    const char* p = std::getenv("RBX_API_AUDIT");
    return (p && *p) ? p : nullptr;
}

void bump(Counts& counts, const char* a, const char* b = nullptr) {
    if (!output_path())
        return;
    std::string key = a ? a : "?";
    if (b) {
        key += '.';
        key += b;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    ++counts[key];
}

void write_string(std::ostream& out, const std::string& s) {
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20)
                out << "\\u" << std::hex << (c < 0x10 ? "000" : "00")
                    << int(c) << std::dec;
            else
                out << char(c);
        }
    }
    out << '"';
}

void write_counts(std::ostream& out, const char* name, const Counts& counts,
                  bool comma) {
    out << "  ";
    write_string(out, name);
    out << ": {";
    bool first = true;
    for (const auto& kv : counts) {
        if (!first)
            out << ',';
        out << '\n' << "    ";
        write_string(out, kv.first);
        out << ": " << kv.second;
        first = false;
    }
    if (!first)
        out << '\n' << "  ";
    out << '}' << (comma ? "," : "") << '\n';
}

} // namespace

void note_global_missing(const char* name) {
    bump(g_missing_globals, name);
}

void note_member_missing(const char* cls, const char* member) {
    bump(g_missing_members, cls, member);
}

void note_stub_call(const char* cls, const char* member) {
    bump(g_stub_calls, cls, member);
}

void note_fallback_call(const char* cls, const char* member) {
    bump(g_fallback_calls, cls, member);
}

void note_event_connect(const char* name) {
    bump(g_event_connects, name);
}

void note_event_fire(const char* name) {
    bump(g_event_fires, name);
}

void dump() {
    const char* path = output_path();
    if (!path)
        return;

    Counts globals, members, stubs, fallbacks, connects, fires;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        globals = g_missing_globals;
        members = g_missing_members;
        stubs = g_stub_calls;
        fallbacks = g_fallback_calls;
        connects = g_event_connects;
        fires = g_event_fires;
    }

    std::error_code ec;
    const std::filesystem::path target(path);
    if (target.has_parent_path())
        std::filesystem::create_directories(target.parent_path(), ec);

    const std::filesystem::path tmp = target.string() + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "[api-audit] cannot write %s\n", path);
        return;
    }
    out << "{\n";
    write_counts(out, "missing_globals", globals, true);
    write_counts(out, "missing_members", members, true);
    write_counts(out, "stub_calls", stubs, true);
    write_counts(out, "fallback_calls", fallbacks, true);
    write_counts(out, "event_connects", connects, true);
    write_counts(out, "event_fires", fires, false);
    out << "}\n";
    out.close();
    if (!out) {
        std::fprintf(stderr, "[api-audit] write failed for %s\n", path);
        std::remove(tmp.string().c_str());
        return;
    }
    std::remove(path);
    if (std::rename(tmp.string().c_str(), path) != 0)
        std::fprintf(stderr, "[api-audit] cannot replace %s\n", path);
    else
        std::fprintf(stderr, "[api-audit] wrote %s\n", path);
}

} // namespace audit
} // namespace rbx
