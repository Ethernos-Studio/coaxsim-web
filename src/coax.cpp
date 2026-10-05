// 真 · CVBS 同轴传输模拟核心（WASM 版）v2 —— 支持 SD(PAL/NTSC) 与 HD(720p/1080p) 栅格
#include <cstdint>
#include <cmath>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

struct Std {
  double fsc;
  int lines, vbi, active;
  double lineUs, blankUs, frontUs;
  bool vFlip, burstAlt;
  double vsUs, longUs;      // 场同步长脉冲宽度 / 场同步搜索窗
};
const Std STDS[] = {
  // PAL 625
  {  4433618.75,  625, 49,  576, 64.0,  11.95, 1.65, true,  true,  30.0, 26.0 },
  // NTSC 525
  {  3579545.00,  525, 45,  480, 63.556,10.9,  1.5,  false, false, 30.0, 26.0 },
  // HD-720p（PAL 彩色，fsc=4×3.5795MHz，~57MHz 采样）
  { 14318181.82,  750, 30,  720, 33.333,11.0,  1.2,  true,  true,  15.0, 13.0 },
  // HD-1080p（PAL 彩色，fsc=6×3.5795MHz，~86MHz 采样）
  { 21477272.73, 1125, 45, 1080, 29.630, 7.3,  1.0,  true,  true,  13.0, 11.5 },
};

class CoaxSim {
public:
  CoaxSim(int stdIdx, int fsMul) : s_(STDS[stdIdx]), fsMul_(fsMul) {
    fs_ = s_.fsc * fsMul_;
    SPL_ = (int)std::lround(fs_ * s_.lineUs / 1e6);
    spF_ = (int)std::lround(s_.frontUs * 1e-6 * fs_);
    spS_ = (int)std::lround(4.7e-6 * fs_);
    aStart_ = (int)std::lround(s_.blankUs * 1e-6 * fs_);
    nact_ = SPL_ - aStart_;
    ghostD_ = std::max(2, (int)std::lround(140e-9 * fs_));   // 140ns 回波
    const size_t N = (size_t)SPL_ * s_.lines + 64;
    tx_.assign(N, 0.3f); rx_.assign(N, 0.0f);
    sLine_.resize(nact_); Uline_.resize(nact_); Vline_.resize(nact_);
    maU_.assign(fsMul_, 0.0f); maV_.assign(fsMul_, 0.0f);
    gbuf_.assign(ghostD_, 0.0f);
    SINT_.resize(fsMul_); COST_.resize(fsMul_);
    for (int k = 0; k < fsMul_; k++) {
      SINT_[k] = (float)std::sin(2 * PI * k / fsMul_);
      COST_[k] = (float)std::cos(2 * PI * k / fsMul_);
    }
  }

  int width() const { return nact_; }
  int height() const { return s_.active; }
  int jitNs() const { return jitNs_; }
  int SPL() const { return SPL_; }
  float* tx() { return tx_.data(); }
  float* rx() { return rx_.data(); }

  // WAV 导入模式：tx 已由外部填充，只做信道 + 解码
  void processPreload(uint8_t* out,
                      float len, float noise, float phaseDeg, float ghost, float hum,
                      int hbw, uint32_t frame) {
    rng_ = 0x9E3779B97F4A7C15ULL * (frame + 1);
    channel(len, noise, ghost, hum, hbw);
    decode(phaseDeg, out);
  }
  // WAV 导出快速通道：只编码 + 信道，不解码
  void encodeChannelOnly(const uint8_t* src, float len, float noise, float ghost, float hum,
                         int hbw, uint32_t frame) {
    rng_ = 0x9E3779B97F4A7C15ULL * (frame + 1);
    encode(src);
    channel(len, noise, ghost, hum, hbw);
  }
  void process(const uint8_t* src, uint8_t* out,
               float len, float noise, float phaseDeg, float ghost, float hum,
               int hbw, uint32_t frame) {
    rng_ = 0x9E3779B97F4A7C15ULL * (frame + 1);
    encode(src);
    channel(len, noise, ghost, hum, hbw);
    decode(phaseDeg, out);
  }

private:
  Std s_;
  int fsMul_;
  double fs_;
  int SPL_, spF_, spS_, aStart_, nact_, ghostD_;
  std::vector<float> tx_, rx_, sLine_, Uline_, Vline_, maU_, maV_, SINT_, COST_, gbuf_;
  int vPhase_ = 0, dLast_ = 0;
  float lpY_ = 0, lpCh_ = 0, humAcc_ = 0;
  int jitNs_ = 0;
  uint64_t rng_ = 1;

