// Orb — animated shader orb on a true-black background, with a liquid-glass clock.
// Native Win32 + Direct3D 11, single file, no dependencies beyond Windows itself.
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
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <timeapi.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
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
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
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
  float4 uC0, uC1, uC2, uC3;
};
cbuffer Comp : register(b1) {
  float4 gPanel;   // centre xy, half-size xy (px)
  float4 gGlass;   // corner radius, frost radius (px), refraction, style (0 off, 1 minimal, 2 glass)
  float4 gText;    // text box xy, wh (px)
  float4 gMisc;    // backbuffer w, h, text alpha, px scale (h / 1440)
};
Texture2D sceneTex : register(t0);
Texture2D textTex  : register(t1);
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

  // only march inside a bounding sphere; elsewhere estimate the distance for the halo
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

    float cells = uRes.y * R * 0.55;
    float st = lerp(hash(floor(q * cells)), hash(floor(q * cells * 0.5) + 7.0), 0.35);
    I *= lerp(1.0, 0.2 + 1.6 * st, uGrain);
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
  float halo = uGlow * (1.0 + uBass * 1.5) * (0.22 * exp(-hd * 9.0) + 0.07 * exp(-hd * 2.5)) * (1.0 - cov);
  halo = max(halo - 0.003, 0.0);
  col += lerp(uC1.rgb, uC3.rgb, 0.35 * uAccent) * halo;
  return float4(col, max(cov, saturate(halo * 4.0)));
}

// ------------------------------------------------ 2D forms
float grain2D(float2 fc, float speed) {
  float2 g = floor((fc + float2(uTime * speed, 0.0)) / max(1.0, uRes.y / 700.0));
  return lerp(1.0, 0.35 + 1.3 * hash(float3(g, 3.7)), uGrain * 0.7);
}

float4 aurora(float2 p, float2 fc) {
  float t = uTime, S = uSize;
  float3 col = 0;
  [unroll] for (int k = 0; k < 3; k++) {
    float fk = (float)k;
    float x = p.x / S * (1.0 + 0.25 * fk) + t * 0.04 * (1.0 + 0.5 * fk) + fk * 3.1;
    float y = (-0.20 + 0.11 * fk) * S
            + S * (0.10 * sin(x * 2.3 + t * 0.21) + 0.05 * sin(x * 5.1 - t * 0.33 + fk)
                   + 0.2 * uWarp * (noise(float3(x * 1.7, fk * 5.0, t * 0.12)) - 0.5));
    float dy = (p.y - y) / S;
    float rays = 0.45 + 0.55 * noise(float3(x * uDetail * 2.2, dy * 1.2 - t * 0.5, fk * 11.0));
    float body = exp(-max(dy, 0.0) * (4.6 + fk * 1.5)) * smoothstep(-0.03, 0.0, dy)
               * smoothstep(0.75, 0.15, dy);                         // let the sky above go back to black
    float I = body * rays * (1.0 - 0.18 * fk) * (1.0 + uBass * 0.8);
    I = lerp(I * 0.6 + 0.12 * body, I, uContrast);
    I *= grain2D(fc, 14.0) * uBright * (1.0 + 0.35 * uLevel);
    float3 c = shade(I);
    c = lerp(c, uC3.rgb * I, saturate(dy * 1.6) * uAccent);
    col += c * (1.0 - 0.25 * fk);
  }
  float lum = max(col.r, max(col.g, col.b));
  return float4(col, saturate(lum * 3.0));
}

float4 nebula(float2 p, float2 fc) {
  float t = uTime, R = 0.5 * uSize;
  float2 u = p / R;
  float3 q = float3(u * 0.9, t * 0.05);
  float2 w = float2(fbm(q + float3(0.0, 0.0, t * 0.03)), fbm(q + float3(5.2, 1.3, -t * 0.02)));
  float f = fbm(q + uWarp * 2.2 * float3(w, 0.0) + float3(0.0, 0.0, t * 0.04));
  float ridge = pow(max(1.0 - abs(sin(f * uDetail * 1.4 - t * 0.6)), 0.0), 3.0);
  float I = f * (0.7 + 0.8 * ridge);
  I = lerp(0.55 * f + 0.2, I, uContrast);
  float mask = exp(-dot(u, u) * 0.85) * (1.0 + uBass * 0.6);
  I = max(I * mask * 1.7 - 0.06, 0.0);
  I *= grain2D(fc, 6.0) * uBright * (1.0 + 0.35 * uLevel);
  float3 c = shade(I);
  float aw = smoothstep(0.0, 1.0, dot(normalize(u + 1e-5), float2(0.8, -0.6))) * smoothstep(0.3, 1.4, length(u));
  c = lerp(c, uC3.rgb * (0.2 + I), saturate(aw * uAccent));
  float halo = uGlow * 0.10 * mask;
  c += lerp(uC1.rgb, uC3.rgb, 0.4) * halo * (1.0 - saturate(I));
  float lum = max(c.r, max(c.g, c.b));
  return float4(c, saturate(lum * 3.0));
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

  col = 1.0 - exp(-col * 1.6);
  float lum = max(col.r, max(col.g, col.b));
  col += (hash(float3(fc, frac(uTime) * 97.0)) - 0.5) / 255.0 * step(1.5 / 255.0, lum);
  return float4(col, 1.0);
}

// ------------------------------------------------ composite: upscale + liquid-glass clock
float sdRBox(float2 p, float2 b, float r) {
  float2 q = abs(p) - b + r;
  return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float3 scene(float2 px) { return sceneTex.SampleLevel(smp, px / gMisc.xy, 0).rgb; }
float textA(float2 px) {
  float2 uv = (px - gText.xy) / gText.zw;
  if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) return 0.0;
  return textTex.SampleLevel(smp, uv, 0).a;
}

