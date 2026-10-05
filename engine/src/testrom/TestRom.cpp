#include "testrom/TestRom.h"

#include <map>
#include <stdexcept>
#include <string>

namespace rn {
namespace {

// Tiny label-resolving 6502 emitter. Each helper emits one instruction with its opcode byte.
class Asm {
 public:
  explicit Asm(uint16_t org) : org_(org) {}
  uint16_t pc() const { return uint16_t(org_ + code.size()); }
  void label(const std::string& l) { labels_[l] = pc(); }

  void imp(uint8_t op) { code.push_back(op); }
  void imm(uint8_t op, uint8_t v) { code.push_back(op); code.push_back(v); }
  void zp(uint8_t op, uint8_t a) { code.push_back(op); code.push_back(a); }
  void abs(uint8_t op, uint16_t a) { code.push_back(op); code.push_back(uint8_t(a)); code.push_back(uint8_t(a >> 8)); }
  void absL(uint8_t op, const std::string& l) { code.push_back(op); fix16_.push_back({code.size(), l}); code.push_back(0); code.push_back(0); }
  void br(uint8_t op, const std::string& l) { code.push_back(op); fix8_.push_back({code.size(), l}); code.push_back(0); }

  // Mnemonics used by the program.
  void SEI() { imp(0x78); }  void CLD() { imp(0xD8); }  void TXS() { imp(0x9A); }
  void DEX() { imp(0xCA); }  void RTI() { imp(0x40); }  void LSR_A() { imp(0x4A); }
  void PHA() { imp(0x48); }  void PLA() { imp(0x68); }  void TXA() { imp(0x8A); }  void TAX() { imp(0xAA); }
  void LDA_I(uint8_t v) { imm(0xA9, v); }  void LDX_I(uint8_t v) { imm(0xA2, v); }
  void AND_I(uint8_t v) { imm(0x29, v); }  void ORA_I(uint8_t v) { imm(0x09, v); }  void EOR_I(uint8_t v) { imm(0x49, v); }
  void LDA_Z(uint8_t a) { zp(0xA5, a); }   void STA_Z(uint8_t a) { zp(0x85, a); }   void ORA_Z(uint8_t a) { zp(0x05, a); }
  void EOR_Z(uint8_t a) { zp(0x45, a); }   void INC_Z(uint8_t a) { zp(0xE6, a); }   void ROL_Z(uint8_t a) { zp(0x26, a); }
  void LSR_Z(uint8_t a) { zp(0x46, a); }   void ROR_Z(uint8_t a) { zp(0x66, a); }
  void LDA_A(uint16_t a) { abs(0xAD, a); } void STA_A(uint16_t a) { abs(0x8D, a); } void BIT_A(uint16_t a) { abs(0x2C, a); }
  void JMP(const std::string& l) { absL(0x4C, l); }
  void BPL(const std::string& l) { br(0x10, l); }  void BNE(const std::string& l) { br(0xD0, l); }
  void BCC(const std::string& l) { br(0x90, l); }

  uint16_t addr(const std::string& l) const {
    auto it = labels_.find(l);
    if (it == labels_.end()) throw std::logic_error("undefined label " + l);
    return it->second;
  }
  void resolve() {
    for (auto& f : fix16_) { uint16_t a = addr(f.second); code[f.first] = uint8_t(a); code[f.first + 1] = uint8_t(a >> 8); }
    for (auto& f : fix8_) {
      int rel = int(addr(f.second)) - int(org_ + f.first + 1);
      if (rel < -128 || rel > 127) throw std::logic_error("branch out of range: " + f.second);
      code[f.first] = uint8_t(int8_t(rel));
    }
  }
  std::vector<uint8_t> code;

