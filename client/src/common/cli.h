// cli.h — the option parser the client apps share.
//
// Each app used to hand-roll the same loop: a `next()` lambda that pulled the
// following argv entry and a long if/else chain of string compares, once in
// raknet_main.cpp and again in play_main.cpp. The chain stays (each app's
// options are its own), but the tokenising, `--opt=value` handling and error
// wording live here once.
//
// Usage:
//   static const rbx::cli::Spec kSpec = rbx::cli::Spec()
//       .value("--place-id").flag("--headless");
//   const rbx::cli::Args args(argc, argv, kSpec);
//   if (!args.ok()) { fprintf(stderr, "%s\n", args.error().c_str()); return 2; }
//   const long long place = args.number("--place-id");
#pragma once

#include <cstdlib>
#include <map>
#include <string>
#include <utility>

namespace rbx {
namespace cli {

// Declared option set. Names are either switches (--headless, no value) or
// value options (--place-id 1818, also accepted as --place-id=1818).
class Spec {
 public:
  Spec& flag(const char* name) {
    kinds_[name] = false;
    return *this;
  }
  Spec& value(const char* name) {
    kinds_[name] = true;
    return *this;
  }
  bool declares(const std::string& name) const {
    return kinds_.find(name) != kinds_.end();
  }
  bool takesValue(const std::string& name) const {
    auto it = kinds_.find(name);
    return it != kinds_.end() && it->second;
  }

 private:
  std::map<std::string, bool> kinds_;
};

// Parsed view of argv[1..]. Construction stops at the first problem; ok()
// then reports it instead of the app crashing on a missing value.
class Args {
 public:
  Args(int argc, char** argv, const Spec& spec) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      // Every option is long-form; a bare word is a mistake (positional
      // arguments have never been accepted by any client app).
      if (a.rfind("--", 0) != 0) {
        fail("unknown arg " + a);
        return;
      }
      std::string name = a;
      std::string inlined;
      bool has_inline = false;
      if (const size_t eq = a.find('='); eq != std::string::npos) {
        name = a.substr(0, eq);
        inlined = a.substr(eq + 1);
        has_inline = true;
      }
      if (!spec.declares(name)) {
        fail("unknown arg " + a);
        return;
      }
      if (!spec.takesValue(name)) {
        if (has_inline) {
          fail(name + " takes no value");
          return;
        }
        values_[name] = "";
        continue;
      }
      if (has_inline) {
        values_[name] = inlined;
        continue;
      }
      if (i + 1 >= argc) {
        fail(name + " needs a value");
        return;
      }
      values_[name] = argv[++i];
    }
  }

  bool ok() const { return error_.empty(); }
  const std::string& error() const { return error_; }

  bool has(const char* name) const { return values_.find(name) != values_.end(); }

  std::string get(const char* name, const std::string& fallback = {}) const {
    auto it = values_.find(name);
    return it == values_.end() ? fallback : it->second;
  }

  // atoll/atoi semantics (0 for a non-numeric value), matching the parsers
  // this replaced.
  long long number(const char* name, long long fallback = 0) const {
    auto it = values_.find(name);
    if (it == values_.end() || it->second.empty()) return fallback;
    return strtoll(it->second.c_str(), nullptr, 10);
  }

 private:
  void fail(std::string e) {
    if (error_.empty()) error_ = std::move(e);
  }

  std::map<std::string, std::string> values_;
  std::string error_;
};

} // namespace cli
} // namespace rbx