  float frand() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17;
    return (float)((rng_ >> 40) & 0xFFFFFF) / 16777216.0f;
  }

  void encode(const uint8_t* src) {
    const int M = fsMul_ - 1;
    const int bl = 10 * fsMul_;
    const int vsW = (int)(s_.vsUs * 1e-6 * fs_);
    for (int l = 0; l < s_.lines; l++) {
      const int b = l * SPL_;
      std::fill(tx_.begin() + b, tx_.begin() + b + SPL_, 0.3f);
      if (l < 5) std::fill(tx_.begin() + b, tx_.begin() + b + vsW, 0.0f);
      else       std::fill(tx_.begin() + b + spF_, tx_.begin() + b + spF_ + spS_, 0.0f);
      if (l < s_.vbi) continue;
      const int bs = b + spF_ + spS_;
      const double ph = s_.burstAlt ? ((l & 1) ? -3 * PI / 4 : 3 * PI / 4) : PI;
      std::vector<float> bp(fsMul_);
      for (int k = 0; k < fsMul_; k++) bp[k] = (float)(0.15 * std::sin(2 * PI * k / fsMul_ + ph));
      for (int i = 0; i < bl; i++) tx_[bs + i] += bp[i & M];
      const int vf = (s_.vFlip && (l & 1)) ? -1 : 1;
      const uint8_t* row = src + (size_t)(l - s_.vbi) * nact_ * 4;
      for (int i = 0; i < nact_; i++) {
        const uint8_t* p = row + i * 4;
        float r = p[0] / 255.0f, g = p[1] / 255.0f, b2 = p[2] / 255.0f;
        float Y = 0.299f * r + 0.587f * g + 0.114f * b2;
        float U = 0.493f * (b2 - Y), V = 0.877f * (r - Y);
        int n = b + aStart_ + i;
        tx_[n] = 0.3f + 0.7f * Y + 0.45f * (U * SINT_[n & M] + V * vf * COST_[n & M]);
      }
    }
  }

  void channel(float len, float noise, float ghost, float hum, int hbw) {
    const float g = std::pow(10.0f, -0.004f * len);
    // 截止频率随制式带宽缩放（HD 信号的电缆衰减基准更高），上限 0.35×fs
    const float scale = (float)(s_.fsc / 4433618.75);
    const float base = (hbw ? 30e6f : 6.5e6f) * scale;
    const float div = hbw ? 50.0f : 25.0f;
    const float fc = std::min(base / (1 + len / div), (float)(0.35 * fs_));
    const float a = 1.0f - (float)std::exp(-2 * PI * fc / fs_);
    const float gh = ghost * 0.5f, sig = noise * 0.28f, humA = hum * 0.06f;
    const float humInc = 50.0f / (float)fs_;
    const size_t N = (size_t)SPL_ * s_.lines;
    int gi = 0;
    for (size_t n = 0; n < N; n++) {
      float x = tx_[n] * g;
      lpCh_ += (x - lpCh_) * a;
      float y = lpCh_ + gh * gbuf_[gi];                  // 140ns 回波重影
      gbuf_[gi] = x; gi = (gi + 1) % ghostD_;
      humAcc_ += humInc; if (humAcc_ >= 1) humAcc_ -= 1;
      y += humA * std::sin(2 * PI * humAcc_);
      if (sig > 0) y += (frand() + frand() - 1.0f) * sig;
      rx_[n] = y;
    }
  }

  void decode(float phaseDeg, uint8_t* out) {
    const int M = fsMul_ - 1;
    const int longW = (int)(s_.longUs * 1e-6 * fs_);
    int vsFound = -1, vsCnt = 0;
    for (int l = 0; l < s_.lines; l++) {
      const int b = l * SPL_;
      float m = 1e9f;
      int wnd = std::min(aStart_ + 40, SPL_);
      for (int i = 0; i < wnd; i++) if (rx_[b + i] < m) m = rx_[b + i];
      int c = 0;
      for (int i = 0; i < longW; i++) if (rx_[b + i] < m + 0.05f) c++;
      if (c > longW * 0.8f) { vsCnt++; if (vsFound < 0) vsFound = l; }
    }
    if (vsCnt >= 2) vPhase_ = vsFound;
    else vPhase_ = (vPhase_ + 1) % s_.lines;

    std::vector<float> SINe(fsMul_), COSo(fsMul_);
    const double pe = phaseDeg * PI / 180.0;
    for (int k = 0; k < fsMul_; k++) {
      SINe[k] = (float)std::sin(2 * PI * k / fsMul_ + pe);
      COSo[k] = (float)std::cos(2 * PI * k / fsMul_ + pe);
    }
    // 亮度低通截止随制式缩放，上限 0.25×fs
    const double yCut = std::min(0.25 * fs_, 4.5e6 * (s_.fsc / 4433618.75));
    const float aY = 1.0f - (float)std::exp(-2 * PI * yCut / fs_);
    const int J = 16;
    double dS = 0, dQ = 0; int dN = 0;

    for (int r = 0; r < s_.active; r++) {
      const int l = (vPhase_ + s_.vbi + r) % s_.lines, b = l * SPL_;
      float m = 1e9f;
      for (int i = b + spF_ - 20; i < b + spF_ + spS_ + (int)(5e-6 * fs_); i++)
        if (rx_[i] < m) m = rx_[i];
      float bp = 0; int bc = 0;
      for (int i = b + spF_ + spS_ + 8; i < b + aStart_ - 24; i++) { bp += rx_[i]; bc++; }
      bp /= std::max(1, bc);
      const float agc = 0.3f / std::max(0.04f, bp - m);
      int d = dLast_; bool found = false;
      for (int i = b + spF_ + spS_ - J; i <= b + spF_ + spS_ + J; i++) {
        if (rx_[i - 1] - m < 0.14f && rx_[i] - m >= 0.14f) { d = i - (b + spF_ + spS_); found = true; break; }
      }
      if (found) { dLast_ = d; dS += d; dQ += (double)d * d; dN++; }
      const int vf = (s_.vFlip && (l & 1)) ? -1 : 1;
      const int base = b + aStart_ + d;
      std::fill(maU_.begin(), maU_.end(), 0.0f);
      std::fill(maV_.begin(), maV_.end(), 0.0f);
      float sumU = 0, sumV = 0; int mi = 0;
      for (int i = 0; i < nact_; i++) {
        const int n = base + i, k = (n - d) & M;
        const float s = (rx_[n] - m) * agc;
        sLine_[i] = s;
        const float uIn = s * SINe[k] * 2, vIn = s * COSo[k] * 2;
        sumU += uIn - maU_[mi]; maU_[mi] = uIn;
        sumV += vIn - maV_[mi]; maV_[mi] = vIn;
        mi = (mi + 1) & M;
        Uline_[i] = sumU / fsMul_;
        Vline_[i] = sumV / fsMul_;
      }
      uint8_t* row = out + (size_t)r * nact_ * 4;
      for (int i = 0; i < nact_; i++) {
        const int n = base + i, k = (n - d) & M;
        const float cm = 0.45f * (Uline_[i] * SINe[k] + Vline_[i] * COSo[k]);
        lpY_ += ((sLine_[i] - cm) - lpY_) * aY;
        const float U = Uline_[i] * 2.222f, V = Vline_[i] * 2.222f * vf, Y = lpY_ - 0.3f;
        float rr = Y + 1.14f * V, gg = Y - 0.395f * U - 0.581f * V, bb = Y + 2.032f * U;
        uint8_t* o = row + i * 4;
        o[0] = rr <= 0 ? 0 : rr >= 0.7f ? 255 : (uint8_t)(rr * 364.2857f);
        o[1] = gg <= 0 ? 0 : gg >= 0.7f ? 255 : (uint8_t)(gg * 364.2857f);
        o[2] = bb <= 0 ? 0 : bb >= 0.7f ? 255 : (uint8_t)(bb * 364.2857f);
        o[3] = 255;
      }
    }
    jitNs_ = dN > 4 ? (int)(std::sqrt(std::max(0.0, dQ / dN - (dS / dN) * (dS / dN))) * 1e9 / fs_) : 0;
  }
};

} // namespace