float4 psComp(VO i) : SV_Target {
  float2 px = i.pos.xy;
  float3 col = scene(px);
  int style = (int)gGlass.w;
  float k1 = gMisc.w;

  if (style == 2) {
    float2 c = gPanel.xy, hs = gPanel.zw;
    float rad = gGlass.x;
    float d = sdRBox(px - c, hs, rad);

    // soft drop shadow
    float dS = sdRBox(px - c - float2(0.0, 0.10 * hs.y), hs, rad);
    col *= 1.0 - 0.5 * exp(-max(dS, 0.0) / (0.22 * hs.y)) * smoothstep(-1.0, 1.0, d);

    if (d < 1.0) {
      float2 g = float2(sdRBox(px + float2(1, 0) - c, hs, rad) - sdRBox(px - float2(1, 0) - c, hs, rad),
                        sdRBox(px + float2(0, 1) - c, hs, rad) - sdRBox(px - float2(0, 1) - c, hs, rad));
      g = dot(g, g) > 1e-8 ? normalize(g) : float2(0.0, 0.0);
      float bw = min(hs.x, hs.y) * 0.6;              // bevel width
      float bev = pow(1.0 - saturate(-d / bw), 2.5);  // 1 at the rim -> 0 in the flat middle
      float2 refr = -g * bev * bw * gGlass.z;          // lens: background is pulled in at the rim
      float2 ca = g * bev * bw * 0.07 * gGlass.z;     // chromatic split along the rim

      float3 acc = 0;
      float br = gGlass.y;
      [loop] for (int k = 0; k < 32; k++) {
        float fk = (float)k + 0.5;
        float a = fk * 2.39996;
        float2 o = px + refr + float2(cos(a), sin(a)) * sqrt(fk / 32.0) * br;
        acc += float3(scene(o + ca).r, scene(o).g, scene(o - ca).b);
      }
      float3 glass = acc / 32.0;
      float lum = dot(glass, float3(0.299, 0.587, 0.114));
      glass = max(lerp(lum.xxx, glass, 1.3), 0.0);    // a bit more saturated, like real glass
      glass = glass * 1.1 + 0.03;                       // faint milky body so it reads on pure black

      float vy = (px.y - (c.y - hs.y)) / (2.0 * hs.y);
      glass += 0.07 * pow(saturate(1.0 - vy), 3.0);     // sheen from the top

      float rimLine = exp(-abs(d + 1.3 * k1) / (1.1 * k1));
      float keyL = saturate(dot(g, normalize(float2(-0.55, -0.85))));
      float backL = saturate(dot(g, normalize(float2(0.55, 0.85))));
      glass += rimLine * (0.16 + 0.6 * keyL * keyL + 0.28 * backL * backL);
      glass += 0.05 * bev;                               // glass thickness glow

      col = lerp(col, glass, saturate(0.5 - d));
    }
  }

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
    float c[4][4];
};
struct CompCB { float panel[4], glass[4], text[4], misc[4]; };

// ---------------------------------------------------------------- settings
enum {
    P_SIZE, P_SPEED, P_DETAIL, P_WARP, P_WOBBLE, P_BREATHE,
    P_GRAIN, P_GLOW, P_ACCENT, P_BRIGHT, P_CONTRAST, P_STARS,
    P_HUE, P_MORPHSEC, P_SCALE,
    P_CLOCKSIZE, P_FROST, P_REFR, P_SENS,
    NP
};
enum { FMT_F, FMT_PCT, FMT_SEC };
struct ParamDef { const wchar_t* key; const wchar_t* label; float mn, mx, def; int fmt; };
static const ParamDef PD[NP] = {
    {L"size",       L"Size",           0.2f, 1.4f, 0.82f, FMT_F},
    {L"speed",      L"Speed",          0.0f, 3.0f, 1.00f, FMT_F},
    {L"detail",     L"Pattern detail", 2.0f, 20.f, 7.00f, FMT_F},
    {L"warp",       L"Turbulence",     0.0f, 1.2f, 0.40f, FMT_F},
    {L"wobble",     L"Edge wobble",    0.0f, 2.0f, 0.50f, FMT_F},
    {L"breathe",    L"Breathing",      0.0f, 1.5f, 0.30f, FMT_F},
    {L"grain",      L"Particle grain", 0.0f, 1.0f, 0.45f, FMT_F},
    {L"glow",       L"Outer glow",     0.0f, 2.0f, 0.60f, FMT_F},
    {L"accent",     L"Accent tint",    0.0f, 1.0f, 0.55f, FMT_F},
    {L"brightness", L"Brightness",     0.3f, 2.0f, 1.00f, FMT_F},
    {L"contrast",   L"Contrast",       0.0f, 1.5f, 0.70f, FMT_F},
    {L"stars",      L"Stars",          0.0f, 1.0f, 0.35f, FMT_F},
    {L"hue",        L"Color cycle",    0.0f, 1.0f, 0.00f, FMT_F},
    {L"morphsec",   L"Morph every",    10.f, 180.f, 45.f, FMT_SEC},
    {L"scale",      L"Render res.",    0.35f, 1.0f, 1.00f, FMT_PCT},
    {L"clocksize",  L"Clock size",     0.5f, 2.5f, 1.35f, FMT_F},
    {L"frost",      L"Frost",          0.0f, 1.0f, 0.55f, FMT_F},
    {L"refraction", L"Refraction",     0.0f, 1.5f, 0.80f, FMT_F},
    {L"sensitivity",L"Sensitivity",    0.0f, 2.0f, 1.00f, FMT_F},
};

