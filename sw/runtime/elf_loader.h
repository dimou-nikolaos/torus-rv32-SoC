// Minimal ELF32 little-endian loader: returns the PT_LOAD segments of a RISC-V
// executable (file bytes plus zero fill up to p_memsz) and the symbol table.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace torus {

struct ElfSegment {
  uint32_t addr;
  std::vector<uint8_t> bytes;  // p_memsz bytes, zero-filled past p_filesz
};

struct ElfImage {
  uint32_t entry = 0;
  std::vector<ElfSegment> segments;
  std::map<std::string, uint32_t> symbols;
};

inline ElfImage load_elf(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + path);
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
  fclose(f);

  auto u16 = [&](size_t o) { return (uint32_t)d.at(o) | (uint32_t)d.at(o + 1) << 8; };
  auto u32 = [&](size_t o) { return u16(o) | u16(o + 2) << 16; };
  if (d.size() < 52 || memcmp(d.data(), "\x7f" "ELF", 4) || d[4] != 1 || d[5] != 1 || u16(18) != 243)
    throw std::runtime_error(path + ": not a little-endian ELF32 RISC-V file");

  ElfImage img;
  img.entry = u32(24);
  uint32_t phoff = u32(28), shoff = u32(32);
  uint32_t phentsize = u16(42), phnum = u16(44), shentsize = u16(46), shnum = u16(48);
  for (uint32_t i = 0; i < phnum; i++) {
    size_t p = phoff + i * phentsize;
    if (u32(p) != 1) continue;  // PT_LOAD
    uint32_t off = u32(p + 4), paddr = u32(p + 12), filesz = u32(p + 16), memsz = u32(p + 20);
    ElfSegment s{paddr, std::vector<uint8_t>(memsz, 0)};
    if (off + filesz > d.size()) throw std::runtime_error(path + ": truncated segment");
    memcpy(s.bytes.data(), d.data() + off, filesz);
    img.segments.push_back(std::move(s));
  }
  // Symbol table (for tohost and friends)
  for (uint32_t i = 0; i < shnum; i++) {
    size_t s = shoff + i * shentsize;
    if (u32(s + 4) != 2) continue;  // SHT_SYMTAB
    uint32_t off = u32(s + 16), size = u32(s + 20), link = u32(s + 24), entsize = u32(s + 36);
    size_t strtab = shoff + link * shentsize;
    uint32_t stroff = u32(strtab + 16);
    for (uint32_t k = 0; k < size / entsize; k++) {
      size_t e = off + k * entsize;
      const char* name = (const char*)d.data() + stroff + u32(e);
      if (*name) img.symbols[name] = u32(e + 4);
    }
  }
  return img;
}

}  // namespace torus
