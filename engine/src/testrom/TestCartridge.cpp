#include "testrom/TestCartridge.h"

namespace rn {
namespace {
#include "testrom/TestCartridge.inc"
}  // namespace

std::vector<uint8_t> buildTestCartridge() {
  return std::vector<uint8_t>(kTestCartridgeRom, kTestCartridgeRom + sizeof kTestCartridgeRom);
}
}  // namespace rn
