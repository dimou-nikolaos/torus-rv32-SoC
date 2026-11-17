// C++ mirror of noc_pkg::flit_t (204 bits) and link fields. Packing is checked
// against the RTL by tb_flit, so every NoC testbench can rely on it.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace torus {

enum class Op : uint8_t { Write = 0, ReadReq = 1, AtomicInc = 2, Multicast = 3 };
enum class Port : uint8_t { Local = 0, X = 1, Y = 2 };

struct Flit {
  uint8_t dst_x = 0, dst_y = 0, src_x = 0, src_y = 0;
  Op op = Op::Write;
  uint8_t vc = 0;
  uint32_t addr = 0;
  uint16_t be = 0;
  uint8_t tag = 0;
  uint32_t data[4] = {};  // data[0] = bits [31:0]

  static constexpr int kBits = 204;
  static constexpr int kWords = 7;

  // Packed layout, LSB first: data[127:0], tag, be, addr, vc, op, src_y, src_x, dst_y, dst_x
  void pack(uint32_t* w) const {
    memset(w, 0, kWords * 4);
    int pos = 0;
    auto put = [&](uint64_t v, int n) {
      for (int i = 0; i < n; i++, pos++)
        if (v >> i & 1) w[pos / 32] |= 1u << (pos % 32);
    };
    for (int i = 0; i < 4; i++) put(data[i], 32);
    put(tag, 8);
    put(be, 16);
    put(addr, 32);
    put(vc, 1);
    put((uint8_t)op, 3);
    put(src_y, 4);
    put(src_x, 4);
    put(dst_y, 4);
    put(dst_x, 4);
  }

  static Flit unpack(const uint32_t* w) {
    Flit f;
    int pos = 0;
    auto get = [&](int n) {
      uint64_t v = 0;
      for (int i = 0; i < n; i++, pos++) v |= (uint64_t)(w[pos / 32] >> (pos % 32) & 1) << i;
      return v;
    };
    for (int i = 0; i < 4; i++) f.data[i] = (uint32_t)get(32);
    f.tag = (uint8_t)get(8);
    f.be = (uint16_t)get(16);
    f.addr = (uint32_t)get(32);
    f.vc = (uint8_t)get(1);
    f.op = (Op)get(3);
    f.src_y = (uint8_t)get(4);
    f.src_x = (uint8_t)get(4);
    f.dst_y = (uint8_t)get(4);
    f.dst_x = (uint8_t)get(4);
    return f;
  }

  bool operator==(const Flit& o) const {
    return dst_x == o.dst_x && dst_y == o.dst_y && src_x == o.src_x && src_y == o.src_y &&
           op == o.op && vc == o.vc && addr == o.addr && be == o.be && tag == o.tag &&
           memcmp(data, o.data, sizeof data) == 0;
  }

  std::string str() const {
    char b[128];
    snprintf(b, sizeof b, "(%u,%u)->(%u,%u) op%u addr=%08x tag=%02x d0=%08x", src_x, src_y,
             dst_x, dst_y, (unsigned)op, addr, tag, data[0]);
    return b;
  }
};

}  // namespace torus