 private:
  uint16_t org_;
  std::map<std::string, uint16_t> labels_;
  std::vector<std::pair<size_t, std::string>> fix16_, fix8_;
};

void readPad(Asm& a, uint16_t port, uint8_t zpDst, const std::string& lbl) {
  a.LDX_I(8);
  a.label(lbl);
  a.LDA_A(port); a.LSR_A(); a.ROL_Z(zpDst); a.DEX(); a.BNE(lbl);
}

}  // namespace

std::vector<uint8_t> buildTestRom() {
  Asm a(0xC000);

  // ------------------------------------------------------------- RESET
  a.label("reset");
  a.SEI(); a.CLD();
  a.LDX_I(0xFF); a.TXS();
  a.LDA_I(0x00); a.STA_A(0x2000); a.STA_A(0x2001); a.STA_A(0x4010);
  a.LDA_I(0x40); a.STA_A(0x4017);                      // frame IRQ inhibit
  a.label("vw1"); a.BIT_A(0x2002); a.BPL("vw1");     // PPU warm-up: two vblanks
  a.label("vw2"); a.BIT_A(0x2002); a.BPL("vw2");
  a.INC_Z(ZP_RESETS);
  a.LDA_I(0x0F); a.STA_A(0x4015);                      // pulse1, pulse2, triangle, noise
  a.LDA_I(0xBF); a.STA_A(0x4000);                      // pulse1: duty 2, halt, const vol 15
  a.LDA_I(0x08); a.STA_A(0x4001);                      // no sweep
  a.LDA_I(0x7A); a.STA_A(0x4004);                      // pulse2: duty 1, halt, const vol 10
  a.LDA_I(0x08); a.STA_A(0x4005);
  a.LDA_I(0x01); a.STA_A(0x4007);
  a.LDA_I(0xFF); a.STA_A(0x4008);                      // triangle linear counter (control)
  a.LDA_I(0x36); a.STA_A(0x400C);                      // noise: halt, const vol 6
  a.LDA_I(0x80); a.STA_A(0x2000);                      // NMI on
  a.label("idle"); a.JMP("idle");

  // ------------------------------------------------------------- NMI
  a.label("nmi");
  a.PHA(); a.TXA(); a.PHA();
  a.LDA_I(1); a.STA_A(0x4016); a.LDA_I(0); a.STA_A(0x4016);   // strobe both pads
  readPad(a, 0x4016, ZP_PAD1, "rp1");
  readPad(a, 0x4017, ZP_PAD2, "rp2");
  // LFSR: never let it stick at zero.
  a.LDA_Z(ZP_SEED_LO); a.ORA_Z(ZP_SEED_HI); a.BNE("seeded"); a.INC_Z(ZP_SEED_LO);
  a.label("seeded");
  a.LSR_Z(ZP_SEED_HI); a.ROR_Z(ZP_SEED_LO); a.BCC("noxor");
  a.LDA_Z(ZP_SEED_HI); a.EOR_I(0xB4); a.STA_Z(ZP_SEED_HI);
  a.label("noxor");
  a.LDA_Z(ZP_SEED_LO); a.EOR_Z(ZP_PAD1); a.STA_Z(ZP_SEED_LO);    // mix input into RNG
  a.LDA_Z(ZP_SEED_HI); a.EOR_Z(ZP_PAD2); a.STA_Z(ZP_SEED_HI);
  a.INC_Z(ZP_FC); a.BNE("nofc2"); a.INC_Z(ZP_FC2);
  a.label("nofc2");
  // Nametable write: $2000 | (fc2&3)<<8 | fc  <- seed_lo
  a.LDA_A(0x2002);
  a.LDA_Z(ZP_FC2); a.AND_I(0x03); a.ORA_I(0x20); a.STA_A(0x2006);
  a.LDA_Z(ZP_FC); a.STA_A(0x2006);
  a.LDA_Z(ZP_SEED_LO); a.STA_A(0x2007);
  // Palette $3F00..$3F03
  a.LDA_I(0x3F); a.STA_A(0x2006); a.LDA_I(0x00); a.STA_A(0x2006);
  a.LDA_Z(ZP_SEED_LO); a.AND_I(0x3F); a.STA_A(0x2007);
  a.LDA_Z(ZP_PAD1); a.AND_I(0x3F); a.STA_A(0x2007);
  a.LDA_Z(ZP_SEED_HI); a.AND_I(0x3F); a.STA_A(0x2007);
  a.LDA_Z(ZP_RESETS); a.AND_I(0x3F); a.STA_A(0x2007);
  // Scroll / control / mask
  a.LDA_I(0x00); a.STA_A(0x2005); a.STA_A(0x2005);
  a.LDA_I(0x80); a.STA_A(0x2000);
  a.LDA_I(0x0A); a.STA_A(0x2001);
  // Audio
  a.LDA_Z(ZP_SEED_LO); a.STA_A(0x4002);
  a.LDA_Z(ZP_FC); a.AND_I(0x07); a.BNE("nohi");
  a.LDA_Z(ZP_SEED_HI); a.AND_I(0x07); a.STA_A(0x4003);
  a.label("nohi");
  a.LDA_Z(ZP_PAD1); a.ORA_I(0x40); a.STA_A(0x4006);
  a.LDA_Z(ZP_SEED_HI); a.STA_A(0x400A);
  a.LDA_I(0x01); a.STA_A(0x400B);
  a.LDA_Z(ZP_SEED_LO); a.AND_I(0x0F); a.STA_A(0x400E);
  a.LDA_I(0x08); a.STA_A(0x400F);
  a.PLA(); a.TAX(); a.PLA();
  a.RTI();

  a.label("irq");
  a.RTI();
  a.resolve();

  const size_t PRG = 16384, CHR = 8192;
  std::vector<uint8_t> rom(16 + PRG + CHR, 0);
  const uint8_t header[16] = {'N', 'E', 'S', 0x1A, 1, 1, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
  std::copy(header, header + 16, rom.begin());
  uint8_t* prg = rom.data() + 16;
  std::fill(prg, prg + PRG, 0xEA);  // NOP fill
  std::copy(a.code.begin(), a.code.end(), prg);
  auto vec = [&](size_t off, uint16_t v) { prg[off] = uint8_t(v); prg[off + 1] = uint8_t(v >> 8); };
  vec(0x3FFA, a.addr("nmi"));
  vec(0x3FFC, a.addr("reset"));
  vec(0x3FFE, a.addr("irq"));
  // CHR: 256 distinct 2bpp tiles generated arithmetically.
  uint8_t* chr = prg + PRG;
  for (int t = 0; t < 256; ++t)
    for (int r = 0; r < 8; ++r) {
      chr[t * 16 + r] = uint8_t((t * 37 + r * 11) ^ (r * t));
      chr[t * 16 + 8 + r] = uint8_t((t * 13) ^ (r * 29) ^ (t >> 1));
    }
  return rom;
}

}  // namespace rn