struct Preset { const wchar_t* name; COLORREF c[4]; };
static const Preset PRESETS[6] = {
    {L"Hub Blue", {RGB(0x01,0x08,0x1a), RGB(0x1b,0x7b,0xd4), RGB(0xae,0xe8,0xff), RGB(0x4b,0x3c,0xff)}},
    {L"Aurora",   {RGB(0x00,0x15,0x0f), RGB(0x13,0xb9,0x8b), RGB(0xca,0xff,0xea), RGB(0x7a,0x3c,0xff)}},
    {L"Ember",    {RGB(0x16,0x04,0x00), RGB(0xe0,0x53,0x1c), RGB(0xff,0xd5,0x9e), RGB(0xff,0x2a,0x6a)}},
    {L"Violet",   {RGB(0x0b,0x02,0x20), RGB(0x7b,0x3c,0xff), RGB(0xe2,0xcc,0xff), RGB(0xff,0x3c,0xa8)}},
    {L"Toxic",    {RGB(0x03,0x12,0x00), RGB(0x74,0xd1,0x1a), RGB(0xf1,0xff,0xb5), RGB(0x00,0xc8,0xff)}},
    {L"Mono",     {RGB(0x04,0x04,0x04), RGB(0x7d,0x85,0x90), RGB(0xff,0xff,0xff), RGB(0x5a,0x78,0xff)}},
};
static const wchar_t* COLOR_NAMES[4] = {L"Deep", L"Mid", L"Light", L"Accent"};
static const wchar_t* FORM_NAMES[7] = {L"Orb", L"Blob", L"Ring", L"Cube", L"Crystal", L"Aurora", L"Nebula"};
static const wchar_t* CLOCK_STYLES[3] = {L"Off", L"Minimal", L"Glass"};
static const wchar_t* CLOCK_POS[7] = {L"Center", L"Top", L"Bottom", L"Top left", L"Top right", L"Bottom left", L"Bottom right"};
static const int FPS_OPTS[4] = {24, 30, 60, 0};
enum { NUM_FORMS = 7, NUM_3D = 5 };

struct Settings {
    float p[NP];
    COLORREF c[4];
    bool drift, autoMorph, h24, seconds, showDate, audio;
    int fps, form, clockStyle, clockPos;
};
static Settings S;

static void defaults() {
    for (int i = 0; i < NP; i++) S.p[i] = PD[i].def;
    for (int i = 0; i < 4; i++) S.c[i] = PRESETS[0].c[i];
    S.drift = true; S.autoMorph = false; S.h24 = true; S.seconds = false; S.showDate = true; S.audio = false;
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
    S.fps = (int)iniF(L"orb", L"fps", 60);
    S.form = clampi((int)iniF(L"orb", L"form", 0), 0, NUM_FORMS - 1);
    S.clockStyle = clampi((int)iniF(L"orb", L"clockstyle", 2), 0, 2);
    S.clockPos = clampi((int)iniF(L"orb", L"clockpos", 0), 0, 6);
    return true;
}

// ---------------------------------------------------------------- globals
static HINSTANCE g_inst;
static HWND g_main, g_set, g_stat;
static HWND g_track[NP], g_val[NP];
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
    // auto-gain: follow the recent peak, so quiet and loud content both move it
    float pd = expf(-dt * 0.15f);
    A.peak = std::max(std::max(A.peak * pd, A.env), 0.01f);
    A.bassPeak = std::max(std::max(A.bassPeak * pd, A.bassEnv), 0.01f);
    float sens = S.p[P_SENS];
    float lv = powf(clampf(A.env / A.peak, 0, 1), 1.5f) * sens;
    float bs = powf(clampf(A.bassEnv / A.bassPeak, 0, 1), 2.0f) * sens;
    A.level += (lv - A.level) * (1.f - expf(-dt * 20.f));
    A.bass += (bs - A.bass) * (1.f - expf(-dt * 25.f));
}

// ---------------------------------------------------------------- Direct3D 11
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain1* g_sc;
static ID3D11RenderTargetView* g_rtv;
static int g_bbW = 0, g_bbH = 0;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader *g_psScene, *g_psComp;
static ID3D11Buffer *g_cbOrb, *g_cbComp;
static ID3D11SamplerState* g_samp;
static ID3D11RasterizerState* g_rast;
static ID3D11Texture2D* g_sceneTex;
static ID3D11RenderTargetView* g_sceneRtv;
static ID3D11ShaderResourceView* g_sceneSrv;
static int g_sceneW = 0, g_sceneH = 0;
static ID3D11ShaderResourceView* g_textSrv;
static std::wstring g_textKey;
static int g_textW = 0, g_textH = 0, g_textPad = 0, g_textTh = 0;

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

static void releaseD3D() {
    if (g_ctx) g_ctx->ClearState();
    rel(g_textSrv); rel(g_sceneSrv); rel(g_sceneRtv); rel(g_sceneTex);
    rel(g_rast); rel(g_samp); rel(g_cbComp); rel(g_cbOrb);
    rel(g_psComp); rel(g_psScene); rel(g_vs); rel(g_rtv); rel(g_sc); rel(g_ctx); rel(g_dev);
    g_sceneW = g_sceneH = 0;
    g_textKey.clear();
}

