// Program load for the host programs, timed: --load=host (default) writes every
// tile's image through the I/O tile; --load=tree installs the resident loader
// (--loader=ELF, default loader.elf next to the program) and lets it copy the
// image tile to tile (SPEC.md, M4). The cycles cover the load only, from the
// first write until every image has landed; installing the loader is not
// counted (it happens once per session). A tree load is then read back and
// compared with the ELF.
#pragma once
#include <string>
#include <vector>

#include "sim_host.h"
#include "tb_args.h"
#include "torus.h"

namespace torus {

inline Program host_load(Device& dev, SimHost& sim, const TbArgs& args, const std::string& elf,
                         const std::vector<Xy>& tiles, uint64_t* cycles) {
  const std::string mode = args.opt("load", "host");
  if (mode != "host" && mode != "tree") throw std::runtime_error("--load must be host or tree");
  if (mode == "tree") {
    std::string dir = elf.find('/') == std::string::npos ? "." : elf.substr(0, elf.rfind('/'));
    dev.install_loader(args.opt("loader", dir + "/loader.elf"), tiles);
    dev.sync();
  }
  uint64_t t0 = sim.cycles();
  Program p = mode == "tree" ? dev.load_kernel_tree(elf, tiles) : dev.load_kernel(elf, tiles);
  dev.sync();
  *cycles = sim.cycles() - t0;
  // Tree loads are checked byte for byte (after the timing): every segment of
  // every tile, read back through the I/O tile
  if (mode == "tree") {
    std::string bad = dev.verify(p);
    if (!bad.empty()) throw std::runtime_error("tree load: " + bad);
  }
  return p;
}

}  // namespace torus
