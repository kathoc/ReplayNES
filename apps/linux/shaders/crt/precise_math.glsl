// log / cos for the RF noise (M3-NOISE gaussAt). Vulkan only guarantees ~2^-11 absolute error for
// cos() and a few ULP for log(); Metal's precise functions (the macOS port) and WebGL highp are
// ~1 ULP. These float32 implementations (cephes logf / cosf) are ~1 ULP on every driver, so the
// noise matches the reference independently of the GPU's transcendental units.
// SPDX-License-Identifier: GPL-2.0-or-later
float rn_cos(float x) {
  // x >= 0 (here [0, 2*pi)): x = q*pi/2 + r, |r| <= pi/4; pi/2 in three parts (exact q*A, q*B).
  precise float q = floor(x * 0.63661977236758134 + 0.5);
  precise float r = ((x - q * 1.5703125) - q * 4.837512969970703125e-4) - q * 7.54978995489188216e-8;
  int quadrant = int(q) & 3;
  precise float z = r * r;
  precise float s = ((-1.9515295891e-4 * z + 8.3321608736e-3) * z - 1.6666654611e-1) * z * r + r;
  precise float c = ((2.443315711809948e-5 * z - 1.388731625493765e-3) * z + 4.166664568298827e-2) * z * z - 0.5 * z + 1.0;
  if (quadrant == 0) return c;
  if (quadrant == 1) return -s;
  if (quadrant == 2) return -c;
  return s;
}

float rn_log(float xin) {
  // xin in (0, 1]: xin = m * 2^e, m in [0.5, 1).
  int e;
  float m = frexp(xin, e);
  precise float x;
  if (m < 0.70710678118654752) { e -= 1; x = m + m - 1.0; } else { x = m - 1.0; }
  precise float z = x * x;
  precise float p = 7.0376836292e-2;
  p = p * x - 1.1514610310e-1;
  p = p * x + 1.1676998740e-1;
  p = p * x - 1.2420140846e-1;
  p = p * x + 1.4249322787e-1;
  p = p * x - 1.6668057665e-1;
  p = p * x + 2.0000714765e-1;
  p = p * x - 2.4999993993e-1;
  p = p * x + 3.3333331174e-1;
  precise float y = p * x * z;
  precise float fe = float(e);
  y = y + -2.12194440e-4 * fe;
  y = y + -0.5 * z;
  precise float r = x + y;
  r = r + 0.693359375 * fe;
  return r;
}
