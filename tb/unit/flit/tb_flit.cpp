// noc_pkg::flit_t is 204 bits and the C++ mirror (noc_flit.h) packs it bit-exactly.
#include <verilated.h>

#include <memory>
#include <random>

#include "Vtb_flit_top.h"
#include "noc_flit.h"
#include "tb_args.h"

using namespace torus;

int main(int argc, char** argv) {
  TbArgs args = TbArgs::parse(argc, argv, 10000);
  std::mt19937_64 rng(args.seed);
  auto ctx = std::make_unique<VerilatedContext>();
  auto dut = std::make_unique<Vtb_flit_top>(ctx.get());
  dut->eval();
  TB_CHECK(dut->bits_o == 204, "flit_t is %d bits, expected 204", dut->bits_o);
  for (uint64_t i = 0; i < args.cycles; i++) {
    Flit f;
    f.dst_x = rng() & 15; f.dst_y = rng() & 15; f.src_x = rng() & 15; f.src_y = rng() & 15;
    f.op = (Op)(rng() & 7); f.vc = rng() & 1; f.addr = (uint32_t)rng(); f.be = (uint16_t)rng();
    f.tag = (uint8_t)rng();
    for (auto& d : f.data) d = (uint32_t)rng();
    dut->dst_x_i = f.dst_x; dut->dst_y_i = f.dst_y; dut->src_x_i = f.src_x; dut->src_y_i = f.src_y;
    dut->op_i = (uint8_t)f.op; dut->vc_i = f.vc; dut->addr_i = f.addr; dut->be_i = f.be;
    dut->tag_i = f.tag;
    for (int k = 0; k < 4; k++) dut->data_i[k] = f.data[k];
    dut->eval();
    uint32_t w[Flit::kWords];
    f.pack(w);
    for (int k = 0; k < Flit::kWords; k++)
      TB_CHECK(dut->flit_o[k] == w[k], "word %d: rtl %08x c++ %08x (%s)", k, dut->flit_o[k], w[k],
               f.str().c_str());
    TB_CHECK(Flit::unpack(w) == f, "unpack(pack(f)) != f");
  }
  printf("tb_flit: PASS %llu random flits\n", (unsigned long long)args.cycles);
  return 0;
}
