// Orb — animated shader orb on a true-black background, with a liquid-glass clock.
// Native Win32 + Direct3D 11 (+ Direct2D/DirectWrite for the settings panel). Single file.
//
// Keys:  H / right-click = settings      F / F11 / Alt+Enter / double-click = fullscreen
//        1-7 = form   M = auto-morph   C = clock style   A = react to sound
//        Space = pause   Esc = close settings / leave fullscreen
// Settings are saved to %APPDATA%\Orb\orb.ini (window position + fullscreen state too).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <winstring.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include "version.h"

#ifdef _MSC_VER
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "winmm.lib")
#endif

template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- shaders (HLSL)
static const char* HLSL = R"HLSL(
cbuffer Orb : register(b0) {
  float2 uRes; float uTime; float uSize;
  float2 uOff; float uDetail; float uWarp;
  float uWobble; float uGrain; float uGlow; float uAccent;
  float uBright; float uContrast; float uStars; float uMorph;
  int uFormA; int uFormB; float uBass; float uLevel;
  float uParticles; float uCharge; float uSS; float _pad0;   // uSS = scene pixels per screen pixel
  float4 uC0, uC1, uC2, uC3;
};
cbuffer Comp : register(b1) {
  float4 gPanel;   // clock panel centre xy, half-size xy (px)
  float4 gGlass;   // corner radius, frost radius (px), refraction, style (0 off, 1 minimal, 2 glass)
  float4 gText;    // text box xy, wh (px)
  float4 gMisc;    // backbuffer w, h, text alpha, px scale (h / 1440)
  float4 gAudio;   // bass, level, bloom strength, settings-panel opacity
  float4 gUi;      // settings panel rect x, y, w, h (px)
  float4 gUi2;     // settings panel corner radius, frost radius
};
cbuffer Post : register(b2) { float4 pTexel; float4 pParam; };
Texture2D sceneTex : register(t0);
Texture2D textTex  : register(t1);
Texture2D bloomTex : register(t2);
Texture2D uiTex    : register(t3);
SamplerState smp   : register(s0);

struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VO vs(uint id : SV_VertexID) {
  float2 c = float2(id & 1, id >> 1);
  VO o;
  o.pos = float4(c.x * 2.0 - 1.0, 1.0 - c.y * 2.0, 0.0, 1.0);
  o.uv = c;
  return o;
}

// ------------------------------------------------ helpers
float hash(float3 p) {
  p = frac(p * float3(.1031, .1030, .0973));
  p += dot(p, p.yxz + 33.33);
  return frac((p.x + p.y) * p.z);
}
float noise(float3 x) {
  float3 i = floor(x), f = frac(x);
  f = f * f * (3.0 - 2.0 * f);
  return lerp(lerp(lerp(hash(i), hash(i + float3(1,0,0)), f.x),
                   lerp(hash(i + float3(0,1,0)), hash(i + float3(1,1,0)), f.x), f.y),
              lerp(lerp(hash(i + float3(0,0,1)), hash(i + float3(1,0,1)), f.x),
                   lerp(hash(i + float3(0,1,1)), hash(i + float3(1,1,1)), f.x), f.y), f.z);
}
float fbm(float3 p) {
  float s = 0.0, a = 0.5;
  [unroll] for (int i = 0; i < 5; i++) { s += a * noise(p); p = p * 2.03 + 17.1; a *= 0.5; }
  return s;
}
float2 rot(float2 v, float a) { float c = cos(a), s = sin(a); return float2(c * v.x - s * v.y, s * v.x + c * v.y); }
float smin(float a, float b, float k) { float h = saturate(0.5 + 0.5 * (b - a) / k); return lerp(b, a, h) - k * h * (1.0 - h); }

float3 pal(float x) {
  x = saturate(x);
  float3 c = lerp(uC0.rgb, uC1.rgb, smoothstep(0.0, 0.55, x));
  return lerp(c, uC2.rgb, smoothstep(0.5, 1.0, x));
}
float3 shade(float I) { return pal(I) * (0.15 + 0.95 * I); }

// ------------------------------------------------ 3D forms (signed distance fields)
float3 objSpace(float3 P) {
  P.xz = rot(P.xz, uTime * 0.11);
  P.yz = rot(P.yz, uTime * 0.07);
  return P;
}

float sdShape(int f, float3 P) {
  float t = uTime;
  if (f == 0) return length(P) - 1.0;                                   // orb
  if (f == 1) {                                                         // blob: 4 melting spheres
    float d = 1e5;
    [loop] for (int i = 0; i < 4; i++) {
      float fi = (float)i;
      float3 c = 0.42 * float3(sin(t * 0.37 + fi * 1.9), sin(t * 0.29 + fi * 2.7), sin(t * 0.33 + fi * 1.3));
      d = smin(d, length(P - c) - (0.62 - 0.04 * fi), 0.42);
    }
    return d;
  }
  if (f == 2) {                                                         // ring
    P.yz = rot(P.yz, 1.1);
    float2 q = float2(length(P.xz) - 0.70, P.y);
    return length(q) - 0.30;
  }
  if (f == 3) {                                                         // softly twisting cube
    P.xz = rot(P.xz, P.y * 0.45 * sin(t * 0.3));
    float3 q = abs(P) - 0.6;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0) - 0.16;
  }
  float3 q = abs(P);                                                    // crystal (octahedron)
  return (q.x + q.y + q.z - 0.98) * 0.57735 - 0.06;
}

float map(float3 Pw, int fa, int fb, float m) {
  float3 P = objSpace(Pw);
  float d = sdShape(fa, P);
  if (fb != fa) d = lerp(d, sdShape(fb, P), m);
  float wob = sin(P.x * 3.1 + uTime * 0.9) * sin(P.y * 2.7 - uTime * 0.7) * sin(P.z * 2.9 + uTime * 0.6);
  return d + (uWobble + uBass * 1.2) * 0.035 * wob;
}

float3 calcNormal(float3 P, int fa, int fb, float m) {
  // tetrahedron taps in a real loop so the compiler doesn't inline map() four times
  float3 n = 0;
  [loop] for (int i = 0; i < 4; i++) {
    float3 e = 0.5773 * (2.0 * float3(((i + 3) >> 1) & 1, (i >> 1) & 1, i & 1) - 1.0);
    n += e * map(P + e * 0.0015, fa, fb, m);
  }
  return normalize(n);
}

float4 orb3D(float2 p, int fa, int fb, float m) {
  float t = uTime;
  float R = 0.5 * uSize;
  float2 uv = p / R;
  float3 ro = float3(0.0, 0.0, 4.0);
  float3 rd = normalize(float3(uv * 0.258, -1.0));

  float b = dot(ro, rd), h = b * b - (dot(ro, ro) - 4.84);   // bounding radius 2.2
  float dmin = length(cross(ro, rd)) - 1.0;
  bool hit = false;
  float3 P = 0;
  if (h > 0.0) {
    float tt = -b - sqrt(h), tEnd = -b + sqrt(h);
    dmin = 1e5;
    [loop] for (int i = 0; i < 110; i++) {
      P = ro + rd * tt;
      float d = map(P, fa, fb, m);
      dmin = min(dmin, d);
      if (d < 0.0008 * tt) { hit = true; break; }
      tt += d * 0.85;
      if (tt > tEnd) break;
    }
  }

  float3 col = 0;
  float cov = 0;
  if (hit) {
    float3 n = calcNormal(P, fa, fb, m);
    float ndv = saturate(dot(n, -rd));
    float3 q = objSpace(P);
    float3 dir = normalize(q);

    float3 wv = float3(noise(dir * 1.8 + float3(0.0, 0.0,  t * 0.25)),
                       noise(dir * 1.8 + float3(17.0, 3.0, -t * 0.21)),
                       noise(dir * 1.8 + float3(5.0, 31.0,  t * 0.18))) - 0.5;
    float3 w = normalize(dir + wv * uWarp);

    float f = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
      float fi = (float)k;
      float3 s = normalize(float3(sin(t * (0.13 + 0.05 * fi) + fi * 1.7),
                                  cos(t * (0.11 + 0.04 * fi) + fi * 2.3),
                                  sin(t * (0.09 + 0.03 * fi) + fi * 0.9)));
      float g = acos(clamp(dot(w, s), -1.0, 1.0));
      f += cos(g * uDetail - t * (1.1 + 0.2 * fi) + fi);
    }
    f *= 0.25;
    float band  = 0.5 + 0.5 * f;
    float ridge = pow(max(1.0 - abs(f), 0.0), 3.0);
    float I = 0.45 * band + 0.6 * ridge;
    I = lerp(0.6, I, uContrast);

    float3 L = normalize(float3(-0.25, 0.35, 0.9));
    float lit = 0.55 + 0.5 * max(dot(n, L), 0.0);
    float rim = pow(max(1.0 - ndv, 0.0), 2.5);
    I = I * lit + rim * (0.2 + 0.55 * ridge);

    // particle stipple: soft round grains (~3 screen px) scattered in 3D cells and anchored to the surface,
    // instead of hard square pixels
    // two layers on different, rotated lattices with random gaps, so no grid pattern shows
    float cells = uRes.y / uSS * R * 0.38;
    float dotv = 0.0;
    [unroll] for (int l = 0; l < 2; l++) {
      float3 cq = (l == 0 ? q : float3(0.8 * q.x - 0.6 * q.z, 0.6 * q.x * 0.48 + 0.8 * q.y + 0.36 * q.z, 0.36 * q.x - 0.6 * q.y + 0.71 * q.z))
                  * cells * (l == 0 ? 1.0 : 1.37);
      float3 ci = floor(cq) + l * 31.7;
      float3 jit = float3(hash(ci), hash(ci + 11.1), hash(ci + 27.3)) - 0.5;
      float hr = hash(ci + 5.7);
      float rad = 0.22 + 0.16 * hash(ci + 19.3);
      float dd = length(frac(cq) - 0.5 - jit * 0.5);
      dotv = max(dotv, smoothstep(rad, rad * 0.25, dd) * step(0.35, hr) * (0.4 + 0.6 * hr) * (l == 0 ? 1.0 : 0.8));
    }
    float st = lerp(dotv, noise(q * cells * 0.45), 0.35);
    I *= lerp(1.0, 0.25 + 1.45 * st, uGrain);
    I *= uBright * (1.0 + 0.35 * uLevel);

    float en = noise(dir * 2.0 + float3(0.0, 0.0, t * 0.4 + 9.0));
    float edge = smoothstep(0.0, 0.62 + 0.16 * en, ndv);

    float3 c = shade(I);
    float aw = smoothstep(0.1, 1.0, dot(normalize(p + 1e-5), float2(0.8, -0.6))) * (0.25 + rim);
    c = lerp(c, uC3.rgb * (0.25 + 0.9 * I), saturate(aw * uAccent * 1.3));
    col = c * edge;
    cov = edge;
  }

  float hd = max(dmin, 0.0);
  float halo = uGlow * (1.0 + uBass * 1.5 + 0.6 * uCharge) * (0.22 * exp(-hd * 9.0) + 0.07 * exp(-hd * 2.5)) * (1.0 - cov);
  halo = max(halo - 0.003, 0.0);
  col += lerp(uC1.rgb, uC3.rgb, 0.35 * uAccent) * halo;
  return float4(col, max(cov, saturate(halo * 4.0)));
}

// sparks that peel off the surface and drift away
float3 embers(float2 p) {
  float R = 0.5 * uSize;
  float3 c = 0;
  [loop] for (int i = 0; i < 48; i++) {
    float fi = (float)i;
    float h1 = hash(float3(fi, 1.3, 7.1)), h2 = hash(float3(fi, 4.7, 2.2)), h3 = hash(float3(fi, 9.1, 5.5));
    float life = 5.0 + 7.0 * h3;
    float tt = frac(uTime / life + h1);
    float ang = h2 * 6.2831853 + tt * (h1 - 0.5) * 1.2;
    float r = R * (0.92 + tt * (0.5 + 0.9 * h1));
    float2 pos = float2(cos(ang), sin(ang)) * r + float2(0.0, tt * 0.04);
    float sz = R * (0.006 + 0.010 * h3);
    float2 d = p - pos;
    float fade = sin(tt * 3.14159);
    fade *= fade;
    c += exp(-dot(d, d) / (sz * sz)) * fade * (0.5 + 0.5 * h2);
  }
  return c * lerp(uC2.rgb, uC3.rgb, 0.35) * uParticles * (1.0 + uLevel);
}

// ------------------------------------------------ 2D forms
float grain2D(float2 fc, float speed) {
  float2 g = floor((fc + float2(uTime * speed, 0.0)) / (max(1.0, uRes.y / uSS / 700.0) * uSS));
  return lerp(1.0, 0.35 + 1.3 * hash(float3(g, 3.7)), uGrain * 0.7);
}

// mountain ridge height (screen units) at x
float ridge(float x) {
  // ridged noise: sharp peaks and valleys instead of soft hills
  float h = 0.0, a = 0.55, f = 1.1;
  [unroll] for (int i = 0; i < 6; i++) {
    float n = 1.0 - abs(2.0 * noise(float3(x * f, 3.7, 1.3 * i)) - 1.0);
    h += a * n * n;
    f *= 2.15; a *= 0.45;
  }
  return -0.37 + 0.22 * h;
}

// near shore: low rolling hills with a pine tree line on top
float shore(float x) {
  float h = -0.392 + 0.022 * noise(float3(x * 2.2, 8.1, 0.0)) + 0.012 * noise(float3(x * 6.0, 2.2, 0.0));
  float cw = 0.0055, cx = x / cw, id = floor(cx), fx = frac(cx) - 0.5;
  float th = hash(float3(id, 4.4, 1.0));
  float forest = smoothstep(0.35, 0.6, noise(float3(x * 1.7, 5.5, 0.0)));   // forest patches and clearings
  float tall = (0.010 + 0.026 * th) * forest * step(0.2, th);
  float sway = fx - 0.12 * (hash(float3(id, 2.0, 9.0)) - 0.5);
  return h + tall * max(1.0 - abs(sway) * 2.4, 0.0);
}

