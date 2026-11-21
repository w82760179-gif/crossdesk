/*
 * @Author: DI JUNKUN
 * @Date: 2026-09-11
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 * Customized: solid pure-black privacy screen without branding or text overlay.
 */

#include "privacy_cover_renderer.h"

namespace crossdesk {

bool DrawPrivacyCover(HDC dc, const RECT& rect, UINT dpi, HINSTANCE module,
                      const wchar_t* unlock_hint, bool allow_local_unlock) {
  (void)dpi;
  (void)module;
  (void)unlock_hint;
  (void)allow_local_unlock;
  return FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))) != 0;
}

}  // namespace crossdesk
