// Common testbench command line: every random test takes a seed, prints it, and
// can replay exactly from it (SPEC.md, Verification strategy).
//   --seed=N     (default: from time, printed)
//   --cycles=N   test length where applicable
//   --trace=FILE write an FST waveform
//   --quiet      less output
//   other --key[=value] options and positional arguments are kept for the test
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct TbArgs {
  uint64_t seed = 0;
  uint64_t cycles = 0;
  std::string trace;
  bool quiet = false;
  std::string plusargs;  // remaining +args, passed to Verilator
  std::vector<std::string> positional;
  std::map<std::string, std::string> opts;  // other --key[=value] options

  std::string opt(const std::string& k, const std::string& def = "") const {
    auto it = opts.find(k);
    return it == opts.end() ? def : it->second;
  }
  bool flag(const std::string& k) const { return opts.count(k) != 0; }

  static TbArgs parse(int argc, char** argv, uint64_t default_cycles) {
    TbArgs a;
    bool have_seed = false;
    a.cycles = default_cycles;
    for (int i = 1; i < argc; i++) {
      const char* s = argv[i];
      if (!strncmp(s, "--seed=", 7)) { a.seed = strtoull(s + 7, nullptr, 0); have_seed = true; }
      else if (!strncmp(s, "--cycles=", 9)) a.cycles = strtoull(s + 9, nullptr, 0);
      else if (!strncmp(s, "--trace=", 8)) a.trace = s + 8;
      else if (!strcmp(s, "--quiet")) a.quiet = true;
      else if (s[0] == '+') { a.plusargs += s; a.plusargs += ' '; }
      else if (!strncmp(s, "--", 2)) {
        const char* eq = strchr(s, '=');
        if (eq) a.opts[std::string(s + 2, eq)] = eq + 1;
        else a.opts[s + 2] = "";
      }
      else a.positional.push_back(s);
    }
    if (!have_seed) {
      if (const char* e = getenv("TORUS_SEED")) a.seed = strtoull(e, nullptr, 0);
      else a.seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    }
    printf("SEED=%llu\n", (unsigned long long)a.seed);
    return a;
  }
};

#define TB_CHECK(cond, ...)                                       \
  do {                                                            \
    if (!(cond)) {                                                \
      fprintf(stderr, "CHECK FAILED %s:%d: ", __FILE__, __LINE__); \
      fprintf(stderr, __VA_ARGS__);                               \
      fprintf(stderr, "\n");                                      \
      return 1;                                                   \
    }                                                             \
  } while (0)