// night landscape: aurora curtains, a far range catching their light, a dark tree line in front
float3 auroraSky(float2 p, out float a) {
  a = 0.0;
  float e = 1.2 / uRes.y;                                // one pixel: analytic anti-aliasing of silhouettes
  float cS = smoothstep(-e, e, p.y - shore(p.x));
  if (cS <= 0.0) { a = 1.0; return 0; }                 // tree line: pure black
  float rg = ridge(p.x);
  float cR = smoothstep(-e, e, p.y - rg);
  float t = uTime;
  float3 tint = lerp(uC1.rgb, uC2.rgb, 0.25);
  // far range: almost black, the upper slopes (snow) catch a little aurora light
  float3 mtn = tint * (0.010 + 0.07 * exp(-max(rg - p.y, 0.0) / 0.022))
             * (0.55 + 0.45 * noise(float3(p.x * 45.0, p.y * 70.0, 1.0))) * uBright * (1.0 - cR);
  // faint airglow low on the horizon
  float3 col = lerp(uC1.rgb, uC3.rgb, 0.3) * 0.04 * exp(-(p.y + 0.30) / 0.10) * uBright;
  [loop] for (int k = 0; k < 3; k++) {
    float fk = (float)k;
    float x = p.x * (0.85 + 0.25 * fk) + fk * 7.3;
    // big slow folds + small ripples that run along the curtain
    float fold = 0.085 * sin(x * 1.6 + t * 0.06 + fk) + 0.045 * sin(x * 3.7 - t * 0.10 + fk * 2.0)
               + 0.11 * uWarp * (noise(float3(x * 1.1, fk * 4.0, t * 0.05)) - 0.5)
               + 0.010 * sin(x * 13.0 - t * 1.1 + fk * 3.0) * (0.5 + 0.5 * sin(x * 1.3 + t * 0.2));
    // where the curtain folds towards us we look along it: brighter and denser
    float slope = 0.136 * cos(x * 1.6 + t * 0.06 + fk) + 0.166 * cos(x * 3.7 - t * 0.10 + fk * 2.0);
    float edgeOn = 0.55 + 0.9 / (1.0 + 30.0 * slope * slope);
    // the curtain is patchy: some stretches glow, others almost vanish
    float patch = smoothstep(0.2, 0.8, noise(float3(x * 0.55 + 3.0, fk * 5.0, t * 0.025)));
    float base = (-0.21 + 0.11 * fk) * (0.6 + 0.5 * uSize) + fold;
    float dy = p.y - base;
    if (dy < -0.03) continue;
    float hy = max(dy, 0.0);
    // soft rays that follow the folds and slowly drift; some reach much higher than others
    float xr = (x + fold * 2.0) * uDetail * 4.0 + t * 0.05;
    float r1 = noise(float3(xr, hy * 0.8 - t * 0.15, fk * 9.0));
    float r2 = noise(float3(xr * 2.7 + 5.0, hy * 0.5, t * 0.25 + fk));
    float rays = 0.32 + 0.68 * smoothstep(0.2, 0.9, r1) * (0.6 + 0.4 * r2);
    float len = 0.14 + 0.26 * noise(float3(xr * 0.3, fk * 2.0, t * 0.04));
    float edge = smoothstep(-0.016, 0.002, dy);                         // sharp lower edge
    float prof = edge * (0.75 * exp(-hy / 0.05) + 0.45 * exp(-hy / (len * (1.0 - 0.15 * fk))));
    float flick = 0.75 + 0.25 * noise(float3(x * 2.4, t * 0.55, fk * 3.0));
    float I = prof * rays * flick * edgeOn * (0.15 + 0.85 * patch) * (1.0 - 0.22 * fk) * (1.0 + uBass * 0.8);
    I = pow(max(I, 0.0), 0.6 + 0.6 * uContrast) * uBright * (1.0 + 0.35 * uLevel);
    // colour by altitude: Mid low down, Accent high up, Light in the hot core, a thin Accent fringe at the bottom
    float hgt = saturate(hy / 0.42);
    float3 c = lerp(uC1.rgb, uC3.rgb, smoothstep(0.10, 0.80, hgt) * (0.35 + 0.65 * uAccent));
    c = lerp(c, uC2.rgb, saturate(I * 1.3 - 0.45) * (1.0 - hgt));
    float fringe = smoothstep(-0.02, -0.005, dy) * (1.0 - smoothstep(-0.004, 0.008, dy));
    c = lerp(c, uC3.rgb, fringe * 0.55 * uAccent);
    col += c * I * 1.25;
  }
  col *= cR;
  a = max(1.0 - cR, saturate(max(col.r, max(col.g, col.b)) * 3.0));
  return (col + mtn) * cS;
}

float4 aurora(float2 p, float2 fc) {
  const float water = -0.40;                         // still lake in front of the mountains
  float2 q = p;
  float k = 1.0;
  if (p.y < water) {
    // mirrored sky, stretched (the lake is seen at a grazing angle) and broken up by slow ripples
    float d = water - p.y;
    float rip = sin(d * 260.0 - uTime * 1.1 + 2.0 * noise(float3(p.x * 9.0, d * 30.0, uTime * 0.2)));
    q = float2(p.x + 0.004 * rip * (0.3 + d * 6.0), water + 0.03 + d * 2.6);
    k = 0.42 * exp(-d * 3.0) * (0.85 + 0.15 * rip);
  }
  float a;
  float3 col = auroraSky(q, a) * k;
  col *= lerp(1.0, grain2D(fc, 10.0), 0.25);
  float wl = smoothstep(-1.2 / uRes.y, 1.2 / uRes.y, water - p.y);    // anti-aliased waterline
  return float4(col, lerp(a, 1.0, wl));
}

float4 nebula(float2 p, float2 fc) {
  float t = uTime, R = 0.5 * uSize;
  float2 u = p / R;
  float3 q = float3(u * 0.9, t * 0.05);
  float2 w = float2(fbm(q + float3(0.0, 0.0, t * 0.03)), fbm(q + float3(5.2, 1.3, -t * 0.02)));
  float f = fbm(q + uWarp * 2.2 * float3(w, 0.0) + float3(0.0, 0.0, t * 0.04));
  float rdg = pow(max(1.0 - abs(sin(f * uDetail * 1.4 - t * 0.6)), 0.0), 3.0);
  float I = f * (0.7 + 0.8 * rdg);
  I = lerp(0.55 * f + 0.2, I, uContrast);
  // dark dust lanes winding through the glowing gas
  float dust = smoothstep(0.42, 0.72, fbm(q * 2.4 + float3(w * 1.6, 7.0)));
  I *= 1.0 - 0.7 * dust;
  float mask = exp(-dot(u, u) * 0.85) * (1.0 + uBass * 0.6);
  I = max(I * mask * 1.7 - 0.06, 0.0);
  I *= grain2D(fc, 6.0) * uBright * (1.0 + 0.35 * uLevel);
  float3 c = shade(I);
  float aw = smoothstep(0.0, 1.0, dot(normalize(u + 1e-5), float2(0.8, -0.6))) * smoothstep(0.3, 1.4, length(u));
  c = lerp(c, uC3.rgb * (0.2 + I), saturate(aw * uAccent));
  c += lerp(uC1.rgb, uC3.rgb, 0.4) * uGlow * 0.10 * mask * (1.0 - saturate(I));
  return float4(c, saturate(max(c.r, max(c.g, c.b)) * 3.0));
}

float3 stars(float2 fc) {
  float s = 0.0;
  [unroll] for (int l = 0; l < 2; l++) {
    float fl = (float)l;
    float sc = (l == 0 ? 38.0 : 71.0) * uRes.y / 1440.0;
    float2 g = (fc + uOff * uRes.y * (0.3 + 0.4 * fl)) / sc;
    float2 id = floor(g), fr = frac(g);
    float h = hash(float3(id, fl * 13.0));
    if (h > 0.86) {
      float2 pos = float2(hash(float3(id, 1.7 + fl)), hash(float3(id, 5.3 + fl))) * 0.8 + 0.1;
      float dd = length(fr - pos) * sc * 1440.0 / uRes.y;
      float tw = 0.55 + 0.45 * sin(uTime * (0.6 + 2.0 * h) + h * 60.0);
      s += exp(-dd * dd * (l == 0 ? 0.9 : 0.45)) * tw * (h - 0.86) / 0.14;
    }
  }
  return s * uStars * lerp(float3(1.0, 1.0, 1.0), uC2.rgb, 0.4);
}

float4 psScene(VO i) : SV_Target {
  float2 fc = float2(i.pos.x, uRes.y - i.pos.y);   // bottom-left origin
  float2 p = (fc - 0.5 * uRes) / uRes.y - uOff;

  // 3D -> 3D is a true shape morph; anything involving a 2D form cross-fades.
  // Each form function has exactly one call site to keep the compiled shader small.
  int fa = uFormA, fb = uFormB;
  float m = uMorph;
  float w3 = 0.0, m3 = 0.0;
  int s3a = 0, s3b = 0;
  if (fa < 5 && fb < 5) { w3 = 1.0; s3a = fa; s3b = fb; m3 = m; }
  else if (fa < 5)      { w3 = 1.0 - m; s3a = s3b = fa; }
  else if (fb < 5)      { w3 = m; s3a = s3b = fb; }
  float wA = (fa == 5 ? 1.0 - m : 0.0) + (fb == 5 ? m : 0.0);
  float wN = (fa == 6 ? 1.0 - m : 0.0) + (fb == 6 ? m : 0.0);

  float4 r = 0;
  if (w3 > 0.001) r += w3 * orb3D(p, s3a, s3b, m3);
  if (wA > 0.001) r += wA * aurora(p, fc);
  if (wN > 0.001) r += wN * nebula(p, fc);
  float3 col = r.rgb + stars(fc) * (1.0 - saturate(r.a));
  if (w3 > 0.001 && uParticles > 0.001) col += w3 * embers(p);

  col = 1.0 - exp(-col * 1.6);
  float lum = max(col.r, max(col.g, col.b));
  col += (hash(float3(fc, frac(uTime) * 97.0)) - 0.5) / 255.0 * step(1.5 / 255.0, lum);
  return float4(col, 1.0);
}

// ------------------------------------------------ bloom
float4 psBright(VO i) : SV_Target {
  float2 t = pTexel.xy;
  float3 c = (sceneTex.SampleLevel(smp, i.uv + t * float2(-1, -1), 0).rgb + sceneTex.SampleLevel(smp, i.uv + t * float2(1, -1), 0).rgb +
              sceneTex.SampleLevel(smp, i.uv + t * float2(-1, 1), 0).rgb + sceneTex.SampleLevel(smp, i.uv + t * float2(1, 1), 0).rgb) * 0.25;
  float l = max(c.r, max(c.g, c.b));
  float k = saturate((l - pParam.x) / pParam.y);
  return float4(c * k * k, 1.0);
}
float4 psBlur(VO i) : SV_Target {
  float2 d = pTexel.xy * pTexel.zw;
  float3 c = bloomTex.SampleLevel(smp, i.uv, 0).rgb * 0.2270270270;
  c += (bloomTex.SampleLevel(smp, i.uv + d * 1.3846153846, 0).rgb + bloomTex.SampleLevel(smp, i.uv - d * 1.3846153846, 0).rgb) * 0.3162162162;
  c += (bloomTex.SampleLevel(smp, i.uv + d * 3.2307692308, 0).rgb + bloomTex.SampleLevel(smp, i.uv - d * 3.2307692308, 0).rgb) * 0.0702702703;
  return float4(c, 1.0);
}

// ------------------------------------------------ composite: upscale + bloom + glass clock + settings panel
float sdRBox(float2 p, float2 b, float r) {
  float2 q = abs(p) - b + r;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float3 scene(float2 px) {
  float2 uv = px / gMisc.xy;
  return sceneTex.SampleLevel(smp, uv, 0).rgb + bloomTex.SampleLevel(smp, uv, 0).rgb * gAudio.z;
}
float textA(float2 px) {
  float2 uv = (px - gText.xy) / gText.zw;
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  return textTex.SampleLevel(smp, uv, 0).a;
}

// liquid glass sheet over 'col': refraction at the rim, frosted blur, chromatic edge, sheen, rim light, shadow
float3 glassOver(float3 col, float2 px, float2 c, float2 hs, float rad, float br, float refrK, float milk, float dark, float pulse) {
  float k1 = gMisc.w;
  float d = sdRBox(px - c, hs, rad);
  float dS = sdRBox(px - c - float2(0.0, min(0.10 * hs.y, 18.0 * k1)), hs, rad);
  col *= 1.0 - 0.5 * exp(-max(dS, 0.0) / min(0.22 * hs.y, 40.0 * k1)) * smoothstep(-1.0, 1.0, d);
  if (d >= 1.0) return col;

  float2 g = float2(sdRBox(px + float2(1, 0) - c, hs, rad) - sdRBox(px - float2(1, 0) - c, hs, rad),
                    sdRBox(px + float2(0, 1) - c, hs, rad) - sdRBox(px - float2(0, 1) - c, hs, rad));
  g = dot(g, g) > 1e-8 ? normalize(g) : float2(0.0, 0.0);
  float bw = min(min(hs.x, hs.y) * 0.6, 70.0 * k1);
  float bev = pow(1.0 - saturate(-d / bw), 2.5);
  float2 refr = -g * bev * bw * refrK * (1.0 + 0.6 * pulse);
  float2 ca = g * bev * bw * 0.07 * refrK;

  float3 acc = 0;
  [loop] for (int k = 0; k < 32; k++) {
    float fk = (float)k + 0.5;
    float a = fk * 2.39996;
    float2 o = px + refr + float2(cos(a), sin(a)) * sqrt(fk / 32.0) * br;
    acc += float3(scene(o + ca).r, scene(o).g, scene(o - ca).b);
  }
  float3 glass = acc / 32.0;
  float lum = dot(glass, float3(0.299, 0.587, 0.114));
  glass = max(lerp(lum.xxx, glass, 1.3), 0.0);
  glass = lerp(glass * 1.1 + milk, glass * 0.40 + 0.012, dark);

  float vy = (px.y - (c.y - hs.y)) / (2.0 * hs.y);
  glass += 0.07 * (1.0 - 0.5 * dark) * pow(saturate(1.0 - vy), 3.0);
  float rimLine = exp(-abs(d + 1.3 * k1) / (1.1 * k1));
  float keyL = saturate(dot(g, normalize(float2(-0.55, -0.85))));
  float backL = saturate(dot(g, normalize(float2(0.55, 0.85))));
  glass += rimLine * (0.16 + 0.6 * keyL * keyL + 0.28 * backL * backL) * (1.0 + 1.5 * pulse);
  glass += (0.05 + 0.12 * pulse) * bev * lerp(float3(1, 1, 1), uC2.rgb, 0.5);
  return lerp(col, glass, saturate(0.5 - d));
}

float4 psComp(VO i) : SV_Target {
  float2 px = i.pos.xy;
  float3 col = scene(px);
  int style = (int)gGlass.w;
  float k1 = gMisc.w;

  if (style == 2) col = glassOver(col, px, gPanel.xy, gPanel.zw, gGlass.x, gGlass.y, gGlass.z, 0.03, 0.0, gAudio.x);
  if (style >= 1) {
    float sa = 0.0;
    float2 so = float2(0.0, 2.0 * k1);
    [unroll] for (int k = 0; k < 9; k++) {
      float2 oo = float2(k % 3 - 1, k / 3 - 1) * 2.5 * k1;
      sa += textA(px - so - oo);
    }
    sa /= 9.0;
    col *= 1.0 - sa * (style == 2 ? 0.35 : 0.65);
    col = lerp(col, float3(0.97, 0.98, 1.0), textA(px) * gMisc.z);
  }

  if (gAudio.w > 0.001) {                     // settings panel: dark liquid glass + Direct2D content
    float2 hs = gUi.zw * 0.5, c = gUi.xy + hs;
    float3 g = glassOver(col, px, c, hs, gUi2.x, gUi2.y, 0.35, 0.0, 0.62, 0.0);
    col = lerp(col, g, gAudio.w);
    float2 uv = (px - gUi.xy) / gUi.zw;
    if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
      float4 u = uiTex.SampleLevel(smp, uv, 0);
      col = col * (1.0 - u.a * gAudio.w) + u.rgb * gAudio.w;
    }
  }
  return float4(col, 1.0);
}
)HLSL";