extern "C" {
  void* coax_create(int stdIdx, int fsMul) { return new CoaxSim(stdIdx, fsMul); }
  void  coax_destroy(void* s) { delete (CoaxSim*)s; }
  int   coax_width(void* s)   { return ((CoaxSim*)s)->width(); }
  int   coax_height(void* s)  { return ((CoaxSim*)s)->height(); }
  int   coax_jit(void* s)     { return ((CoaxSim*)s)->jitNs(); }
  int   coax_spl(void* s)     { return ((CoaxSim*)s)->SPL(); }
  float* coax_tx(void* s)     { return ((CoaxSim*)s)->tx(); }
  float* coax_rx(void* s)     { return ((CoaxSim*)s)->rx(); }
  void coax_process_preload(void* s, uint8_t* out,
                            double len, double noise, double phase, double ghost, double hum,
                            int hbw, int frame) {
    ((CoaxSim*)s)->processPreload(out, (float)len, (float)noise, (float)phase,
                                  (float)ghost, (float)hum, hbw, (uint32_t)frame);
  }
  void  coax_process(void* s, const uint8_t* in, uint8_t* out,
                     double len, double noise, double phase, double ghost, double hum,
                     int hbw, int frame) {
    ((CoaxSim*)s)->process(in, out, (float)len, (float)noise, (float)phase,
                           (float)ghost, (float)hum, hbw, (uint32_t)frame);
  }
  void coax_encode_channel(void* s, const uint8_t* in, double len, double noise,
                           double ghost, double hum, int hbw, int frame) {
    ((CoaxSim*)s)->encodeChannelOnly(in, (float)len, (float)noise, (float)ghost,
                                     (float)hum, hbw, (uint32_t)frame);
  }
}