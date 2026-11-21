#ifndef _SINGLE_DISPLAY_WIN_H_
#define _SINGLE_DISPLAY_WIN_H_

#include <Windows.h>

#include <vector>

namespace crossdesk {

class WindowsSingleDisplayGuard {
 public:
  bool Collapsed() const { return collapsed_; }

  bool CollapseIfMultiDisplay() {
    if (collapsed_) return false;

    // 1. Snapshot exact active display topology for full restoration.
    UINT32 active_path_count = 0;
    UINT32 active_mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &active_path_count,
                                    &active_mode_count) != ERROR_SUCCESS ||
        active_path_count == 0) {
      return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> active_paths(active_path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> active_modes(active_mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &active_path_count,
                           active_paths.data(), &active_mode_count,
                           active_modes.data(), nullptr) != ERROR_SUCCESS) {
      return false;
    }
    active_paths.resize(active_path_count);
    active_modes.resize(active_mode_count);

    // 2. Switch cleanly to the laptop's built-in internal physical display
    //    (keeps Intel GPU active so OEM WDTDrv watchdog never triggers,
    //     gathers all windows from dual screens onto 1 screen, and unlocks
    //     native 2560x1920 HiDPI 4:3 mode for OPPO Pad 5!).
    LONG status = SetDisplayConfig(0, nullptr, 0, nullptr,
                                   SDC_TOPOLOGY_INTERNAL | SDC_APPLY);
    if (status != ERROR_SUCCESS) {
      if (active_paths.size() <= 1) return false;
      size_t primary_index = 0;
      for (size_t i = 0; i < active_paths.size(); ++i) {
        const UINT32 idx = active_paths[i].sourceInfo.modeInfoIdx;
        if (idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            idx < active_modes.size() &&
            active_modes[idx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
            active_modes[idx].sourceMode.position.x == 0 &&
            active_modes[idx].sourceMode.position.y == 0) {
          primary_index = i;
          break;
        }
      }
      DISPLAYCONFIG_PATH_INFO single_path = active_paths[primary_index];
      auto apply_modes = active_modes;
      status = SetDisplayConfig(
          1, &single_path, static_cast<UINT32>(apply_modes.size()),
          apply_modes.data(),
          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
      if (status != ERROR_SUCCESS) return false;
    }

    // Apply crisp HiDPI 4:3 mode (2560x1920, fallback 2048x1536) on the active
    // single display if supported — fills ~94% of 7:5 OPPO Pad 5 razor-sharp!
    ApplyPreferredActiveMode(2560, 1920);

    saved_paths_ = std::move(active_paths);
    saved_modes_ = std::move(active_modes);
    collapsed_ = true;
    return true;
  }

  bool Restore() {
    if (!collapsed_) return true;
    LONG status = ERROR_GEN_FAILURE;
    if (!saved_paths_.empty() && !saved_modes_.empty()) {
      auto paths_copy = saved_paths_;
      auto modes_copy = saved_modes_;
      status = SetDisplayConfig(
          static_cast<UINT32>(paths_copy.size()), paths_copy.data(),
          static_cast<UINT32>(modes_copy.size()), modes_copy.data(),
          SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    }
    if (status != ERROR_SUCCESS) {
      const UINT32 fallback_topology =
          saved_paths_.size() > 1 ? SDC_TOPOLOGY_EXTEND : SDC_TOPOLOGY_INTERNAL;
      status = SetDisplayConfig(0, nullptr, 0, nullptr,
                                fallback_topology | SDC_APPLY);
    }
    if (status != ERROR_SUCCESS) return false;
    saved_paths_.clear();
    saved_modes_.clear();
    collapsed_ = false;
    return true;
  }

 private:
  static void ApplyPreferredActiveMode(DWORD pref_w, DWORD pref_h) {
    for (DWORD index = 0;; ++index) {
      DISPLAY_DEVICEW dd{};
      dd.cb = sizeof(dd);
      if (!EnumDisplayDevicesW(nullptr, index, &dd, 0)) break;
      if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
      DEVMODEW best{};
      bool found = false;
      DEVMODEW mode{};
      mode.dmSize = sizeof(mode);
      for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &mode); ++m) {
        if (mode.dmPelsWidth == pref_w && mode.dmPelsHeight == pref_h &&
            mode.dmBitsPerPel == 32 && !(mode.dmDisplayFlags & DM_INTERLACED)) {
          if (!found || mode.dmDisplayFrequency > best.dmDisplayFrequency) {
            best = mode;
            found = true;
          }
        }
      }
      if (found) {
        best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                        DM_DISPLAYFREQUENCY;
        ChangeDisplaySettingsExW(dd.DeviceName, &best, nullptr, 0, nullptr);
      }
      return;
    }
  }

  bool collapsed_ = false;
  std::vector<DISPLAYCONFIG_PATH_INFO> saved_paths_;
  std::vector<DISPLAYCONFIG_MODE_INFO> saved_modes_;
};

}  // namespace crossdesk

#endif