struct OrbCB {
    float res[2], time, size;
    float off[2], detail, warp;
    float wobble, grain, glow, accent;
    float bright, contrast, stars, morph;
    int formA, formB;
    float bass, level;
    float particles, charge, ss, pad0;
    float c[4][4];
};
struct CompCB { float panel[4], glass[4], text[4], misc[4], audio[4], ui[4], ui2[4]; };
struct PostCB { float texel[4], param[4]; };

// ---------------------------------------------------------------- settings
enum {
    P_SIZE, P_SPEED, P_DETAIL, P_WARP, P_WOBBLE, P_BREATHE,
    P_GRAIN, P_GLOW, P_ACCENT, P_BRIGHT, P_CONTRAST, P_STARS,
    P_HUE, P_MORPHSEC, P_SCALE,
    P_CLOCKSIZE, P_FROST, P_REFR, P_SENS,
    P_BLOOM, P_PARTICLES,
    NP
};
enum { FMT_F, FMT_PCT, FMT_SEC };
struct ParamDef { const wchar_t* key; const wchar_t* label; float mn, mx, def; int fmt; };
static const ParamDef PD[NP] = {
    {L"size",       L"Size",              0.2f, 1.4f, 0.82f, FMT_F},
    {L"speed",      L"Speed",             0.0f, 3.0f, 1.00f, FMT_F},
    {L"detail",     L"Pattern detail",    2.0f, 20.f, 7.00f, FMT_F},
    {L"warp",       L"Turbulence",        0.0f, 1.2f, 0.40f, FMT_F},
    {L"wobble",     L"Edge wobble",       0.0f, 2.0f, 0.50f, FMT_F},
    {L"breathe",    L"Breathing",         0.0f, 1.5f, 0.30f, FMT_F},
    {L"grain",      L"Particle grain",    0.0f, 1.0f, 0.45f, FMT_F},
    {L"glow",       L"Outer glow",        0.0f, 2.0f, 0.60f, FMT_F},
    {L"accent",     L"Accent tint",       0.0f, 1.0f, 0.55f, FMT_F},
    {L"brightness", L"Brightness",        0.3f, 2.0f, 1.00f, FMT_F},
    {L"contrast",   L"Contrast",          0.0f, 1.5f, 0.70f, FMT_F},
    {L"stars",      L"Stars",             0.0f, 1.0f, 0.35f, FMT_F},
    {L"hue",        L"Colour cycle",      0.0f, 1.0f, 0.00f, FMT_F},
    {L"morphsec",   L"Morph every",       10.f, 180.f, 45.f, FMT_SEC},
    {L"scale",      L"Render resolution", 0.35f, 1.0f, 1.00f, FMT_PCT},
    {L"clocksize",  L"Clock size",        0.5f, 2.5f, 1.35f, FMT_F},
    {L"frost",      L"Frost",             0.0f, 1.0f, 0.55f, FMT_F},
    {L"refraction", L"Refraction",        0.0f, 1.5f, 0.80f, FMT_F},
    {L"sensitivity",L"Sensitivity",       0.0f, 2.0f, 1.00f, FMT_F},
    {L"bloom",      L"Bloom",             0.0f, 1.5f, 0.60f, FMT_F},
    {L"particles",  L"Drifting sparks",   0.0f, 1.0f, 0.50f, FMT_F},
};

