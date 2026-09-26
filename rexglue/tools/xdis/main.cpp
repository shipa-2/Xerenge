// xdis: disassembles the title's guest image, naming what it calls.
//
//   xdis <start> <end> [--first]      addresses in hex; --first stops at the
//                                      first return (one function)
//
// Reads, relative to the project directory (or $XERENGE_PROJECT):
//   generated/xenos-scan/guest-image.bin   the image, based at 0x82000000
//   generated/retail-symbols.txt           names carried over from the beta
//   generated/default/burnout_init.cpp     the import thunks ({ 0x..., __imp__Name })
// A development tool; the game does not need it.
#include <capstone/capstone.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kImageBase = 0x82000000;

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
}

std::map<uint32_t, std::string> ReadNames(const std::string& root) {
  std::map<uint32_t, std::string> names;
  std::ifstream symbols(root + "/generated/retail-symbols.txt");
  for (std::string line; std::getline(symbols, line);) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    names[uint32_t(std::strtoul(line.c_str(), nullptr, 16))] = line.substr(tab + 1);
  }
  std::ifstream init(root + "/generated/default/burnout_init.cpp");
  for (std::string line; std::getline(init, line);) {
    const size_t at = line.find("{ 0x");
    const size_t imp = line.find("__imp__");
    if (at == std::string::npos || imp == std::string::npos) {
      continue;
    }
    const uint32_t address = uint32_t(std::strtoul(line.c_str() + at + 2, nullptr, 16));
    std::string name = line.substr(imp + 7);
    name = name.substr(0, name.find_first_of(" }"));
    names[address] = "<" + name + ">";
  }
  return names;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: xdis <start> <end> [--first]\n");
    return 2;
  }
  const uint32_t start = uint32_t(std::strtoul(argv[1], nullptr, 16));
  const uint32_t end = uint32_t(std::strtoul(argv[2], nullptr, 16));
  const bool first = argc > 3 && std::strcmp(argv[3], "--first") == 0;
  const char* project = std::getenv("XERENGE_PROJECT");
  const std::string root = project ? project : ".";

  const std::vector<uint8_t> image = ReadFile(root + "/generated/xenos-scan/guest-image.bin");
  if (image.empty() || start < kImageBase || end <= start ||
      end - kImageBase > image.size()) {
    std::fprintf(stderr, "xdis: no image, or the range is outside it\n");
    return 1;
  }
  const auto names = ReadNames(root);

  csh handle;
  if (cs_open(CS_ARCH_PPC, cs_mode(CS_MODE_32 | CS_MODE_BIG_ENDIAN), &handle) != CS_ERR_OK) {
    std::fprintf(stderr, "xdis: capstone would not open\n");
    return 1;
  }
  cs_option(handle, CS_OPT_SKIPDATA, CS_OPT_ON);
  cs_insn* insns = nullptr;
  const size_t count = cs_disasm(handle, image.data() + (start - kImageBase), end - start, start,
                                 0, &insns);
  for (size_t i = 0; i < count; ++i) {
    const cs_insn& insn = insns[i];
    std::printf("%08llX  %-8s %s", (unsigned long long)insn.address, insn.mnemonic, insn.op_str);
    if (!std::strcmp(insn.mnemonic, "bl") || !std::strcmp(insn.mnemonic, "b")) {
      const char* hex = std::strstr(insn.op_str, "0x");
      if (hex) {
        const auto found = names.find(uint32_t(std::strtoul(hex, nullptr, 16)));
        if (found != names.end()) {
          std::printf("    ; %s", found->second.c_str());
        }
      }
    }
    std::printf("\n");
    if (first && (!std::strcmp(insn.mnemonic, "blr") ||
                  (!std::strcmp(insn.mnemonic, "b") && std::strstr(insn.op_str, "0x8259bb")))) {
      break;
    }
  }
  cs_free(insns, count);
  cs_close(&handle);
  return 0;
}