static bool initD3D(HWND hwnd) {
    // Default adapter = whatever Windows picks (Settings > Display > Graphics lets you choose per app).
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_SINGLETHREADED, &fl, 1,
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
    rel(f2); rel(ad); rel(dxdev);
    if (FAILED(hr)) return false;
    createBackbufferView();

    // Compiled shaders are cached next to the settings (keyed by a hash of the source), so only the
    // first launch after an update pays the ~3 s compile.
    const char* entries[3] = {"vs", "psScene", "psComp"};
    const char* targets[3] = {"vs_5_0", "ps_5_0", "ps_5_0"};
    std::string blobs[3];
    unsigned long long hsh = 1469598103934665603ull;
    for (const char* c = HLSL; *c; c++) hsh = (hsh ^ (unsigned char)*c) * 1099511628211ull;
    std::wstring cachePath = g_dataDir + L"\\shaders.bin";
    bool cached = false;
    if (FILE* f = _wfopen(cachePath.c_str(), L"rb")) {
        unsigned long long fh = 0;
        cached = fread(&fh, 8, 1, f) == 1 && fh == hsh;
        for (int i = 0; cached && i < 3; i++) {
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
        for (int i = 0; i < 3; i++) {
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
    if (!g_vs || !g_psScene || !g_psComp) { DeleteFileW(cachePath.c_str()); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof(OrbCB);
    g_dev->CreateBuffer(&bd, NULL, &g_cbOrb);
    bd.ByteWidth = sizeof(CompCB);
    g_dev->CreateBuffer(&bd, NULL, &g_cbComp);

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

static void ensureSceneTarget(int w, int h) {
    if (g_sceneTex && g_sceneW == w && g_sceneH == h) return;
    rel(g_sceneSrv); rel(g_sceneRtv); rel(g_sceneTex);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_dev->CreateTexture2D(&td, NULL, &g_sceneTex);
    g_dev->CreateRenderTargetView(g_sceneTex, NULL, &g_sceneRtv);
    g_dev->CreateShaderResourceView(g_sceneTex, NULL, &g_sceneSrv);
    g_sceneW = w;
    g_sceneH = h;
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

// Renders "12:34 ss" + date into a texture. Each element goes into its own colour channel so it can get its
// own opacity: big digits = red (100%), seconds / AM-PM = green (60%), date = blue (72%).
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
    int th = (int)clampf(g_bbH * 0.085f * S.p[P_CLOCKSIZE], 24.f, 420.f);
    wchar_t ks[32];
    swprintf(ks, 32, L"|%d", th);
    std::wstring key = std::wstring(hm) + L"|" + small + L"|" + date + ks;
    if (key == g_textKey && g_textSrv) return;
    g_textKey = key;

    int sh = std::max(8, th * 36 / 100), dh = std::max(9, th * 17 / 100);
    HFONT fBig = makeFont(th, FW_LIGHT, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fSmall = makeFont(sh, FW_NORMAL, L"Segoe UI Variable Display", L"Segoe UI");
    HFONT fDate = makeFont(dh, FW_SEMIBOLD, L"Segoe UI Variable Text", L"Segoe UI");
    HDC mdc = CreateCompatibleDC(NULL);
    HGDIOBJ oldFont = SelectObject(mdc, fBig);

    int bigExtra = th / 40, dateExtra = dh * 28 / 100, gapSmall = th / 9;
    TEXTMETRICW tmB, tmD;
    SIZE sBig, sSmall = {0, 0}, sDate = {0, 0};
    SetTextCharacterExtra(mdc, bigExtra);
    GetTextMetricsW(mdc, &tmB);
    GetTextExtentPoint32W(mdc, hm, (int)wcslen(hm), &sBig);
    SelectObject(mdc, fSmall);
    SetTextCharacterExtra(mdc, 0);
    if (*small) GetTextExtentPoint32W(mdc, small, (int)wcslen(small), &sSmall);
    SelectObject(mdc, fDate);
    SetTextCharacterExtra(mdc, dateExtra);
    GetTextMetricsW(mdc, &tmD);
    if (*date) GetTextExtentPoint32W(mdc, date, (int)wcslen(date), &sDate);

    int pad = std::max(4, th / 8);
    int capB = tmB.tmAscent - tmB.tmInternalLeading;
    int line1W = sBig.cx + (*small ? gapSmall + sSmall.cx : 0);
    int line1H = capB + tmB.tmDescent / 4;
    int gap = th * 22 / 100;
    int capD = tmD.tmAscent - tmD.tmInternalLeading;
    int line2H = *date ? capD + tmD.tmDescent : 0;
    int W = std::max(line1W, (int)sDate.cx) + pad * 2;
    int H = line1H + (*date ? gap + line2H : 0) + pad * 2;

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

    int x0 = (W - line1W) / 2, base1 = pad + capB;
    SelectObject(mdc, fBig);
    SetTextCharacterExtra(mdc, bigExtra);
    SetTextColor(mdc, RGB(255, 0, 0));
    TextOutW(mdc, x0, base1, hm, (int)wcslen(hm));
    if (*small) {
        SelectObject(mdc, fSmall);
        SetTextCharacterExtra(mdc, 0);
        SetTextColor(mdc, RGB(0, 255, 0));
        TextOutW(mdc, x0 + sBig.cx + gapSmall, base1, small, (int)wcslen(small));
    }
    if (*date) {
        SelectObject(mdc, fDate);
        SetTextCharacterExtra(mdc, dateExtra);
        SetTextColor(mdc, RGB(0, 0, 255));
        TextOutW(mdc, (W - sDate.cx) / 2 + dateExtra / 2, pad + line1H + gap + capD, date, (int)wcslen(date));
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
    DeleteObject(fSmall);
    DeleteObject(fDate);
    DeleteDC(mdc);
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
    updateMorph(tn);
    if (!g_paused) g_simT += dt * S.p[P_SPEED] * (1.0 + 1.2 * A.level);

    float offX = 0, offY = 0;
    if (S.drift) {
        offX = (float)(sin(real * 0.021) * 0.07);
        offY = (float)(sin(real * 0.029 + 1.3) * 0.05);
    }
    int vw = std::max(1, (int)(g_bbW * S.p[P_SCALE])), vh = std::max(1, (int)(g_bbH * S.p[P_SCALE]));

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
    float hue = (float)fmod(real * S.p[P_HUE] * 0.25, 6.283185307);
    for (int i = 0; i < 4; i++) {
        cb.c[i][0] = GetRValue(S.c[i]) / 255.f;
        cb.c[i][1] = GetGValue(S.c[i]) / 255.f;
        cb.c[i][2] = GetBValue(S.c[i]) / 255.f;
        cb.c[i][3] = 1.f;
        if (hue != 0) hueRotate(cb.c[i], hue);
    }
    g_ctx->UpdateSubresource(g_cbOrb, 0, NULL, &cb, 0, 0);

    ID3D11ShaderResourceView* nulls[2] = {nullptr, nullptr};
    g_ctx->PSSetShaderResources(0, 2, nulls);
    g_ctx->IASetInputLayout(NULL);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    g_ctx->RSSetState(g_rast);
    g_ctx->VSSetShader(g_vs, NULL, 0);
    g_ctx->PSSetSamplers(0, 1, &g_samp);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cbOrb);
    g_ctx->PSSetConstantBuffers(1, 1, &g_cbComp);

    // 1) scene at render resolution
    ensureSceneTarget(vw, vh);
    g_ctx->OMSetRenderTargets(1, &g_sceneRtv, NULL);
    D3D11_VIEWPORT vp = {0, 0, (float)vw, (float)vh, 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->PSSetShader(g_psScene, NULL, 0);
    g_ctx->Draw(4, 0);

    // 2) composite at full resolution: upscale + clock
    CompCB cc = {};
    float H = (float)g_bbH, W = (float)g_bbW, k1 = H / 1440.f;
    cc.misc[0] = W;
    cc.misc[1] = H;
    cc.misc[2] = 0.95f;
    cc.misc[3] = k1;
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
    g_ctx->OMSetRenderTargets(1, &g_rtv, NULL);
    vp.Width = W;
    vp.Height = H;
    g_ctx->RSSetViewports(1, &vp);
    ID3D11ShaderResourceView* srvs[2] = {g_sceneSrv, S.clockStyle > 0 ? g_textSrv : nullptr};
    g_ctx->PSSetShaderResources(0, 2, srvs);
    g_ctx->PSSetShader(g_psComp, NULL, 0);
    g_ctx->Draw(4, 0);

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
        swprintf(b, 160, L"%d fps  ·  %d×%d px%ls\n%ls", g_frames, vw, vh, g_paused ? L"  ·  paused" : L"",
                 S.audio ? (A.client ? L"Sound: listening" : L"Sound: no audio device") : L"");
        if (g_stat) SetWindowTextW(g_stat, b);
        g_frames = 0;
        g_fpsT = tn;
    }
}

// ---------------------------------------------------------------- fullscreen
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

// ---------------------------------------------------------------- save
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

// ---------------------------------------------------------------- settings window
enum {
    ID_PRESET0 = 100, ID_COLOR0 = 110,
    ID_DRIFT = 120, ID_FPS, ID_FULL, ID_RESET,
    ID_FORM0 = 130, ID_AUTOMORPH = 140,
    ID_CSTYLE0 = 150, ID_CPOS = 155, ID_H24, ID_SECS, ID_DATE,
    ID_AUDIO = 160,
    ID_TRACK0 = 200, ID_VAL0 = 300, ID_LABEL0 = 400, ID_HDR = 900, ID_STAT = 950, ID_HINT
};
static const COLORREF UI_BG = RGB(12, 16, 24), UI_FG = RGB(223, 232, 245), UI_DIM = RGB(125, 138, 160),
                      UI_LINE = RGB(44, 52, 68), UI_BTN = RGB(22, 28, 40), UI_ACC = RGB(74, 168, 255),
                      UI_SEL = RGB(20, 44, 74);
static HBRUSH g_brBg;
static HFONT g_fUI, g_fHdr, g_fSmall;
static int g_setDpi = 96;
static const int SET_W = 900, SET_H = 512;

static int D(int v) { return MulDiv(v, g_setDpi, 96); }

static bool* toggleOf(int id) {
    switch (id) {
    case ID_DRIFT: return &S.drift;
    case ID_AUTOMORPH: return &S.autoMorph;
    case ID_H24: return &S.h24;
    case ID_SECS: return &S.seconds;
    case ID_DATE: return &S.showDate;
    case ID_AUDIO: return &S.audio;
    }
    return nullptr;
}

static void makeFonts() {
    if (g_fUI) DeleteObject(g_fUI);
    if (g_fHdr) DeleteObject(g_fHdr);
    if (g_fSmall) DeleteObject(g_fSmall);
    auto mk = [](int pt10, int weight) {
        return CreateFontW(-MulDiv(pt10, g_setDpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    g_fUI = mk(90, FW_NORMAL);
    g_fHdr = mk(75, FW_SEMIBOLD);
    g_fSmall = mk(80, FW_NORMAL);
}

static HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, D(x), D(y), D(w), D(h), g_set,
                             (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
}
static HWND label(const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
    return child(L"STATIC", text, style | SS_NOPREFIX, x, y, w, h, id, f);
}
static HWND button(const wchar_t* text, int x, int y, int w, int h, int id, HFONT f) {
    return child(L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id, f);
}

static void setValText(int i) {
    wchar_t b[32];
    if (PD[i].fmt == FMT_PCT) swprintf(b, 32, L"%d%%", (int)lroundf(S.p[i] * 100));
    else if (PD[i].fmt == FMT_SEC) swprintf(b, 32, L"%d s", (int)lroundf(S.p[i]));
    else swprintf(b, 32, L"%.2f", S.p[i]);
    SetWindowTextW(g_val[i], b);
}

static void syncControls() {
    if (!g_set) return;
    for (int i = 0; i < NP; i++) {
        int pos = (int)lroundf((S.p[i] - PD[i].mn) / (PD[i].mx - PD[i].mn) * 1000.f);
        SendMessageW(g_track[i], TBM_SETPOS, TRUE, pos);
        setValText(i);
    }
    for (HWND c = GetWindow(g_set, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT))
        if (GetDlgCtrlID(c) < ID_TRACK0) InvalidateRect(c, NULL, FALSE);
}

static void sliderRow(int i, int x, int y, int w) {
    label(PD[i].label, SS_LEFT, x, y + 4, 104, 20, ID_LABEL0 + i, g_fUI);
    g_track[i] = child(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + 100, y, w - 100 - 46, 26,
                       ID_TRACK0 + i, g_fUI);
    SendMessageW(g_track[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000));
    SendMessageW(g_track[i], TBM_SETLINESIZE, 0, 10);
    SendMessageW(g_track[i], TBM_SETPAGESIZE, 0, 50);
    g_val[i] = label(L"", SS_RIGHT, x + w - 46, y + 4, 46, 20, ID_VAL0 + i, g_fSmall);
}

static void buildSettings() {
    for (HWND c; (c = GetWindow(g_set, GW_CHILD));) DestroyWindow(c);
    makeFonts();

    // column 1: palette, form, screen
    const int x = 16, W = 262;
    label(L"PALETTE", 0, x, 12, W, 16, ID_HDR, g_fHdr);
    for (int i = 0; i < 6; i++) button(PRESETS[i].name, x + (i % 3) * 88, 32 + (i / 3) * 50, 82, 44, ID_PRESET0 + i, g_fSmall);
    for (int i = 0; i < 4; i++) button(COLOR_NAMES[i], x + i * 66, 134, 60, 40, ID_COLOR0 + i, g_fSmall);
    sliderRow(P_HUE, x, 182, W);
    label(L"FORM", 0, x, 222, W, 16, ID_HDR, g_fHdr);
    for (int i = 0; i < 4; i++) button(FORM_NAMES[i], x + i * 67, 242, 61, 32, ID_FORM0 + i, g_fUI);
    for (int i = 4; i < 7; i++) button(FORM_NAMES[i], x + (i - 4) * 89, 280, 84, 32, ID_FORM0 + i, g_fUI);
    button(L"Auto-morph between 3D forms", x, 320, W, 24, ID_AUTOMORPH, g_fUI);
    sliderRow(P_MORPHSEC, x, 348, W);
    label(L"SCREEN & PERFORMANCE", 0, x, 388, W, 16, ID_HDR, g_fHdr);
    button(L"Anti burn-in drift", x, 408, W, 24, ID_DRIFT, g_fUI);
    button(L"", x, 436, W, 28, ID_FPS, g_fUI);
    sliderRow(P_SCALE, x, 472, W);

    // column 2: shape + look
    const int x2 = 310, W2 = 280;
    label(L"SHAPE & MOTION", 0, x2, 12, W2, 16, ID_HDR, g_fHdr);
    for (int i = P_SIZE; i <= P_BREATHE; i++) sliderRow(i, x2, 32 + (i - P_SIZE) * 30, W2);
    label(L"LOOK", 0, x2, 222, W2, 16, ID_HDR, g_fHdr);
    for (int i = P_GRAIN; i <= P_STARS; i++) sliderRow(i, x2, 242 + (i - P_GRAIN) * 30, W2);
    button(L"Fullscreen", x2, 432, 137, 30, ID_FULL, g_fUI);
    button(L"Reset all", x2 + 143, 432, 137, 30, ID_RESET, g_fUI);
    g_stat = label(L"", 0, x2, 470, W2, 36, ID_STAT, g_fSmall);

    // column 3: clock + audio
    const int x3 = 610, W3 = 274;
    label(L"CLOCK", 0, x3, 12, W3, 16, ID_HDR, g_fHdr);
    for (int i = 0; i < 3; i++) button(CLOCK_STYLES[i], x3 + i * 93, 32, 88, 28, ID_CSTYLE0 + i, g_fUI);
    button(L"", x3, 66, W3, 28, ID_CPOS, g_fUI);
    button(L"24-hour time", x3, 100, W3, 24, ID_H24, g_fUI);
    button(L"Show seconds", x3, 126, W3, 24, ID_SECS, g_fUI);
    button(L"Show date", x3, 152, W3, 24, ID_DATE, g_fUI);
    sliderRow(P_CLOCKSIZE, x3, 180, W3);
    sliderRow(P_FROST, x3, 210, W3);
    sliderRow(P_REFR, x3, 240, W3);
    label(L"SOUND", 0, x3, 282, W3, 16, ID_HDR, g_fHdr);
    button(L"React to what's playing", x3, 302, W3, 24, ID_AUDIO, g_fUI);
    sliderRow(P_SENS, x3, 330, W3);
    label(L"KEYS\nH  settings     F / F11  fullscreen\n1–7  forms     M  auto-morph\n"
          L"C  clock style     A  sound     Space  pause\nRight-click the orb to open this panel",
          0, x3, 380, W3, 110, ID_HINT, g_fSmall);

    syncControls();
}

static void sizeSettingsWindow(const RECT* at) {
    RECT r = {0, 0, D(SET_W), D(SET_H)};
    DWORD st = (DWORD)GetWindowLongPtrW(g_set, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(g_set, GWL_EXSTYLE);
    AdjustWindowRectExForDpi(&r, st, FALSE, ex, g_setDpi);
    if (at) SetWindowPos(g_set, NULL, at->left, at->top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    else SetWindowPos(g_set, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void showSettings(bool show) {
    if (!show) {
        ShowWindow(g_set, SW_HIDE);
        SetForegroundWindow(g_main);
        return;
    }
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(g_set, NULL, mi.rcWork.left + 40, mi.rcWork.top + 40, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT r;
    GetWindowRect(g_set, &r);
    SetWindowPos(g_set, NULL, mi.rcWork.right - (r.right - r.left) - 24, mi.rcWork.top + 24, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_set, SW_SHOW);
    SetForegroundWindow(g_set);
}

static void fillRound(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int rad) {
    HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
}

static COLORREF lerpC(COLORREF a, COLORREF b, float t) {
    return RGB(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t, GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t,
               GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
}

static void drawItem(DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    int id = (int)di->CtlID;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool focus = (di->itemState & ODS_FOCUS) != 0 && !(di->itemState & ODS_NOFOCUSRECT);
    wchar_t text[128];
    GetWindowTextW(di->hwndItem, text, 128);
    HBRUSH bg = CreateSolidBrush(UI_BG);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, (HFONT)SendMessageW(di->hwndItem, WM_GETFONT, 0, 0));
    int rad = D(10);
    const UINT DT = DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER;

    if (id >= ID_PRESET0 && id < ID_PRESET0 + 6) {
        const Preset& p = PRESETS[id - ID_PRESET0];
        bool on = true;
        for (int i = 0; i < 4; i++) on = on && p.c[i] == S.c[i];
        fillRound(dc, r, pressed ? UI_LINE : UI_BTN, on ? UI_ACC : (focus ? UI_DIM : UI_LINE), rad);
        RECT g = {r.left + D(7), r.top + D(7), r.right - D(7), r.top + D(21)};
        int w = g.right - g.left;
        for (int x = 0; x < w; x++) {
            float t = (float)x / (w - 1) * 3.f;
            int seg = std::min(2, (int)t);
            RECT l = {g.left + x, g.top, g.left + x + 1, g.bottom};
            HBRUSH b = CreateSolidBrush(lerpC(p.c[seg], p.c[seg + 1], t - seg));
            FillRect(dc, &l, b);
            DeleteObject(b);
        }
        RECT tr = {r.left, g.bottom + D(2), r.right, r.bottom - D(2)};
        SetTextColor(dc, UI_FG);
        DrawTextW(dc, text, -1, &tr, DT | DT_CENTER);
    } else if (id >= ID_COLOR0 && id < ID_COLOR0 + 4) {
        COLORREF c = S.c[id - ID_COLOR0];
        fillRound(dc, r, c, pressed || focus ? UI_ACC : UI_LINE, rad);
        int lum = (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000;
        SetTextColor(dc, lum > 140 ? RGB(10, 14, 20) : UI_FG);
        DrawTextW(dc, text, -1, &r, DT | DT_CENTER);
    } else if (bool* tg = toggleOf(id)) {
        bool on = *tg;
        int bs = D(16);
        RECT b = {r.left + D(1), r.top + (r.bottom - r.top - bs) / 2, r.left + D(1) + bs, r.top + (r.bottom - r.top + bs) / 2};
        fillRound(dc, b, on ? UI_ACC : UI_BTN, on ? UI_ACC : (focus ? UI_DIM : UI_LINE), D(5));
        if (on) {
            HPEN pen = CreatePen(PS_SOLID, std::max(2, D(2)), RGB(8, 20, 40));
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, b.left + bs * 25 / 100, b.top + bs * 52 / 100, NULL);
            LineTo(dc, b.left + bs * 43 / 100, b.top + bs * 70 / 100);
            LineTo(dc, b.left + bs * 76 / 100, b.top + bs * 32 / 100);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        RECT tr = {b.right + D(10), r.top, r.right, r.bottom};
        SetTextColor(dc, UI_FG);
        DrawTextW(dc, text, -1, &tr, DT | DT_LEFT);
    } else {
        bool sel = (id >= ID_FORM0 && id < ID_FORM0 + NUM_FORMS && S.form == id - ID_FORM0) ||
                   (id >= ID_CSTYLE0 && id < ID_CSTYLE0 + 3 && S.clockStyle == id - ID_CSTYLE0);
        fillRound(dc, r, pressed ? UI_LINE : (sel ? UI_SEL : UI_BTN), sel ? UI_ACC : (focus ? UI_DIM : UI_LINE), rad);
        if (id == ID_FPS) {
            if (S.fps > 0) swprintf(text, 128, L"FPS cap:  %d", S.fps);
            else swprintf(text, 128, L"FPS cap:  Max / VSync");
        } else if (id == ID_CPOS) {
            swprintf(text, 128, L"Position:  %ls", CLOCK_POS[S.clockPos]);
        } else if (id == ID_FULL) {
            wcscpy(text, g_fs ? L"Exit fullscreen" : L"Fullscreen");
        }
        SetTextColor(dc, sel ? RGB(255, 255, 255) : UI_FG);
        DrawTextW(dc, text, -1, &r, DT | DT_CENTER);
    }
}

static void pickColor(int i) {
    static COLORREF custom[16];
    CHOOSECOLORW cc = {sizeof cc};
    cc.hwndOwner = g_set;
    cc.rgbResult = S.c[i];
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;
    if (ChooseColorW(&cc)) {
        S.c[i] = cc.rgbResult;
        g_dirty = true;
        syncControls();
    }
}

static LRESULT CALLBACK SetProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        SetTextColor(dc, (id >= ID_HDR || (id >= ID_VAL0 && id < ID_LABEL0)) ? UI_DIM : UI_FG);
        SetBkColor(dc, UI_BG);
        return (LRESULT)g_brBg;
    }
    case WM_DRAWITEM:
        drawItem((DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_HSCROLL:
        for (int i = 0; i < NP; i++)
            if ((HWND)lp == g_track[i]) {
                int pos = (int)SendMessageW(g_track[i], TBM_GETPOS, 0, 0);
                S.p[i] = PD[i].mn + (PD[i].mx - PD[i].mn) * pos / 1000.f;
                setValText(i);
                g_dirty = true;
            }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code != BN_CLICKED && code != BN_DOUBLECLICKED) break;
        if (id >= ID_PRESET0 && id < ID_PRESET0 + 6) {
            for (int i = 0; i < 4; i++) S.c[i] = PRESETS[id - ID_PRESET0].c[i];
        } else if (id >= ID_COLOR0 && id < ID_COLOR0 + 4) {
            pickColor(id - ID_COLOR0);
            return 0;
        } else if (id >= ID_FORM0 && id < ID_FORM0 + NUM_FORMS) {
            setForm(id - ID_FORM0, 1.6);
        } else if (id >= ID_CSTYLE0 && id < ID_CSTYLE0 + 3) {
            S.clockStyle = id - ID_CSTYLE0;
        } else if (bool* tg = toggleOf(id)) {
            *tg = !*tg;
            if (id == ID_AUTOMORPH) g_lastAuto = now();
        } else if (id == ID_CPOS) {
            S.clockPos = (S.clockPos + 1) % 7;
        } else if (id == ID_FPS) {
            int k = 0;
            for (int i = 0; i < 4; i++) if (FPS_OPTS[i] == S.fps) k = i;
            S.fps = FPS_OPTS[(k + 1) % 4];
        } else if (id == ID_FULL) toggleFullscreen();
        else if (id == ID_RESET) { defaults(); setForm(0, 1.6); }
        else break;
        g_dirty = true;
        syncControls();
        return 0;
    }
    case WM_DPICHANGED:
        g_setDpi = HIWORD(wp);
        buildSettings();
        sizeSettingsWindow((RECT*)lp);
        return 0;
    case WM_CLOSE:
        showSettings(false);
        return 0;
    case WM_ENTERSIZEMOVE: SetTimer(g_main, 1, 15, NULL); return 0;
    case WM_EXITSIZEMOVE: KillTimer(g_main, 1); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------- main window
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
    case WM_LBUTTONDBLCLK:
        toggleFullscreen();
        syncControls();
        return 0;
    case WM_RBUTTONUP:
        showSettings(!IsWindowVisible(g_set));
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { toggleFullscreen(); syncControls(); return 0; }
        break;
    case WM_MOUSEMOVE: {
        POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
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

static bool handleHotkey(const MSG& msg) {
    if (msg.message != WM_KEYDOWN) return false;
    if (msg.hwnd != g_main && msg.hwnd != g_set && GetParent(msg.hwnd) != g_set) return false;
    WPARAM k = msg.wParam;
    if (k >= '1' && k < '1' + NUM_FORMS) { setForm((int)(k - '1'), 1.6); syncControls(); return true; }
    if (k >= VK_NUMPAD1 && k < VK_NUMPAD1 + NUM_FORMS) { setForm((int)(k - VK_NUMPAD1), 1.6); syncControls(); return true; }
    switch (k) {
    case 'H': showSettings(!IsWindowVisible(g_set)); return true;
    case 'F': case VK_F11: toggleFullscreen(); syncControls(); return true;
    case 'M': S.autoMorph = !S.autoMorph; g_lastAuto = now(); g_dirty = true; syncControls(); return true;
    case 'C': S.clockStyle = (S.clockStyle + 1) % 3; g_dirty = true; syncControls(); return true;
    case 'A': S.audio = !S.audio; g_dirty = true; syncControls(); return true;
    case VK_SPACE: g_paused = !g_paused; return true;
    case VK_ESCAPE:
        if (IsWindowVisible(g_set)) showSettings(false);
        else if (g_fs) { toggleFullscreen(); syncControls(); }
        return true;
    }
    return false;
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

    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

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

    g_brBg = CreateSolidBrush(UI_BG);
    WNDCLASSEXW sc = {sizeof sc};
    sc.lpfnWndProc = SetProc;
    sc.hInstance = inst;
    sc.hIcon = icon;
    sc.hIconSm = iconSm;
    sc.hCursor = LoadCursor(NULL, IDC_ARROW);
    sc.hbrBackground = g_brBg;
    sc.lpszClassName = L"OrbSettings";
    RegisterClassExW(&sc);

    g_main = CreateWindowExW(0, L"OrbMain", L"Orb", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
                             NULL, NULL, inst, NULL);
    if (!g_main || !initD3D(g_main)) {
        MessageBoxW(NULL, L"Could not initialise Direct3D 11. Please update your graphics driver.", L"Orb", MB_ICONERROR);
        return 1;
    }
    darkTitle(g_main);

    g_set = CreateWindowExW(WS_EX_TOOLWINDOW, L"OrbSettings", L"Orb settings  ·  v" ORB_VERSION_W, WS_CAPTION | WS_SYSMENU | WS_POPUP,
                            0, 0, 100, 100, g_main, NULL, inst, NULL);
    darkTitle(g_set);
    g_setDpi = GetDpiForWindow(g_set);
    buildSettings();
    sizeSettingsWindow(NULL);

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
    syncControls();
    if (!hadIni) { showSettings(true); g_dirty = true; }

    MSG msg;
    double lastDraw = 0, lastSave = now();
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            if (handleHotkey(msg)) continue;
            if (IsWindowVisible(g_set) && IsDialogMessageW(g_set, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;

        double t = now();
        if (!g_cursorHidden && t - g_lastMove > 2.5 && !IsWindowVisible(g_set)) {
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
    releaseD3D();
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