struct Preset { const wchar_t* name; COLORREF c[4]; };
// Palette presets: name + 4 colours {Deep, Mid, Light, Accent}. Add a line to add a preset (max 10).
static const Preset PRESETS[] = {
    {L"Hub Blue", {RGB(0x01,0x08,0x1a), RGB(0x1b,0x7b,0xd4), RGB(0xae,0xe8,0xff), RGB(0x4b,0x3c,0xff)}},
    {L"Aurora",   {RGB(0x00,0x15,0x0f), RGB(0x13,0xb9,0x8b), RGB(0xca,0xff,0xea), RGB(0x7a,0x3c,0xff)}},
    {L"Ember",    {RGB(0x16,0x04,0x00), RGB(0xe0,0x53,0x1c), RGB(0xff,0xd5,0x9e), RGB(0xff,0x2a,0x6a)}},
    {L"Violet",   {RGB(0x0b,0x02,0x20), RGB(0x7b,0x3c,0xff), RGB(0xe2,0xcc,0xff), RGB(0xff,0x3c,0xa8)}},
    {L"Toxic",    {RGB(0x03,0x12,0x00), RGB(0x74,0xd1,0x1a), RGB(0xf1,0xff,0xb5), RGB(0x00,0xc8,0xff)}},
    {L"Mono",     {RGB(0x04,0x04,0x04), RGB(0x7d,0x85,0x90), RGB(0xff,0xff,0xff), RGB(0x5a,0x78,0xff)}},
};
static const int NUM_PRESETS = (int)(sizeof(PRESETS) / sizeof(PRESETS[0]));
static_assert(sizeof(PRESETS) / sizeof(PRESETS[0]) <= 10, "at most 10 palette presets");
static const wchar_t* COLOR_NAMES[4] = {L"Deep", L"Mid", L"Light", L"Accent"};
static const wchar_t* FORM_NAMES[7] = {L"Orb", L"Blob", L"Ring", L"Cube", L"Crystal", L"Aurora", L"Nebula"};
static const wchar_t* CLOCK_STYLES[3] = {L"Off", L"Minimal", L"Glass"};
static const wchar_t* CLOCK_POS[7] = {L"Center", L"Top", L"Bottom", L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
static const int FPS_OPTS[4] = {24, 30, 60, 0};
enum { NUM_FORMS = 7, NUM_3D = 5 };

struct Settings {
    float p[NP];
    COLORREF c[4];
    bool drift, autoMorph, h24, seconds, showDate, audio, showBattery, showMedia;
    int fps, form, clockStyle, clockPos, aa;   // aa: 0 off, 1 = 1.5x, 2 = 2x supersampling
};
static const float AA_FACTOR[3] = {1.f, 1.5f, 2.f};
static const wchar_t* AA_NAMES[3] = {L"Off", L"1.5× SSAA", L"2× SSAA"};
static Settings S;

static void defaults() {
    for (int i = 0; i < NP; i++) S.p[i] = PD[i].def;
    for (int i = 0; i < 4; i++) S.c[i] = PRESETS[0].c[i];
    S.drift = true; S.autoMorph = false; S.h24 = true; S.seconds = false; S.showDate = true; S.audio = false;
    S.showBattery = true; S.showMedia = true; S.aa = 1;
    S.fps = 60; S.form = 0; S.clockStyle = 2; S.clockPos = 0;
}

static std::wstring g_ini, g_dataDir;
static bool g_dirty = false;

static float iniF(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t b[64];
    GetPrivateProfileStringW(sec, key, L"", b, 64, g_ini.c_str());
    return *b ? (float)_wtof(b) : def;
}
static void iniWF(const wchar_t* sec, const wchar_t* key, double v) {
    wchar_t b[64];
    swprintf(b, 64, L"%.4f", v);
    WritePrivateProfileStringW(sec, key, b, g_ini.c_str());
}
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static int clampi(int v, int a, int b) { return v < a ? a : (v > b ? b : v); }

// returns false on first run (no ini yet)
static bool loadSettings() {
    wchar_t app[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", app, MAX_PATH);
    std::wstring dir = (n && n < MAX_PATH) ? std::wstring(app) + L"\\Orb" : L".";
    CreateDirectoryW(dir.c_str(), NULL);
    g_dataDir = dir;
    g_ini = dir + L"\\orb.ini";
    wchar_t custom[MAX_PATH];  // optional override, e.g. to keep several profiles
    n = GetEnvironmentVariableW(L"ORB_INI", custom, MAX_PATH);
    if (n && n < MAX_PATH) g_ini = custom;
    defaults();
    if (GetFileAttributesW(g_ini.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    for (int i = 0; i < NP; i++) S.p[i] = clampf(iniF(L"orb", PD[i].key, PD[i].def), PD[i].mn, PD[i].mx);
    for (int i = 0; i < 4; i++) {
        wchar_t key[8], b[32];
        swprintf(key, 8, L"c%d", i);
        GetPrivateProfileStringW(L"orb", key, L"", b, 32, g_ini.c_str());
        if (b[0] == L'#') {
            unsigned long v = wcstoul(b + 1, NULL, 16);
            S.c[i] = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
        }
    }
    S.drift = iniF(L"orb", L"drift", 1) != 0;
    S.autoMorph = iniF(L"orb", L"automorph", 0) != 0;
    S.h24 = iniF(L"orb", L"h24", 1) != 0;
    S.seconds = iniF(L"orb", L"seconds", 0) != 0;
    S.showDate = iniF(L"orb", L"date", 1) != 0;
    S.audio = iniF(L"orb", L"audio", 0) != 0;
    S.showBattery = iniF(L"orb", L"battery", 1) != 0;
    S.showMedia = iniF(L"orb", L"media", 1) != 0;
    S.aa = clampi((int)iniF(L"orb", L"aa", 1), 0, 2);
    S.fps = (int)iniF(L"orb", L"fps", 60);
    S.form = clampi((int)iniF(L"orb", L"form", 0), 0, NUM_FORMS - 1);
    S.clockStyle = clampi((int)iniF(L"orb", L"clockstyle", 2), 0, 2);
    S.clockPos = clampi((int)iniF(L"orb", L"clockpos", 0), 0, 6);
    return true;
}

// ---------------------------------------------------------------- globals
static HINSTANCE g_inst;
static HWND g_main;
static int g_w = 1, g_h = 1;
static bool g_fs = false, g_paused = false;
static WINDOWPLACEMENT g_wpPrev = {sizeof(WINDOWPLACEMENT)};
static double g_simT = 0, g_lastT = 0, g_t0 = 0;
static LARGE_INTEGER g_freq;
static int g_frames = 0;
static double g_fpsT = 0;
static double g_lastMove = 0;
static bool g_cursorHidden = false;
static POINT g_lastMouse = {-1, -1};
static std::wstring g_statText;

static double now() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_freq.QuadPart;
}

// ---------------------------------------------------------------- form morphing
static int g_formA = 0, g_formB = 0;
static float g_morph = 0;
static bool g_morphing = false;
static double g_morphT0 = 0, g_morphDur = 1, g_lastAuto = 0;

static void setForm(int f, double dur) {
    int cur = g_morphing ? (g_morph > 0.5f ? g_formB : g_formA) : g_formB;
    g_lastAuto = now();
    S.form = f;
    g_dirty = true;
    if (f == cur && !g_morphing) return;
    g_formA = cur;
    g_formB = f;
    g_morph = 0;
    g_morphT0 = now();
    g_morphDur = dur;
    g_morphing = true;
}

static void updateMorph(double t) {
    if (g_morphing) {
        float x = (float)((t - g_morphT0) / g_morphDur);
        if (x >= 1.f) { g_formA = g_formB; g_morph = 0; g_morphing = false; }
        else g_morph = x * x * (3.f - 2.f * x);
    }
    if (S.autoMorph && !g_morphing && t - g_lastAuto > S.p[P_MORPHSEC]) {
        int next = S.form < NUM_3D ? (S.form + 1) % NUM_3D : 0;
        setForm(next, 3.5);
    }
}

// ---------------------------------------------------------------- audio (WASAPI loopback of what the PC is playing)
static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kIID_IAudioCaptureClient = {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
static const GUID kSUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

struct AudioState {
    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    int channels = 2, bits = 32;
    bool isFloat = true;
    float rate = 48000, lp = 0, env = 0, bassEnv = 0, peak = 0.02f, bassPeak = 0.02f;
    double retryAt = 0;
    float level = 0, bass = 0;  // 0..~1 after auto-gain
} A;

static void audioStop() {
    if (A.client) A.client->Stop();
    rel(A.cap);
    rel(A.client);
}

static bool audioStart() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&A.client);
    if (SUCCEEDED(hr)) hr = A.client->GetMixFormat(&wf);
    if (SUCCEEDED(hr)) hr = A.client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, wf, NULL);
    if (SUCCEEDED(hr)) hr = A.client->GetService(kIID_IAudioCaptureClient, (void**)&A.cap);
    if (SUCCEEDED(hr)) {
        A.channels = std::max<int>(1, wf->nChannels);
        A.rate = (float)wf->nSamplesPerSec;
        A.bits = wf->wBitsPerSample;
        A.isFloat = wf->wFormatTag == 3 /* WAVE_FORMAT_IEEE_FLOAT */ ||
                    (wf->wFormatTag == 0xFFFE /* EXTENSIBLE */ &&
                     IsEqualGUID(((WAVEFORMATEXTENSIBLE*)wf)->SubFormat, kSUBTYPE_FLOAT));
        hr = A.client->Start();
    }
    if (wf) CoTaskMemFree(wf);
    rel(dev);
    rel(en);
    if (FAILED(hr)) { audioStop(); return false; }
    return true;
}

static void audioUpdate(double t, float dt) {
    float relax = 1.f - expf(-dt * 4.f);
    if (!S.audio) {
        if (A.client) audioStop();
        A.level -= A.level * relax;
        A.bass -= A.bass * relax;
        return;
    }
    if (!A.client && (t < A.retryAt || !audioStart())) {
        if (!A.client) A.retryAt = std::max(A.retryAt, t + 2.0);
        A.level -= A.level * relax;
        A.bass -= A.bass * relax;
        return;
    }
    double sum = 0, bsum = 0;
    long n = 0;
    float a = 1.f - expf(-2.f * 3.14159265f * 120.f / A.rate);  // ~120 Hz low-pass for the bass
    UINT32 packet = 0;
    HRESULT hr;
    while (SUCCEEDED(hr = A.cap->GetNextPacketSize(&packet)) && packet > 0) {
        BYTE* data;
        UINT32 frames;
        DWORD flags;
        hr = A.cap->GetBuffer(&data, &frames, &flags, NULL, NULL);
        if (FAILED(hr)) break;
        bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        for (UINT32 f = 0; f < frames; f++) {
            float x = 0;
            if (!silent) {
                for (int c = 0; c < A.channels; c++) {
                    size_t idx = (size_t)f * A.channels + c;
                    if (A.isFloat) x += ((float*)data)[idx];
                    else if (A.bits == 16) x += ((short*)data)[idx] / 32768.f;
                    else if (A.bits == 32) x += ((int*)data)[idx] / 2147483648.f;
                }
                x /= A.channels;
            }
            A.lp += a * (x - A.lp);
            sum += x * x;
            bsum += A.lp * A.lp;
            n++;
        }
        A.cap->ReleaseBuffer(frames);
    }
    if (FAILED(hr)) { audioStop(); A.retryAt = t + 1.0; }  // e.g. headphones plugged in: reconnect

    float rms = n ? (float)sqrt(sum / n) : 0.f, brms = n ? (float)sqrt(bsum / n) : 0.f;
    float att = 1.f - expf(-dt * 35.f), rl = 1.f - expf(-dt * 6.f);
    A.env += (rms > A.env ? att : rl) * (rms - A.env);
    A.bassEnv += (brms > A.bassEnv ? att : rl) * (brms - A.bassEnv);
    float pd = expf(-dt * 0.15f);   // auto-gain: follow the recent peak
    A.peak = std::max(std::max(A.peak * pd, A.env), 0.01f);
    A.bassPeak = std::max(std::max(A.bassPeak * pd, A.bassEnv), 0.01f);
    float sens = S.p[P_SENS];
    float lv = powf(clampf(A.env / A.peak, 0, 1), 1.5f) * sens;
    float bs = powf(clampf(A.bassEnv / A.bassPeak, 0, 1), 2.0f) * sens;
    A.level += (lv - A.level) * (1.f - expf(-dt * 20.f));
    A.bass += (bs - A.bass) * (1.f - expf(-dt * 25.f));
}

// ---------------------------------------------------------------- battery
static int g_batPct = -1;
static bool g_charging = false;
static double g_nextPower = 0;

static void updatePower(double t) {
    if (t < g_nextPower) return;
    g_nextPower = t + 5.0;
    SYSTEM_POWER_STATUS ps;
    if (GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryLifePercent <= 100) {
        g_batPct = ps.BatteryLifePercent;
        g_charging = (ps.BatteryFlag & 8) != 0;
    } else {
        g_batPct = -1;
        g_charging = false;
    }
}

// ---------------------------------------------------------------- now playing (Windows media session, raw WinRT ABI)
// Interface IDs and vtable order were read from Windows.Media.Control.winmd.
static const GUID kIID_SessionManagerStatics = {0x2050C4EE, 0x11A0, 0x57DE, {0xAE, 0xD7, 0xC9, 0x7C, 0x70, 0x33, 0x82, 0x45}};
static const GUID kIID_IAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

template <class... T> static HRESULT vcall(void* o, int slot, T... a) {
    typedef HRESULT(STDMETHODCALLTYPE * F)(void*, T...);
    return ((F)((*(void***)o)[slot]))(o, a...);
}
static void urel(void*& p) { if (p) { ((IUnknown*)p)->Release(); p = nullptr; } }

struct MediaState {
    HRESULT(WINAPI* getFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT(WINAPI* createStr)(LPCWSTR, UINT32, HSTRING*) = nullptr;
    HRESULT(WINAPI* deleteStr)(HSTRING) = nullptr;
    PCWSTR(WINAPI* rawStr)(HSTRING, UINT32*) = nullptr;
    void *statics = nullptr, *mgrOp = nullptr, *mgr = nullptr, *propsOp = nullptr;
    double next = 0, retry = 0;
    bool playing = false;
    std::wstring title, artist;
} M;

static std::wstring takeHString(HSTRING h) {
    std::wstring s;
    if (!h) return s;
    UINT32 len = 0;
    PCWSTR p = M.rawStr(h, &len);
    if (p) s.assign(p, len);
    M.deleteStr(h);
    return s;
}

static int asyncStatus(void* op) {   // 0 started, 1 completed, 2 canceled, 3 error
    void* info = nullptr;
    int st = 3;
    if (SUCCEEDED(((IUnknown*)op)->QueryInterface(kIID_IAsyncInfo, &info)) && info) {
        vcall(info, 7, &st);
        urel(info);
    }
    return st;
}

static void mediaReset() {
    urel(M.propsOp); urel(M.mgr); urel(M.mgrOp);
    M.playing = false;
    M.title.clear();
    M.artist.clear();
}

static void mediaUpdate(double t) {
    if (!S.showMedia) { if (M.mgr || M.mgrOp) mediaReset(); return; }
    if (!M.statics) {
        if (t < M.retry) return;
        M.retry = t + 15.0;
        HMODULE cb = GetModuleHandleW(L"combase.dll");
        if (!cb) cb = LoadLibraryW(L"combase.dll");
        if (!cb) return;
        M.getFactory = (decltype(M.getFactory))(void*)GetProcAddress(cb, "RoGetActivationFactory");
        M.createStr = (decltype(M.createStr))(void*)GetProcAddress(cb, "WindowsCreateString");
        M.deleteStr = (decltype(M.deleteStr))(void*)GetProcAddress(cb, "WindowsDeleteString");
        M.rawStr = (decltype(M.rawStr))(void*)GetProcAddress(cb, "WindowsGetStringRawBuffer");
        if (!M.getFactory || !M.createStr || !M.deleteStr || !M.rawStr) return;
        const wchar_t* cls = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
        HSTRING h = nullptr;
        if (SUCCEEDED(M.createStr(cls, (UINT32)wcslen(cls), &h))) {
            M.getFactory(h, kIID_SessionManagerStatics, &M.statics);
            M.deleteStr(h);
        }
        return;
    }
    if (!M.mgr) {                                         // RequestAsync -> manager
        if (!M.mgrOp) vcall(M.statics, 6, &M.mgrOp);
        else {
            int st = asyncStatus(M.mgrOp);
            if (st == 1) vcall(M.mgrOp, 8, &M.mgr);
            if (st >= 1) urel(M.mgrOp);
        }
        return;
    }
    if (M.propsOp) {                                      // TryGetMediaPropertiesAsync -> title / artist
        int st = asyncStatus(M.propsOp);
        if (st == 1) {
            void* props = nullptr;
            vcall(M.propsOp, 8, &props);
            if (props) {
                HSTRING h = nullptr;
                if (SUCCEEDED(vcall(props, 6, &h))) M.title = takeHString(h);
                h = nullptr;
                if (SUCCEEDED(vcall(props, 9, &h))) M.artist = takeHString(h);
                urel(props);
            }
        }
        if (st >= 1) urel(M.propsOp);
        return;
    }
    if (t < M.next) return;
    M.next = t + 1.5;
    void* sess = nullptr;
    vcall(M.mgr, 6, &sess);                               // GetCurrentSession
    if (!sess) { M.playing = false; return; }
    void* pb = nullptr;
    int status = 0;
    if (SUCCEEDED(vcall(sess, 9, &pb)) && pb) {           // GetPlaybackInfo -> PlaybackStatus
        vcall(pb, 7, &status);
        urel(pb);
    }
    M.playing = status == 4;
    if (M.playing) vcall(sess, 7, &M.propsOp);
    urel(sess);
}

// ---------------------------------------------------------------- Direct3D 11
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain1* g_sc;
static ID3D11RenderTargetView* g_rtv;
static int g_bbW = 0, g_bbH = 0;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader *g_psScene, *g_psComp, *g_psBright, *g_psBlur;
static ID3D11Buffer *g_cbOrb, *g_cbComp, *g_cbPost;
static ID3D11SamplerState* g_samp;
static ID3D11RasterizerState* g_rast;
struct Target { ID3D11Texture2D* tex = nullptr; ID3D11RenderTargetView* rtv = nullptr; ID3D11ShaderResourceView* srv = nullptr; int w = 0, h = 0; };
static Target g_scene, g_bloomA, g_bloomB;
static ID3D11ShaderResourceView* g_textSrv;
static std::wstring g_textKey;
static int g_textW = 0, g_textH = 0, g_textPad = 0, g_textTh = 0;

// Direct2D / DirectWrite (settings panel)
static ID2D1Factory1* g_d2dF;
static ID2D1Device* g_d2dDev;
static ID2D1DeviceContext* g_d2d;
static ID2D1RenderTarget* RT;  // same object, base interface (avoids C++ name hiding of older overloads)
static IDWriteFactory* g_dw;
static ID2D1SolidColorBrush* g_br;
static Target g_ui;
static ID2D1Bitmap1* g_uiBmp;

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                     UINT, UINT, ID3DBlob**, ID3DBlob**);

static ID3DBlob* compileHLSL(PFN_Compile compile, const char* entry, const char* target) {
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = compile(HLSL, strlen(HLSL), "orb.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        MessageBoxA(NULL, err ? (const char*)err->GetBufferPointer() : "D3DCompile failed", "Orb: shader error", MB_ICONERROR);
        ExitProcess(1);
    }
    rel(err);
    return code;
}

static void createBackbufferView() {
    ID3D11Texture2D* bb = nullptr;
    g_sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    g_dev->CreateRenderTargetView(bb, NULL, &g_rtv);
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    g_bbW = (int)td.Width;
    g_bbH = (int)td.Height;
    bb->Release();
}

static void releaseTarget(Target& t) { rel(t.srv); rel(t.rtv); rel(t.tex); t.w = t.h = 0; }

static void ensureTarget(Target& t, int w, int h) {
    if (t.tex && t.w == w && t.h == h) return;
    releaseTarget(t);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, NULL, &t.tex);
    g_dev->CreateRenderTargetView(t.tex, NULL, &t.rtv);
    g_dev->CreateShaderResourceView(t.tex, NULL, &t.srv);
    t.w = w;
    t.h = h;
}

static IDWriteTextFormat *fTitle, *fHdr, *fBody, *fValR, *fSmall, *fSmallC, *fBtn;
static float g_fmtScale = 0;

static void releaseFormats() {
    rel(fTitle); rel(fHdr); rel(fBody); rel(fValR); rel(fSmall); rel(fSmallC); rel(fBtn);
    g_fmtScale = 0;
}

static void releaseD3D() {
    if (g_ctx) g_ctx->ClearState();
    if (g_d2d) g_d2d->SetTarget(nullptr);
    rel(g_uiBmp); releaseTarget(g_ui); rel(g_br); releaseFormats();
    rel(g_d2d); RT = nullptr; rel(g_d2dDev); rel(g_d2dF); rel(g_dw);
    rel(g_textSrv);
    releaseTarget(g_scene); releaseTarget(g_bloomA); releaseTarget(g_bloomB);
    rel(g_rast); rel(g_samp); rel(g_cbPost); rel(g_cbComp); rel(g_cbOrb);
    rel(g_psBlur); rel(g_psBright); rel(g_psComp); rel(g_psScene); rel(g_vs);
    rel(g_rtv); rel(g_sc); rel(g_ctx); rel(g_dev);
    g_textKey.clear();
}

