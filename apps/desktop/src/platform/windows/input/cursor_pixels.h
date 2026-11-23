#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <nlohmann/json.hpp>

namespace crossdesk {
// Draw twice to recover foreground color and alpha for both color and monochrome cursors.
inline nlohmann::json CursorPixels(HCURSOR cursor) {
  ICONINFO icon{};
  if (!GetIconInfo(cursor, &icon)) return nullptr;
  BITMAP bitmap{};
  bool color = icon.hbmColor != nullptr;
  GetObjectW(color ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap);
  if (icon.hbmColor) DeleteObject(icon.hbmColor);
  if (icon.hbmMask) DeleteObject(icon.hbmMask);
  const int w = bitmap.bmWidth, h = color ? bitmap.bmHeight : bitmap.bmHeight / 2;
  if (w < 1 || w > 128 || h < 1 || h > 128 || icon.xHotspot >= DWORD(w) || icon.yHotspot >= DWORD(h))
    return nullptr;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  HDC dc = CreateCompatibleDC(nullptr);
  if (!dc) return nullptr;
  void* memory = nullptr;
  HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &memory, nullptr, 0);
  if (!dib) { DeleteDC(dc); return nullptr; }
  auto old = SelectObject(dc, dib);
  auto pixels = static_cast<uint32_t*>(memory);
  const int n = w*h;
  std::fill_n(pixels, n, 0u);
  bool ok = DrawIconEx(dc, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL) != FALSE;
  GdiFlush();
  std::vector<uint32_t> black(pixels, pixels+n);
  std::fill_n(pixels, n, 0x00ffffffu);
  ok = ok && DrawIconEx(dc, 0, 0, cursor, w, h, 0, nullptr, DI_NORMAL);
  GdiFlush();
  std::string runs;
  constexpr char hex[] = "0123456789abcdef";
  auto emit = [&](uint32_t v, int digits) {
    for (int shift = (digits-1)*4; shift >= 0; shift -= 4) runs += hex[(v >> shift) & 15];
  };
  unsigned length = 0;
  uint32_t last = 0;
  for (int i=0; ok && i<n; ++i) {
    const int b = black[i] & 255, white = pixels[i] & 255;
    const int alpha = std::clamp(255 - white + b, 0, 255);
    // XOR cursors depend on the pixel beneath them. Use dark ink for those pixels.
    uint32_t argb = uint32_t(alpha) << 24;
    if (alpha && white >= b) {
      for (int shift : {0, 8, 16}) {
        unsigned channel = (black[i] >> shift) & 255;
        argb |= (std::min)(255u, (channel*255u + alpha/2) / alpha) << shift;
      }
    }
    if (length && (argb != last || length == 255)) {
      emit(length,2); emit(last,8); length=0;
    }
    last=argb; ++length;
    if (runs.size() > 11000) ok=false;
  }
  if (length) { emit(length,2); emit(last,8); }
  SelectObject(dc, old); DeleteObject(dib); DeleteDC(dc);
  if (!ok) return nullptr;
  int desktop_w = GetSystemMetrics(SM_CXSCREEN);
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0) {
    desktop_w = static_cast<int>(dm.dmPelsWidth);
  }
  return {{"w",w},{"h",h},{"hx",icon.xHotspot},{"hy",icon.yHotspot},
          {"desktop_w",desktop_w},{"runs",std::move(runs)}};
}
}  // namespace crossdesk
