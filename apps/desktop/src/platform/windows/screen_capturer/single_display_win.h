#ifndef _SINGLE_DISPLAY_WIN_H_
#define _SINGLE_DISPLAY_WIN_H_

#include <Windows.h>

#include <cwchar>
#include <set>
#include <string>
#include <vector>

namespace crossdesk {

class WindowsSingleDisplayGuard {
 public:
  bool Collapsed() const { return collapsed_; }

  bool CollapseIfMultiDisplay() {
    if (collapsed_) return false;

    // 1. Snapshot current active display topology for exact restoration.
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

    // 2. Priority 1: Switch exclusively to installed usbmmidd virtual display
    //    (supports custom 7:5 tablet & 20:9 phone native resolutions).
    const auto vdd_devices = FindUsbmmiddDeviceNames();
    if (!vdd_devices.empty()) {
      // If we are already exclusively on the single usbmmidd virtual display,
      // nothing to switch.
      if (active_paths.size() == 1 &&
          IsPathInDeviceSet(active_paths[0], vdd_devices)) {
        return false;
      }

      DISPLAYCONFIG_PATH_INFO vdd_path{};
      if (FindAvailablePathForDevices(vdd_devices, &vdd_path)) {
        vdd_path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
        vdd_path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        vdd_path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

        const LONG vdd_status = SetDisplayConfig(
            1, &vdd_path, 0, nullptr,
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
        if (vdd_status == ERROR_SUCCESS) {
          saved_paths_ = std::move(active_paths);
          saved_modes_ = std::move(active_modes);
          collapsed_ = true;
          return true;
        }
      }
    }

    // 3. Priority 2 (Option A fallback): Collapse physical multi-display to
    //    the single primary display so all windows gather onto one screen.
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
    std::vector<DISPLAYCONFIG_MODE_INFO> apply_modes = active_modes;
    LONG status = SetDisplayConfig(
        1, &single_path, static_cast<UINT32>(apply_modes.size()),
        apply_modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    if (status != ERROR_SUCCESS) {
      status = SetDisplayConfig(0, nullptr, 0, nullptr,
                                SDC_TOPOLOGY_INTERNAL | SDC_APPLY);
    }
    if (status != ERROR_SUCCESS) return false;

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
  static std::set<std::wstring> FindUsbmmiddDeviceNames() {
    std::set<std::wstring> devices;
    for (DWORD index = 0;; ++index) {
      DISPLAY_DEVICEW dd{};
      dd.cb = sizeof(dd);
      if (!EnumDisplayDevicesW(nullptr, index, &dd, 0)) break;
      if (std::wcsstr(dd.DeviceString, L"USB Mobile Monitor") != nullptr) {
        devices.insert(dd.DeviceName);
      }
    }
    return devices;
  }

  static bool IsPathInDeviceSet(const DISPLAYCONFIG_PATH_INFO& path,
                                const std::set<std::wstring>& devices) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
      return false;
    }
    return devices.find(source.viewGdiDeviceName) != devices.end();
  }

  static bool FindAvailablePathForDevices(const std::set<std::wstring>& devices,
                                          DISPLAYCONFIG_PATH_INFO* out_path) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) !=
            ERROR_SUCCESS ||
        path_count == 0) {
      return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count > 0 ? mode_count : 1);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths.data(),
                           &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS) {
      return false;
    }
    paths.resize(path_count);

    for (const auto& path : paths) {
      if (!path.targetInfo.targetAvailable) continue;
      if (IsPathInDeviceSet(path, devices)) {
        *out_path = path;
        return true;
      }
    }
    return false;
  }

  bool collapsed_ = false;
  std::vector<DISPLAYCONFIG_PATH_INFO> saved_paths_;
  std::vector<DISPLAYCONFIG_MODE_INFO> saved_modes_;
};

}  // namespace crossdesk

#endif