static bool initD3D(HWND hwnd) {
    // Default adapter = whatever Windows picks (Settings > Display > Graphics lets you choose per app).
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx);
    if (FAILED(hr)) return false;

    IDXGIDevice1* dxdev = nullptr;
    IDXGIAdapter* ad = nullptr;
    IDXGIFactory2* f2 = nullptr;
    g_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev);
    dxdev->SetMaximumFrameLatency(1);
    dxdev->GetAdapter(&ad);
    ad->GetParent(__uuidof(IDXGIFactory2), (void**)&f2);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    hr = f2 ? f2->CreateSwapChainForHwnd(g_dev, hwnd, &sd, NULL, NULL, &g_sc) : E_FAIL;
    if (f2) f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    rel(f2); rel(ad);
    if (FAILED(hr)) { rel(dxdev); return false; }
    createBackbufferView();

    // Direct2D on the same device, for the settings panel
    D2D1_FACTORY_OPTIONS fo = {D2D1_DEBUG_LEVEL_NONE};
    if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void**)&g_d2dF)) &&
        SUCCEEDED(g_d2dF->CreateDevice(dxdev, &g_d2dDev)) &&
        SUCCEEDED(g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_d2d))) {
        RT = g_d2d;
        D2D1_COLOR_F white = {1, 1, 1, 1};
        RT->CreateSolidColorBrush(&white, NULL, &g_br);
        RT->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    }
    rel(dxdev);

    // Compiled shaders are cached next to the settings (keyed by a hash of the source), so only the
    // first launch after an update pays the compile.
    const int NS = 5;
    const char* entries[NS] = {"vs", "psScene", "psComp", "psBright", "psBlur"};
    const char* targets[NS] = {"vs_5_0", "ps_5_0", "ps_5_0", "ps_5_0", "ps_5_0"};
    std::string blobs[NS];
    unsigned long long hsh = 1469598103934665603ull;
    for (const char* c = HLSL; *c; c++) hsh = (hsh ^ (unsigned char)*c) * 1099511628211ull;
    std::wstring cachePath = g_dataDir + L"\\shaders.bin";
    bool cached = false;
    if (FILE* f = _wfopen(cachePath.c_str(), L"rb")) {
        unsigned long long fh = 0;
        cached = fread(&fh, 8, 1, f) == 1 && fh == hsh;
        for (int i = 0; cached && i < NS; i++) {
            unsigned int n = 0;
            cached = fread(&n, 4, 1, f) == 1 && n > 0 && n < (64u << 20);
            if (cached) { blobs[i].resize(n); cached = fread(&blobs[i][0], 1, n, f) == n; }
        }
        fclose(f);
    }
    if (!cached) {
        HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
        PFN_Compile compile = dc ? (PFN_Compile)(void*)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) {
            MessageBoxW(NULL, L"d3dcompiler_47.dll is missing (it ships with Windows 10/11).", L"Orb", MB_ICONERROR);
            return false;
        }
        for (int i = 0; i < NS; i++) {
            ID3DBlob* b = compileHLSL(compile, entries[i], targets[i]);
            blobs[i].assign((const char*)b->GetBufferPointer(), b->GetBufferSize());
            b->Release();
        }
        FreeLibrary(dc);
        if (FILE* f = _wfopen(cachePath.c_str(), L"wb")) {
            fwrite(&hsh, 8, 1, f);
            for (auto& b : blobs) {
                unsigned int n = (unsigned int)b.size();
                fwrite(&n, 4, 1, f);
                fwrite(b.data(), 1, n, f);
            }
            fclose(f);
        }
    }
    g_dev->CreateVertexShader(blobs[0].data(), blobs[0].size(), NULL, &g_vs);
    g_dev->CreatePixelShader(blobs[1].data(), blobs[1].size(), NULL, &g_psScene);
    g_dev->CreatePixelShader(blobs[2].data(), blobs[2].size(), NULL, &g_psComp);
    g_dev->CreatePixelShader(blobs[3].data(), blobs[3].size(), NULL, &g_psBright);
    g_dev->CreatePixelShader(blobs[4].data(), blobs[4].size(), NULL, &g_psBlur);
    if (!g_vs || !g_psScene || !g_psComp || !g_psBright || !g_psBlur) { DeleteFileW(cachePath.c_str()); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof(OrbCB);
    g_dev->CreateBuffer(&bd, NULL, &g_cbOrb);
    bd.ByteWidth = sizeof(CompCB);
    g_dev->CreateBuffer(&bd, NULL, &g_cbComp);
    bd.ByteWidth = sizeof(PostCB);
    g_dev->CreateBuffer(&bd, NULL, &g_cbPost);

    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp);

    D3D11_RASTERIZER_DESC rs = {};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rs, &g_rast);
    return true;
}

// ---------------------------------------------------------------- clock text
static HFONT makeFont(int px, int weight, const wchar_t* face, const wchar_t* fallback) {
    HFONT f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH, face);
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ o = SelectObject(dc, f);
    wchar_t got[LF_FACESIZE] = {0};
    GetTextFaceW(dc, LF_FACESIZE, got);
    SelectObject(dc, o);
    DeleteDC(dc);
    if (_wcsicmp(got, face) != 0) {
        DeleteObject(f);
        f = CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                        ANTIALIASED_QUALITY, DEFAULT_PITCH, fallback);
    }
    return f;
}

// Renders time, date/battery and now-playing into a texture. Each element goes into its own colour channel so
// it can get its own opacity: big digits = red (100%), seconds / AM-PM / now playing = green (60%), date = blue (72%).
static void updateClockTexture() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t hm[16], small[16] = L"", date[128] = L"";
    if (S.h24) swprintf(hm, 16, L"%02d:%02d", st.wHour, st.wMinute);
    else swprintf(hm, 16, L"%d:%02d", st.wHour % 12 ? st.wHour % 12 : 12, st.wMinute);
    if (S.seconds) swprintf(small, 16, L"%02d", st.wSecond);
    if (!S.h24) { if (*small) wcscat(small, L" "); wcscat(small, st.wHour < 12 ? L"AM" : L"PM"); }
    if (S.showDate) {
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"dddd, d MMMM", date, 128, NULL);
        CharUpperW(date);
    }
    std::wstring dline = date;
    if (S.showBattery && g_batPct >= 0) {
        wchar_t b[32];
        swprintf(b, 32, g_charging ? L"%d%%  CHARGING" : L"%d%%", g_batPct);
        if (!dline.empty()) dline += L"   ·   ";
        dline += b;
    }
    std::wstring np;
    if (S.showMedia && M.playing && !M.title.empty()) {
        np = L"♪   " + M.title;
        if (!M.artist.empty()) np += L"   —   " + M.artist;
        if (np.size() > 70) np = np.substr(0, 69) + L"…";
    }
    int th = (int)clampf(g_bbH * 0.085f * S.p[P_CLOCKSIZE], 24.f, 420.f);
    wchar_t ks[32];
    swprintf(ks, 32, L"|%d", th);
    std::wstring key = std::wstring(hm) + L"|" + small + L"|" + dline + L"|" + np + ks;
    if (key == g_textKey && g_textSrv) return;
    g_textKey = key;

    int sh = std::max(8, th * 36 / 100), dh = std::max(9, th * 17 / 100), nh = std::max(9, th * 19 / 100);
    HFONT fBig = makeFont(th, FW_LIGHT, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fSm = makeFont(sh, FW_NORMAL, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fDate = makeFont(dh, FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");
    HFONT fNp = makeFont(nh, FW_NORMAL, L"Segoe UI Variable Text", L"Segoe UI");
    HDC mdc = CreateCompatibleDC(NULL);
    HGDIOBJ oldFont = SelectObject(mdc, fBig);

    int bigExtra = th / 40, dateExtra = dh * 28 / 100, gapSmall = th / 9;
    TEXTMETRICW tmB, tmD, tmN;
    SIZE sBig, sSmall = {0, 0}, sDate = {0, 0}, sNp = {0, 0};
    SetTextCharacterExtra(mdc, bigExtra);
    GetTextMetricsW(mdc, &tmB);
    GetTextExtentPoint32W(mdc, hm, (int)wcslen(hm), &sBig);
    SelectObject(mdc, fSm);
    SetTextCharacterExtra(mdc, 0);
    if (*small) GetTextExtentPoint32W(mdc, small, (int)wcslen(small), &sSmall);
    SelectObject(mdc, fDate);
    SetTextCharacterExtra(mdc, dateExtra);
    GetTextMetricsW(mdc, &tmD);
    if (!dline.empty()) GetTextExtentPoint32W(mdc, dline.c_str(), (int)dline.size(), &sDate);
    SelectObject(mdc, fNp);
    SetTextCharacterExtra(mdc, 0);
    GetTextMetricsW(mdc, &tmN);
    if (!np.empty()) GetTextExtentPoint32W(mdc, np.c_str(), (int)np.size(), &sNp);

    int pad = std::max(4, th / 8);
    int capB = tmB.tmAscent - tmB.tmInternalLeading;
    int line1W = sBig.cx + (*small ? gapSmall + sSmall.cx : 0);
    int line1H = capB + tmB.tmDescent / 4;
    int gap = th * 22 / 100, gap3 = th * 16 / 100;
    int capD = tmD.tmAscent - tmD.tmInternalLeading;
    int line2H = !dline.empty() ? capD + tmD.tmDescent : 0;
    int capN = tmN.tmAscent - tmN.tmInternalLeading;
    int line3H = !np.empty() ? capN + tmN.tmDescent : 0;
    int W = std::max(std::max(line1W, (int)sDate.cx), (int)sNp.cx) + pad * 2;
    int H = line1H + (line2H ? gap + line2H : 0) + (line3H ? gap3 + line3H : 0) + pad * 2;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(mdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ oldBmp = SelectObject(mdc, bmp);
    memset(bits, 0, (size_t)W * H * 4);
    SetBkMode(mdc, TRANSPARENT);
    SetTextAlign(mdc, TA_BASELINE | TA_LEFT);

    int x0 = (W - line1W) / 2, base1 = pad + capB, y = pad + line1H;
    SelectObject(mdc, fBig);
    SetTextCharacterExtra(mdc, bigExtra);
    SetTextColor(mdc, RGB(255, 0, 0));
    TextOutW(mdc, x0, base1, hm, (int)wcslen(hm));
    if (*small) {
        SelectObject(mdc, fSm);
        SetTextCharacterExtra(mdc, 0);
        SetTextColor(mdc, RGB(0, 255, 0));
        TextOutW(mdc, x0 + sBig.cx + gapSmall, base1, small, (int)wcslen(small));
    }
    if (line2H) {
        SelectObject(mdc, fDate);
        SetTextCharacterExtra(mdc, dateExtra);
        SetTextColor(mdc, RGB(0, 0, 255));
        TextOutW(mdc, (W - sDate.cx) / 2 + dateExtra / 2, y + gap + capD, dline.c_str(), (int)dline.size());
        y += gap + line2H;
    }
    if (line3H) {
        SelectObject(mdc, fNp);
        SetTextCharacterExtra(mdc, 0);
        SetTextColor(mdc, RGB(0, 255, 0));
        TextOutW(mdc, (W - sNp.cx) / 2, y + gap3 + capN, np.c_str(), (int)np.size());
    }
    GdiFlush();

    unsigned char* px = (unsigned char*)bits;  // BGRA
    for (size_t i = 0; i < (size_t)W * H; i++) {
        unsigned char* q = px + i * 4;
        int a = std::max<int>(q[2], std::max<int>(q[1] * 60 / 100, q[0] * 72 / 100));
        q[0] = q[1] = q[2] = 255;
        q[3] = (unsigned char)a;
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W;
    td.Height = H;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd = {bits, (UINT)W * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    rel(g_textSrv);
    if (SUCCEEDED(g_dev->CreateTexture2D(&td, &srd, &tex))) {
        g_dev->CreateShaderResourceView(tex, NULL, &g_textSrv);
        tex->Release();
    }
    g_textW = W;
    g_textH = H;
    g_textPad = pad;
    g_textTh = th;

    SelectObject(mdc, oldBmp);
    SelectObject(mdc, oldFont);
    DeleteObject(bmp);
    DeleteObject(fBig);
    DeleteObject(fSm);
    DeleteObject(fDate);
    DeleteObject(fNp);
    DeleteDC(mdc);
}

// ---------------------------------------------------------------- fullscreen + save
static void toggleFullscreen() {
    DWORD style = (DWORD)GetWindowLongPtrW(g_main, GWL_STYLE);
    if (!g_fs) {
        MONITORINFO mi = {sizeof mi};
        if (GetWindowPlacement(g_main, &g_wpPrev) &&
            GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(g_main, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(g_main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            g_fs = true;
        }
    } else {
        SetWindowLongPtrW(g_main, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_main, &g_wpPrev);
        SetWindowPos(g_main, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fs = false;
    }
    g_dirty = true;
}

static void saveSettings() {
    for (int i = 0; i < NP; i++) iniWF(L"orb", PD[i].key, S.p[i]);
    for (int i = 0; i < 4; i++) {
        wchar_t key[8], b[16];
        swprintf(key, 8, L"c%d", i);
        swprintf(b, 16, L"#%02x%02x%02x", GetRValue(S.c[i]), GetGValue(S.c[i]), GetBValue(S.c[i]));
        WritePrivateProfileStringW(L"orb", key, b, g_ini.c_str());
    }
    iniWF(L"orb", L"drift", S.drift);
    iniWF(L"orb", L"automorph", S.autoMorph);
    iniWF(L"orb", L"h24", S.h24);
    iniWF(L"orb", L"seconds", S.seconds);
    iniWF(L"orb", L"date", S.showDate);
    iniWF(L"orb", L"audio", S.audio);
    iniWF(L"orb", L"battery", S.showBattery);
    iniWF(L"orb", L"media", S.showMedia);
    iniWF(L"orb", L"aa", S.aa);
    iniWF(L"orb", L"fps", S.fps);
    iniWF(L"orb", L"form", S.form);
    iniWF(L"orb", L"clockstyle", S.clockStyle);
    iniWF(L"orb", L"clockpos", S.clockPos);

    if (g_main) {
        WINDOWPLACEMENT wp = {sizeof wp};
        if (g_fs) wp = g_wpPrev;
        else GetWindowPlacement(g_main, &wp);
        RECT& r = wp.rcNormalPosition;
        iniWF(L"window", L"x", r.left);
        iniWF(L"window", L"y", r.top);
        iniWF(L"window", L"w", r.right - r.left);
        iniWF(L"window", L"h", r.bottom - r.top);
        iniWF(L"window", L"fullscreen", g_fs);
    }
    g_dirty = false;
}

static void pickColor(int i) {
    static COLORREF custom[16];
    CHOOSECOLORW cc = {sizeof cc};
    cc.hwndOwner = g_main;
    cc.rgbResult = S.c[i];
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) {
        S.c[i] = cc.rgbResult;
        g_dirty = true;
    }
}

// ---------------------------------------------------------------- settings panel (immediate-mode UI, Direct2D)
// The panel is drawn every frame while visible into a texture; the composite shader puts it on a dark liquid-glass
// sheet that slides in from the right. Every widget eases its hover / press / value state for fluid motion.
static bool g_uiOpen = false;
static float g_uiT = 0;
static float g_uiRect[4];
static float g_mx = -1, g_my = -1, g_wheel = 0;
static bool g_mDown = false, g_mPressed = false;
static int g_uiActive = -1, g_pendingColor = -1;
static float g_scroll = 0, g_scrollT = 0, g_contentH = 0;
static float g_anim[512][2];
static bool g_animInit[512];
static float g_secH[16];

struct UiFrame {
    float s, W, H, pad, cw, y, mx, my, dt;
    bool inside, pressed, down;
    int id, sec;
    float secY;
    D2D1_COLOR_F acc;
} U;

static D2D1_COLOR_F C4(float r, float g, float b, float a) { D2D1_COLOR_F c = {r, g, b, a}; return c; }
static D2D1_COLOR_F WA(float a) { return C4(1, 1, 1, a); }
static D2D1_COLOR_F CRA(COLORREF c, float a) { return C4(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, a); }
static D2D1_COLOR_F withA(D2D1_COLOR_F c, float a) { c.a = a; return c; }
static D2D1_COLOR_F mixC(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return C4(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
static D2D1_RECT_F RC(float x, float y, float w, float h) { D2D1_RECT_F r = {x, y, x + w, y + h}; return r; }
static D2D1_POINT_2F PT(float x, float y) { D2D1_POINT_2F p = {x, y}; return p; }

static void fRR(float x, float y, float w, float h, float r, D2D1_COLOR_F c) {
    g_br->SetColor(&c);
    D2D1_ROUNDED_RECT rr = {RC(x, y, w, h), r, r};
    RT->FillRoundedRectangle(&rr, g_br);
}
static void sRR(float x, float y, float w, float h, float r, D2D1_COLOR_F c, float th) {
    g_br->SetColor(&c);
    D2D1_ROUNDED_RECT rr = {RC(x, y, w, h), r, r};
    RT->DrawRoundedRectangle(&rr, g_br, th, NULL);
}
static void fEl(float cx, float cy, float r, D2D1_COLOR_F c) {
    g_br->SetColor(&c);
    D2D1_ELLIPSE e = {PT(cx, cy), r, r};
    RT->FillEllipse(&e, g_br);
}
static void sEl(float cx, float cy, float rx, float ry, D2D1_COLOR_F c, float th) {
    g_br->SetColor(&c);
    D2D1_ELLIPSE e = {PT(cx, cy), rx, ry};
    RT->DrawEllipse(&e, g_br, th, NULL);
}
static void ln(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float th) {
    g_br->SetColor(&c);
    RT->DrawLine(PT(x0, y0), PT(x1, y1), g_br, th, NULL);
}
static void txt(const std::wstring& s, IDWriteTextFormat* f, float x, float y, float w, float h, D2D1_COLOR_F c) {
    if (!f || s.empty()) return;
    g_br->SetColor(&c);
    D2D1_RECT_F r = RC(x, y, w, h);
    RT->DrawText(s.c_str(), (UINT32)s.size(), f, &r, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}
static void ease(float& v, float target, float rate) { v += (target - v) * (1.f - expf(-rate * U.dt)); }
static float& AN(int id, int k, float init) {
    id &= 511;
    if (!g_animInit[id]) { g_animInit[id] = true; g_anim[id][0] = 0; g_anim[id][1] = init; }
    return g_anim[id][k];
}
static bool over(float x, float y, float w, float h) {
    return U.inside && U.mx >= x && U.mx < x + w && U.my >= y && U.my < y + h;
}

static IDWriteTextFormat* makeFormat(const wchar_t* family, DWRITE_FONT_WEIGHT wt, float size,
                                     DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa, bool wrap) {
    IDWriteTextFormat* f = nullptr;
    if (!g_dw) return nullptr;
    g_dw->CreateTextFormat(family, NULL, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &f);
    if (f) {
        f->SetTextAlignment(ta);
        f->SetParagraphAlignment(pa);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

static void makeFormats(float s) {
    releaseFormats();
    const wchar_t *disp = L"Segoe UI Variable Display", *text = L"Segoe UI Variable Text";
    fTitle = makeFormat(disp, DWRITE_FONT_WEIGHT_SEMI_BOLD, 24 * s, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fHdr = makeFormat(text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 11 * s, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fBody = makeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 13.5f * s, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fValR = makeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 12.5f * s, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fSmall = makeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 11.5f * s, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    fSmallC = makeFormat(text, DWRITE_FONT_WEIGHT_NORMAL, 11.5f * s, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    fBtn = makeFormat(text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13 * s, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    g_fmtScale = s;
}

static void uiEndSection() {
    if (U.sec >= 0) g_secH[U.sec] = U.y - U.secY;
    U.sec = -1;
}
static void uiSection(int sec, const wchar_t* title) {
    uiEndSection();
    U.y += 16 * U.s;
    txt(title, fHdr, U.pad, U.y, U.cw, 16 * U.s, WA(0.42f));
    U.y += 26 * U.s;
    if (g_secH[sec] > 0) fRR(U.pad - 14 * U.s, U.y - 8 * U.s, U.cw + 28 * U.s, g_secH[sec] + 14 * U.s, 16 * U.s, WA(0.045f));
    U.sec = sec;
    U.secY = U.y;
}

static void fmtVal(int i, wchar_t* b) {
    if (PD[i].fmt == FMT_PCT) swprintf(b, 32, L"%d%%", (int)lroundf(S.p[i] * 100));
    else if (PD[i].fmt == FMT_SEC) swprintf(b, 32, L"%d s", (int)lroundf(S.p[i]));
    else swprintf(b, 32, L"%.2f", S.p[i]);
}

static void uiSlider(int pi) {
    int id = U.id++;
    float x = U.pad, w = U.cw, h = 46 * U.s, y = U.y;
    bool hover = over(x - 6 * U.s, y, w + 12 * U.s, h);
    if (hover && U.pressed) g_uiActive = id;
    bool act = g_uiActive == id;
    float range = PD[pi].mx - PD[pi].mn;
    if (act && U.down) {
        float nv = PD[pi].mn + range * clampf((U.mx - x) / w, 0, 1);
        if (nv != S.p[pi]) { S.p[pi] = nv; g_dirty = true; }
    }
    float target = (S.p[pi] - PD[pi].mn) / range;
    float& va = AN(id, 1, target);
    float& ha = AN(id, 0, 0);
    ease(ha, (hover || act) ? 1.f : 0.f, 14);
    ease(va, target, act ? 40.f : 14.f);

    wchar_t vb[32];
    fmtVal(pi, vb);
    txt(PD[pi].label, fBody, x, y + 2 * U.s, w, 22 * U.s, WA(0.82f + 0.18f * ha));
    txt(vb, fValR, x, y + 2 * U.s, w, 22 * U.s, WA(0.45f + 0.35f * ha));
    float ty = y + 33 * U.s, th = (4 + 2 * ha) * U.s;
    fRR(x, ty - th / 2, w, th, th / 2, WA(0.10f + 0.04f * ha));
    if (va > 0.002f) fRR(x, ty - th / 2, std::max(th, w * va), th, th / 2, withA(U.acc, 0.85f + 0.15f * ha));
    float kx = x + w * va, kr = (6.5f + 2.5f * ha) * U.s;
    if (ha > 0.01f) fEl(kx, ty, kr + 7 * U.s * ha, withA(U.acc, 0.22f * ha));
    fEl(kx, ty, kr, WA(1));
    U.y += h;
}

static bool uiToggle(const wchar_t* label, bool* v) {
    int id = U.id++;
    float x = U.pad, w = U.cw, h = 40 * U.s, y = U.y;
    bool hover = over(x - 6 * U.s, y, w + 12 * U.s, h), changed = false;
    if (hover && U.pressed) { *v = !*v; g_dirty = true; changed = true; }
    float& a = AN(id, 1, *v ? 1.f : 0.f);
    float& ha = AN(id, 0, 0);
    ease(a, *v ? 1.f : 0.f, 13);
    ease(ha, hover ? 1.f : 0.f, 14);
    txt(label, fBody, x, y, w - 60 * U.s, h, WA(0.82f + 0.18f * ha));
    float sw = 42 * U.s, sh = 24 * U.s, sx = x + w - sw, sy = y + (h - sh) / 2;
    fRR(sx, sy, sw, sh, sh / 2, mixC(WA(0.12f + 0.06f * ha), withA(U.acc, 0.95f), a));
    float kr = sh / 2 - 3 * U.s;
    float kx = sx + sh / 2 + (sw - sh) * a;
    fEl(kx, sy + sh / 2 + 1 * U.s, kr, C4(0, 0, 0, 0.18f));
    fEl(kx, sy + sh / 2, kr * (1 + 0.06f * ha), WA(1));
    U.y += h;
    return changed;
}

static void uiSegment(const wchar_t* const* items, int n, int* sel) {
    int id = U.id++;
    float x = U.pad, w = U.cw, h = 36 * U.s, y = U.y + 2 * U.s, iw = w / n;
    for (int i = 0; i < n; i++)
        if (over(x + i * iw, y, iw, h) && U.pressed) { *sel = i; g_dirty = true; }
    float& pos = AN(id, 1, (float)*sel);
    ease(pos, (float)*sel, 14);
    fRR(x, y, w, h, h / 2, WA(0.06f));
    fRR(x + pos * iw + 3 * U.s, y + 3 * U.s, iw - 6 * U.s, h - 6 * U.s, (h - 6 * U.s) / 2, withA(U.acc, 0.32f));
    sRR(x + pos * iw + 3 * U.s, y + 3 * U.s, iw - 6 * U.s, h - 6 * U.s, (h - 6 * U.s) / 2, withA(U.acc, 0.7f), 1 * U.s);
    for (int i = 0; i < n; i++) {
        float k = 1.f - std::min(1.f, fabsf(pos - i));
        txt(items[i], fBtn, x + i * iw, y, iw, h, WA(0.55f + 0.45f * k));
    }
    U.y += h + 10 * U.s;
}

static int uiCycle(const wchar_t* label, const wchar_t* value) {
    int id = U.id++;
    float x = U.pad, w = U.cw, h = 40 * U.s, y = U.y;
    float pw = 176 * U.s, ph = 30 * U.s, px = x + w - pw, py = y + (h - ph) / 2;
    bool hl = over(px, py, pw / 2, ph), hr = over(px + pw / 2, py, pw / 2, ph);
    float& a = AN(id, 0, 0);
    ease(a, (hl || hr) ? 1.f : 0.f, 14);
    txt(label, fBody, x, y, w - pw, h, WA(0.82f));
    fRR(px, py, pw, ph, ph / 2, WA(0.07f + 0.05f * a));
    txt(value, fBtn, px + 20 * U.s, py, pw - 40 * U.s, ph, WA(0.92f));
    txt(L"‹", fBtn, px + 4 * U.s, py - 1 * U.s, 22 * U.s, ph, WA(hl ? 1.f : 0.45f));
    txt(L"›", fBtn, px + pw - 26 * U.s, py - 1 * U.s, 22 * U.s, ph, WA(hr ? 1.f : 0.45f));
    U.y += h;
    if (U.pressed && hl) return -1;
    if (U.pressed && hr) return 1;
    return 0;
}

static int uiButtons2(const wchar_t* a, const wchar_t* b) {
    float gap = 10 * U.s, bw = (U.cw - gap) / 2, h = 38 * U.s, y = U.y + 6 * U.s;
    int res = 0;
    for (int k = 0; k < 2; k++) {
        int id = U.id++;
        float x = U.pad + k * (bw + gap);
        bool hv = over(x, y, bw, h);
        float& ha = AN(id, 0, 0);
        ease(ha, hv ? 1.f : 0.f, 14);
        D2D1_COLOR_F bg = k == 0 ? withA(U.acc, 0.30f + 0.20f * ha) : WA(0.07f + 0.06f * ha);
        fRR(x, y, bw, h, h / 2, bg);
        txt(k == 0 ? a : b, fBtn, x, y, bw, h, WA(0.95f));
        if (hv && U.pressed) res = k + 1;
    }
    U.y += h + 12 * U.s;
    return res;
}

static void drawFormIcon(int f, float cx, float cy, float r, D2D1_COLOR_F c) {
    float th = 1.6f * U.s;
    switch (f) {
    case 0:
        sEl(cx, cy, r, r, c, th);
        fEl(cx - r * 0.32f, cy - r * 0.32f, r * 0.26f, withA(c, c.a * 0.8f));
        break;
    case 1:
        sEl(cx - r * 0.35f, cy + r * 0.12f, r * 0.62f, r * 0.62f, c, th);
        sEl(cx + r * 0.40f, cy - r * 0.22f, r * 0.52f, r * 0.52f, c, th);
        break;
    case 2:
        sEl(cx, cy, r, r * 0.55f, c, th * 1.8f);
        break;
    case 3: {
        float a = r * 0.87f, b = r * 0.5f;
        ln(cx, cy - r, cx + a, cy - b, c, th); ln(cx + a, cy - b, cx + a, cy + b, c, th);
        ln(cx + a, cy + b, cx, cy + r, c, th); ln(cx, cy + r, cx - a, cy + b, c, th);
        ln(cx - a, cy + b, cx - a, cy - b, c, th); ln(cx - a, cy - b, cx, cy - r, c, th);
        ln(cx, cy, cx - a, cy - b, c, th); ln(cx, cy, cx + a, cy - b, c, th); ln(cx, cy, cx, cy + r, c, th);
        break;
    }
    case 4: {
        float a = r * 0.72f;
        ln(cx, cy - r, cx + a, cy - r * 0.1f, c, th); ln(cx + a, cy - r * 0.1f, cx, cy + r, c, th);
        ln(cx, cy + r, cx - a, cy - r * 0.1f, c, th); ln(cx - a, cy - r * 0.1f, cx, cy - r, c, th);
        ln(cx - a, cy - r * 0.1f, cx + a, cy - r * 0.1f, withA(c, c.a * 0.6f), th);
        break;
    }
    case 5:
        for (int k = 0; k < 3; k++) {
            float yy = cy + (k - 1) * r * 0.48f;
            float px0 = cx - r, py0 = yy;
            for (int i = 1; i <= 12; i++) {
                float px1 = cx - r + 2 * r * i / 12.f, py1 = yy + sinf(i * 0.9f + k) * r * 0.16f;
                ln(px0, py0, px1, py1, withA(c, c.a * (1.f - 0.25f * k)), th);
                px0 = px1;
                py0 = py1;
            }
        }
        break;
    default:
        for (int k = 0; k < 10; k++) {
            float a = k * 0.85f, rr = r * (0.1f + k * 0.09f);
            fEl(cx + cosf(a) * rr, cy + sinf(a) * rr, r * (0.17f - k * 0.011f), c);
        }
        break;
    }
}

static void uiForms() {
    float gap = 8 * U.s, cwid = (U.cw - gap * 3) / 4, ch = 70 * U.s;
    for (int i = 0; i < NUM_FORMS; i++) {
        int id = U.id++;
        float x = U.pad + (i % 4) * (cwid + gap), y = U.y + (i / 4) * (ch + gap);
        bool hv = over(x, y, cwid, ch);
        if (hv && U.pressed && S.form != i) setForm(i, 1.6);
        float& sa = AN(id, 1, S.form == i ? 1.f : 0.f);
        float& ha = AN(id, 0, 0);
        ease(ha, hv ? 1.f : 0.f, 14);
        ease(sa, S.form == i ? 1.f : 0.f, 10);
        fRR(x, y, cwid, ch, 14 * U.s, mixC(WA(0.05f + 0.05f * ha), withA(U.acc, 0.24f), sa));
        if (sa > 0.01f) sRR(x + 0.5f, y + 0.5f, cwid - 1, ch - 1, 14 * U.s, withA(U.acc, 0.85f * sa), 1.2f * U.s);
        D2D1_COLOR_F ic = mixC(WA(0.62f + 0.25f * ha), WA(1), sa);
        drawFormIcon(i, x + cwid / 2, y + 27 * U.s - 2 * U.s * ha, 11.5f * U.s, ic);
        txt(FORM_NAMES[i], fSmallC, x, y + 46 * U.s, cwid, 18 * U.s, WA(0.55f + 0.45f * std::max(ha, sa)));
    }
    U.y += 2 * (ch + gap) + 4 * U.s;
}

static void uiPalette() {
    float d = 32 * U.s, gap = 12 * U.s;
    int per = std::max(1, (int)((U.cw + gap) / (d + gap)));
    const wchar_t* caption = L"Custom";
    for (int i = 0; i < NUM_PRESETS; i++) {
        bool on = true;
        for (int k = 0; k < 4; k++) on = on && PRESETS[i].c[k] == S.c[k];
        if (on) caption = PRESETS[i].name;
    }
    for (int i = 0; i < NUM_PRESETS; i++) {
        int id = U.id++;
        const Preset& p = PRESETS[i];
        float cx = U.pad + (i % per) * (d + gap) + d / 2, cy = U.y + (i / per) * (d + gap) + d / 2;
        bool hv = over(cx - d / 2, cy - d / 2, d, d);
        bool on = true;
        for (int k = 0; k < 4; k++) on = on && p.c[k] == S.c[k];
        if (hv && U.pressed) { for (int k = 0; k < 4; k++) S.c[k] = p.c[k]; g_dirty = true; }
        if (hv) caption = p.name;
        float& sa = AN(id, 1, on ? 1.f : 0.f);
        float& ha = AN(id, 0, 0);
        ease(ha, hv ? 1.f : 0.f, 14);
        ease(sa, on ? 1.f : 0.f, 12);
        float r = d / 2 * (1 + 0.08f * ha);
        D2D1_GRADIENT_STOP stops[3] = {{0.f, CRA(p.c[1], 1)}, {0.55f, CRA(p.c[2], 1)}, {1.f, CRA(p.c[3], 1)}};
        ID2D1GradientStopCollection* coll = nullptr;
        ID2D1LinearGradientBrush* gb = nullptr;
        RT->CreateGradientStopCollection(stops, 3, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &coll);
        if (coll) {
            D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lp = {PT(cx - r, cy - r), PT(cx + r, cy + r)};
            RT->CreateLinearGradientBrush(&lp, NULL, coll, &gb);
        }
        if (gb) {
            D2D1_ELLIPSE e = {PT(cx, cy), r, r};
            RT->FillEllipse(&e, gb);
        }
        rel(gb);
        rel(coll);
        if (sa > 0.01f) sEl(cx, cy, r + 4.5f * U.s, r + 4.5f * U.s, WA(0.9f * sa), 2 * U.s);
    }
    int rows = (NUM_PRESETS + per - 1) / per;
    U.y += rows * (d + gap);
    txt(caption, fSmall, U.pad, U.y - 4 * U.s, U.cw, 18 * U.s, WA(0.5f));
    U.y += 20 * U.s;

    float gap2 = 8 * U.s, bw = (U.cw - gap2 * 3) / 4, bh = 34 * U.s;
    for (int i = 0; i < 4; i++) {
        int id = U.id++;
        float x = U.pad + i * (bw + gap2);
        bool hv = over(x, U.y, bw, bh);
        if (hv && U.pressed) g_pendingColor = i;
        float& ha = AN(id, 0, 0);
        ease(ha, hv ? 1.f : 0.f, 14);
        COLORREF c = S.c[i];
        fRR(x, U.y, bw, bh, 10 * U.s, CRA(c, 1));
        sRR(x + 0.5f, U.y + 0.5f, bw - 1, bh - 1, 10 * U.s, WA(0.12f + 0.5f * ha), 1 * U.s);
        int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        txt(COLOR_NAMES[i], fSmallC, x, U.y, bw, bh, lum > 140 ? C4(0.04f, 0.06f, 0.1f, 0.9f) : WA(0.9f));
    }
    U.y += bh + 8 * U.s;
}

static void drawUi(float dt) {
    if (!RT || !g_dw) return;
    float s = clampf(g_bbH / 1080.f, 0.8f, 2.0f);
    float m = 16 * s, w = floorf(std::min(410 * s, g_bbW - 2 * m)), h = floorf(g_bbH - 2 * m);
    if (w < 50 || h < 50) return;
    ensureTarget(g_ui, (int)w, (int)h);
    static ID3D11Texture2D* bmpTex = nullptr;   // the texture g_uiBmp draws into; rebuild when it was replaced
    if (!g_uiBmp || bmpTex != g_ui.tex) {
        bmpTex = g_ui.tex;
        rel(g_uiBmp);
        IDXGISurface* surf = nullptr;
        g_ui.tex->QueryInterface(__uuidof(IDXGISurface), (void**)&surf);
        D2D1_BITMAP_PROPERTIES1 bp = {};
        bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        bp.dpiX = bp.dpiY = 96;
        bp.bitmapOptions = (D2D1_BITMAP_OPTIONS)(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);
        if (surf) g_d2d->CreateBitmapFromDxgiSurface(surf, &bp, &g_uiBmp);
        rel(surf);
        if (!g_uiBmp) return;
    }
    if (fabsf(s - g_fmtScale) > 0.01f) makeFormats(s);

    float e = g_uiT;
    float x = (float)g_bbW - m - w + (1.f - e) * (w + m + 40 * s);
    g_uiRect[0] = floorf(x); g_uiRect[1] = m; g_uiRect[2] = w; g_uiRect[3] = h;
    float lx = g_mx - g_uiRect[0], ly = g_my - g_uiRect[1];
    bool inPanel = g_uiOpen && lx >= 0 && ly >= 0 && lx < w && ly < h;
    float headerH = 70 * s;

    U = UiFrame();
    U.s = s; U.W = w; U.H = h; U.dt = dt; U.pad = 30 * s; U.cw = w - 2 * U.pad; U.sec = -1;
    if (inPanel && g_wheel != 0) g_scrollT -= g_wheel / 120.f * 80 * s;
    float maxScroll = std::max(0.f, g_contentH - (h - headerH));
    g_scrollT = clampf(g_scrollT, 0, maxScroll);
    ease(g_scroll, g_scrollT, 16);
    U.mx = lx;
    U.my = ly - headerH + g_scroll;
    U.inside = inPanel && ly > headerH;
    U.pressed = g_mPressed && U.inside;
    U.down = g_mDown;
    if (!g_mDown) g_uiActive = -1;
    // accent for the UI = the palette's Mid colour, nudged brighter so it always reads on the dark glass
    D2D1_COLOR_F acc = CRA(S.c[1], 1);
    float al = 0.3f * acc.r + 0.59f * acc.g + 0.11f * acc.b;
    U.acc = mixC(acc, WA(1), al < 0.45f ? 0.3f : 0.1f);

    g_d2d->SetTarget(g_uiBmp);
    RT->BeginDraw();
    D2D1_COLOR_F clear = {0, 0, 0, 0};
    RT->Clear(&clear);
    D2D1_MATRIX_3X2_F I = {};
    I._11 = I._22 = 1;
    RT->SetTransform(&I);

    // header
    txt(L"Orb", fTitle, U.pad, 20 * s, 120 * s, 32 * s, WA(0.96f));
    txt(L"v" ORB_VERSION_W, fSmall, U.pad + 52 * s, 31 * s, 100 * s, 18 * s, WA(0.38f));
    {
        float cs = 32 * s, cx = w - U.pad - cs + 6 * s, cy = 20 * s;
        bool hv = g_uiOpen && lx >= cx && lx < cx + cs && ly >= cy && ly < cy + cs;
        float& ha = AN(500, 0, 0);
        ease(ha, hv ? 1.f : 0.f, 14);
        fEl(cx + cs / 2, cy + cs / 2, cs / 2, WA(0.06f + 0.10f * ha));
        float k = 5 * s;
        ln(cx + cs / 2 - k, cy + cs / 2 - k, cx + cs / 2 + k, cy + cs / 2 + k, WA(0.7f + 0.3f * ha), 1.6f * s);
        ln(cx + cs / 2 + k, cy + cs / 2 - k, cx + cs / 2 - k, cy + cs / 2 + k, WA(0.7f + 0.3f * ha), 1.6f * s);
        if (hv && g_mPressed) g_uiOpen = false;
    }
    ln(U.pad - 14 * s, headerH - 0.5f, w - U.pad + 14 * s, headerH - 0.5f, WA(0.07f), 1);

    // scrolling content
    D2D1_RECT_F clip = RC(0, headerH, w, h - headerH);
    RT->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_MATRIX_3X2_F T = I;
    T._32 = headerH - g_scroll;
    RT->SetTransform(&T);
    U.y = 0;

    uiSection(0, L"FORM");
    uiForms();
    if (uiToggle(L"Auto-morph between 3D forms", &S.autoMorph)) g_lastAuto = now();
    uiSlider(P_MORPHSEC);

    uiSection(1, L"COLOURS");
    uiPalette();
    uiSlider(P_HUE);

    uiSection(2, L"CLOCK");
    uiSegment(CLOCK_STYLES, 3, &S.clockStyle);
    if (int dlt = uiCycle(L"Position", CLOCK_POS[S.clockPos])) { S.clockPos = (S.clockPos + dlt + 7) % 7; g_dirty = true; }
    uiToggle(L"24-hour time", &S.h24);
    uiToggle(L"Seconds", &S.seconds);
    uiToggle(L"Date", &S.showDate);
    uiToggle(L"Battery", &S.showBattery);
    uiToggle(L"Now playing", &S.showMedia);
    uiSlider(P_CLOCKSIZE);
    uiSlider(P_FROST);
    uiSlider(P_REFR);

    uiSection(3, L"SHAPE & MOTION");
    for (int i = P_SIZE; i <= P_BREATHE; i++) uiSlider(i);

    uiSection(4, L"LOOK");
    const int look[] = {P_BLOOM, P_PARTICLES, P_GLOW, P_GRAIN, P_ACCENT, P_BRIGHT, P_CONTRAST, P_STARS};
    for (int i : look) uiSlider(i);

    uiSection(5, L"SOUND");
    uiToggle(L"React to what's playing", &S.audio);
    uiSlider(P_SENS);

    uiSection(6, L"DISPLAY");
    uiToggle(L"Anti burn-in drift", &S.drift);
    {
        wchar_t fb[32];
        if (S.fps > 0) swprintf(fb, 32, L"%d fps", S.fps);
        else wcscpy(fb, L"Max (VSync)");
        if (int dlt = uiCycle(L"Frame rate cap", fb)) {
            int k = 0;
            for (int i = 0; i < 4; i++) if (FPS_OPTS[i] == S.fps) k = i;
            S.fps = FPS_OPTS[(k + dlt + 4) % 4];
            g_dirty = true;
        }
    }
    if (int dlt = uiCycle(L"Anti-aliasing", AA_NAMES[S.aa])) { S.aa = (S.aa + dlt + 3) % 3; g_dirty = true; }
    uiSlider(P_SCALE);
    int b = uiButtons2(g_fs ? L"Exit fullscreen" : L"Fullscreen", L"Reset all");
    if (b == 1) toggleFullscreen();
    if (b == 2) { defaults(); setForm(0, 1.6); g_dirty = true; }
    uiEndSection();

    U.y += 18 * s;
    txt(g_statText, fSmall, U.pad, U.y, U.cw, 18 * s, WA(0.38f));
    U.y += 22 * s;
    txt(L"H settings  ·  F fullscreen  ·  1–7 forms  ·  M auto-morph\n"
        L"C clock style  ·  A sound  ·  Space pause  ·  Esc close",
        fSmall, U.pad, U.y, U.cw, 40 * s, WA(0.32f));
    U.y += 46 * s;
    g_contentH = U.y + 10 * s;

    RT->SetTransform(&I);
    RT->PopAxisAlignedClip();

    // scrollbar + soft fade under the header
    if (maxScroll > 1) {
        float vh = h - headerH, bh = std::max(30 * s, vh * vh / g_contentH);
        float by = headerH + (vh - bh) * (g_scroll / maxScroll);
        fRR(w - 8 * s, by + 4 * s, 3 * s, bh - 8 * s, 1.5f * s, WA(inPanel ? 0.22f : 0.10f));
    }
    RT->EndDraw();
    g_d2d->SetTarget(nullptr);
}

// ---------------------------------------------------------------- render
static void hueRotate(float* c, float a) {
    float cs = cosf(a), sn = sinf(a), k = 1.f / 3.f, sq = sqrtf(k);
    float m0 = cs + (1 - cs) * k, m1 = k * (1 - cs) - sq * sn, m2 = k * (1 - cs) + sq * sn;
    float r = c[0], g = c[1], b = c[2];
    c[0] = clampf(r * m0 + g * m1 + b * m2, 0, 1);
    c[1] = clampf(r * m2 + g * m0 + b * m1, 0, 1);
    c[2] = clampf(r * m1 + g * m2 + b * m0, 0, 1);
}

static void fullPass(ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps) {
    ID3D11ShaderResourceView* nulls[4] = {};
    g_ctx->OMSetRenderTargets(1, &rtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->PSSetShader(ps, NULL, 0);
    (void)nulls;
    g_ctx->Draw(4, 0);
}
static void setPost(float tx, float ty, float dx, float dy) {
    PostCB p = {{tx, ty, dx, dy}, {0.32f, 0.45f, 0, 0}};
    g_ctx->UpdateSubresource(g_cbPost, 0, NULL, &p, 0, 0);
}
static void bindSRVs(ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11ShaderResourceView* c, ID3D11ShaderResourceView* d) {
    ID3D11ShaderResourceView* nulls[4] = {};
    g_ctx->PSSetShaderResources(0, 4, nulls);  // never leave a target bound as input
    ID3D11ShaderResourceView* s[4] = {a, b, c, d};
    g_ctx->PSSetShaderResources(0, 4, s);
}

static void render() {
    if (!g_sc) return;

    if (g_w != g_bbW || g_h != g_bbH) {
        g_ctx->OMSetRenderTargets(0, NULL, NULL);
        rel(g_rtv);
        g_sc->ResizeBuffers(0, g_w, g_h, DXGI_FORMAT_UNKNOWN, 0);
        createBackbufferView();
    }

    double tn = now();
    float dt = (float)std::min(tn - g_lastT, 0.1);
    g_lastT = tn;
    double real = tn - g_t0;
    audioUpdate(tn, dt);
    updatePower(tn);
    mediaUpdate(tn);
    updateMorph(tn);
    if (!g_paused) g_simT += dt * S.p[P_SPEED] * (1.0 + 1.2 * A.level);

    // settings panel first: Direct2D shares the device context, and everything below sets its own state
    U.dt = dt;
    ease(g_uiT, g_uiOpen ? 1.f : 0.f, g_uiOpen ? 9.f : 12.f);
    if (g_uiT > 0.002f) drawUi(dt);
    g_mPressed = false;
    g_wheel = 0;

    float offX = 0, offY = 0;
    if (S.drift) {
        offX = (float)(sin(real * 0.021) * 0.07);
        offY = (float)(sin(real * 0.029 + 1.3) * 0.05);
    }
    // supersampling: render the scene bigger; the composite's bilinear fetch averages it back down
    float rs = S.p[P_SCALE] * AA_FACTOR[S.aa];
    int vw = std::min(16384, std::max(1, (int)(g_bbW * rs))), vh = std::min(16384, std::max(1, (int)(g_bbH * rs)));

    OrbCB cb = {};
    cb.res[0] = (float)vw;
    cb.res[1] = (float)vh;
    cb.time = (float)fmod(g_simT, 20000.0);
    cb.size = S.p[P_SIZE] * (1.0f + S.p[P_BREATHE] * 0.035f * (float)sin(g_simT * 0.55)) * (1.0f + 0.07f * A.bass);
    cb.off[0] = offX;
    cb.off[1] = offY;
    cb.detail = S.p[P_DETAIL];
    cb.warp = S.p[P_WARP];
    cb.wobble = S.p[P_WOBBLE];
    cb.grain = S.p[P_GRAIN];
    cb.glow = S.p[P_GLOW];
    cb.accent = S.p[P_ACCENT];
    cb.bright = S.p[P_BRIGHT];
    cb.contrast = S.p[P_CONTRAST];
    cb.stars = S.p[P_STARS];
    cb.morph = g_morph;
    cb.formA = g_formA;
    cb.formB = g_formB;
    cb.bass = A.bass;
    cb.level = A.level;
    cb.particles = S.p[P_PARTICLES];
    cb.ss = (float)vh / (float)g_bbH;
    cb.charge = g_charging ?(float)(0.5 + 0.5 * sin(real * 1.4)) : 0.f;
    float hue = (float)fmod(real * S.p[P_HUE] * 0.25, 6.283185307);
    for (int i = 0; i < 4; i++) {
        cb.c[i][0] = GetRValue(S.c[i]) / 255.f;
        cb.c[i][1] = GetGValue(S.c[i]) / 255.f;
        cb.c[i][2] = GetBValue(S.c[i]) / 255.f;
        cb.c[i][3] = 1.f;
        if (hue != 0) hueRotate(cb.c[i], hue);
    }
    g_ctx->UpdateSubresource(g_cbOrb, 0, NULL, &cb, 0, 0);

    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->RSSetState(g_rast);
    g_ctx->OMSetBlendState(NULL, NULL, 0xffffffff);
    g_ctx->OMSetDepthStencilState(NULL, 0);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    ID3D11Buffer* cbs[3] = {g_cbOrb, g_cbComp, g_cbPost};
    g_ctx->PSSetConstantBuffers(0, 3, cbs);

    // 1) scene at render resolution
    ensureTarget(g_scene, vw, vh);
    bindSRVs(nullptr, nullptr, nullptr, nullptr);
    fullPass(g_scene.rtv, vw, vh, g_psScene);

    // 2) bloom: bright pass at 1/4 resolution, then two separable blur rounds
    float bloom = S.p[P_BLOOM];
    if (bloom > 0.001f) {
        int bw = std::max(1, vw / 4), bh = std::max(1, vh / 4);
        ensureTarget(g_bloomA, bw, bh);
        ensureTarget(g_bloomB, bw, bh);
        setPost(1.f / vw, 1.f / vh, 0, 0);
        bindSRVs(g_scene.srv, nullptr, nullptr, nullptr);
        fullPass(g_bloomA.rtv, bw, bh, g_psBright);
        for (int it = 0; it < 2; it++) {
            float sp = it == 0 ? 1.2f : 2.6f;
            setPost(1.f / bw, 1.f / bh, sp, 0);
            bindSRVs(nullptr, nullptr, g_bloomA.srv, nullptr);
            fullPass(g_bloomB.rtv, bw, bh, g_psBlur);
            setPost(1.f / bw, 1.f / bh, 0, sp);
            bindSRVs(nullptr, nullptr, g_bloomB.srv, nullptr);
            fullPass(g_bloomA.rtv, bw, bh, g_psBlur);
        }
    }

    // 3) composite at full resolution
    CompCB cc = {};
    float H = (float)g_bbH, W = (float)g_bbW, k1 = H / 1440.f;
    cc.misc[0] = W;
    cc.misc[1] = H;
    cc.misc[2] = 0.95f;
    cc.misc[3] = k1;
    cc.audio[0] = A.bass;
    cc.audio[1] = A.level;
    cc.audio[2] = bloom > 0.001f ? bloom : 0.f;
    cc.audio[3] = g_uiT > 0.002f ? std::min(1.f, g_uiT * 1.3f) : 0.f;
    for (int i = 0; i < 4; i++) cc.ui[i] = g_uiRect[i];
    cc.ui2[0] = 26 * k1 * 1.333f;
    cc.ui2[1] = 30 * k1;
    if (S.clockStyle > 0) {
        updateClockTexture();
        float th = (float)g_textTh;
        float hx = (g_textW - 2 * g_textPad) * 0.5f + th * 0.45f;
        float hy = (g_textH - 2 * g_textPad) * 0.5f + th * 0.30f;
        float m = H * 0.07f, cx = W * 0.5f, cy = H * 0.5f;
        switch (S.clockPos) {
        case 1: cy = m + hy; break;
        case 2: cy = H - m - hy; break;
        case 3: cx = m + hx; cy = m + hy; break;
        case 4: cx = W - m - hx; cy = m + hy; break;
        case 5: cx = m + hx; cy = H - m - hy; break;
        case 6: cx = W - m - hx; cy = H - m - hy; break;
        }
        cx += offX * H * 0.6f;
        cy -= offY * H * 0.6f;
        cc.panel[0] = cx; cc.panel[1] = cy; cc.panel[2] = hx; cc.panel[3] = hy;
        cc.glass[0] = std::min(hx, hy) * 0.78f;
        cc.glass[1] = (2.f + 38.f * S.p[P_FROST]) * k1;
        cc.glass[2] = S.p[P_REFR];
        cc.glass[3] = (float)S.clockStyle;
        cc.text[0] = floorf(cx - g_textW * 0.5f);
        cc.text[1] = floorf(cy - g_textH * 0.5f);
        cc.text[2] = (float)g_textW;
        cc.text[3] = (float)g_textH;
    }
    g_ctx->UpdateSubresource(g_cbComp, 0, NULL, &cc, 0, 0);
    bindSRVs(g_scene.srv, S.clockStyle > 0 ? g_textSrv : nullptr, bloom > 0.001f ? g_bloomA.srv : nullptr,
             cc.audio[3] > 0 ? g_ui.srv : nullptr);
    fullPass(g_rtv, g_bbW, g_bbH, g_psComp);

    HRESULT hr = g_sc->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        releaseD3D();
        if (!initD3D(g_main)) {
            MessageBoxW(g_main, L"The graphics device was lost and could not be recreated.", L"Orb", MB_ICONERROR);
            PostQuitMessage(1);
        }
        return;
    }

    g_frames++;
    if (tn - g_fpsT >= 1.0) {
        wchar_t b[160];
        swprintf(b, 160, L"%d fps  ·  %d×%d%ls%ls", g_frames, vw, vh, g_paused ? L"  ·  paused" : L"",
                 S.audio ? (A.client ? L"  ·  listening" : L"  ·  no audio device") : L"");
        g_statText = b;
        g_frames = 0;
        g_fpsT = tn;
    }

    if (g_pendingColor >= 0) {  // modal dialog: open it only after the frame is finished
        int i = g_pendingColor;
        g_pendingColor = -1;
        pickColor(i);
        g_mDown = false;
        g_lastT = now();
    }
}

// ---------------------------------------------------------------- main window
static bool overPanel(float x, float y) {
    return g_uiOpen && x >= g_uiRect[0] && y >= g_uiRect[1] && x < g_uiRect[0] + g_uiRect[2] && y < g_uiRect[1] + g_uiRect[3];
}

static void handleKey(WPARAM k) {
    if (k >= '1' && k < '1' + NUM_FORMS) { setForm((int)(k - '1'), 1.6); return; }
    if (k >= VK_NUMPAD1 && k < VK_NUMPAD1 + NUM_FORMS) { setForm((int)(k - VK_NUMPAD1), 1.6); return; }
    switch (k) {
    case 'H': g_uiOpen = !g_uiOpen; break;
    case 'F': case VK_F11: toggleFullscreen(); break;
    case 'M': S.autoMorph = !S.autoMorph; g_lastAuto = now(); g_dirty = true; break;
    case 'C': S.clockStyle = (S.clockStyle + 1) % 3; g_dirty = true; break;
    case 'A': S.audio = !S.audio; g_dirty = true; break;
    case VK_SPACE: g_paused = !g_paused; break;
    case VK_ESCAPE:
        if (g_uiOpen) g_uiOpen = false;
        else if (g_fs) toggleFullscreen();
        break;
    }
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) {
            g_w = std::max(1, (int)LOWORD(lp));
            g_h = std::max(1, (int)HIWORD(lp));
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ValidateRect(h, NULL);
        return 0;
    case WM_KEYDOWN:
        handleKey(wp);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        float x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        g_mx = x; g_my = y;
        if (overPanel(x, y)) {
            g_mDown = true;
            g_mPressed = true;
            SetCapture(h);
        } else if (g_uiOpen) {
            g_uiOpen = false;                       // click on the orb closes the panel
        } else if (m == WM_LBUTTONDBLCLK) {
            toggleFullscreen();
        }
        return 0;
    }
    case WM_LBUTTONUP:
        g_mDown = false;
        ReleaseCapture();
        return 0;
    case WM_MOUSEWHEEL:
        g_wheel += GET_WHEEL_DELTA_WPARAM(wp);
        return 0;
    case WM_RBUTTONUP:
        g_uiOpen = !g_uiOpen;
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { toggleFullscreen(); return 0; }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
        g_mx = (float)pt.x;
        g_my = (float)pt.y;
        if (pt.x != g_lastMouse.x || pt.y != g_lastMouse.y) {
            g_lastMouse = pt;
            g_lastMove = now();
            if (g_cursorHidden) { g_cursorHidden = false; SetCursor(LoadCursor(NULL, IDC_ARROW)); }
        }
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(g_cursorHidden ? NULL : LoadCursor(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_ENTERSIZEMOVE: SetTimer(h, 1, 15, NULL); return 0;
    case WM_EXITSIZEMOVE: KillTimer(h, 1); g_dirty = true; return 0;
    case WM_TIMER:
        if (wp == 1) render();
        return 0;
    case WM_DPICHANGED:
        if (!g_fs) {
            RECT* r = (RECT*)lp;
            SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_CLOSE:
        saveSettings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void darkTitle(HWND h) {
    BOOL on = TRUE;
    DwmSetWindowAttribute(h, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    g_inst = inst;
    QueryPerformanceFrequency(&g_freq);
    g_t0 = g_lastT = g_fpsT = g_lastMove = now();
    timeBeginPeriod(1);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    bool hadIni = loadSettings();
    g_formA = g_formB = S.form;
    g_lastAuto = now();

    HICON icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    HICON iconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0);
    WNDCLASSEXW wc = {sizeof wc};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = iconSm;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"OrbMain";
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(0, L"OrbMain", L"Orb", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
                             NULL, NULL, inst, NULL);
    if (!g_main || !initD3D(g_main)) {
        MessageBoxW(NULL, L"Could not initialise Direct3D 11. Please update your graphics driver.", L"Orb", MB_ICONERROR);
        return 1;
    }
    darkTitle(g_main);

    bool startFs = false;
    if (hadIni) {
        RECT r;
        r.left = (LONG)iniF(L"window", L"x", 0);
        r.top = (LONG)iniF(L"window", L"y", 0);
        r.right = r.left + (LONG)iniF(L"window", L"w", 0);
        r.bottom = r.top + (LONG)iniF(L"window", L"h", 0);
        if (r.right - r.left > 100 && r.bottom - r.top > 100 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp = {sizeof wp};
            GetWindowPlacement(g_main, &wp);
            wp.rcNormalPosition = r;
            wp.showCmd = SW_HIDE;
            SetWindowPlacement(g_main, &wp);
            startFs = iniF(L"window", L"fullscreen", 0) != 0;
        }
    }
    ShowWindow(g_main, SW_SHOW);
    if (startFs) toggleFullscreen();
    if (!hadIni) { g_uiOpen = true; g_dirty = true; }
    if (wchar_t v[8]; GetEnvironmentVariableW(L"ORB_OPEN_PANEL", v, 8)) g_uiOpen = true;  // used by tools/shot.ps1

    MSG msg;
    double lastDraw = 0, lastSave = now();
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        double t = now();
        if (!g_cursorHidden && t - g_lastMove > 2.5 && !g_uiOpen) {
            g_cursorHidden = true;
            POINT pt;
            GetCursorPos(&pt);
            if (WindowFromPoint(pt) == g_main) SetCursor(NULL);
        }
        if (g_dirty && t - lastSave > 1.0) { saveSettings(); lastSave = t; }

        if (IsIconic(g_main)) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT);
            continue;
        }
        double interval = S.fps > 0 ? 1.0 / S.fps : 0.0;
        double wait = interval - (t - lastDraw);
        if (wait > 0.002) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)((wait - 0.001) * 1000), QS_ALLINPUT);
            continue;
        }
        lastDraw = (interval > 0 && t - lastDraw < interval * 2) ? lastDraw + interval : t;
        render();
    }

    saveSettings();
    audioStop();
    mediaReset();
    urel(M.statics);
    releaseD3D();
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
