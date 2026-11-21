#ifndef _SINGLE_DISPLAY_WIN_H_
#define _SINGLE_DISPLAY_WIN_H_

#include <Windows.h>

#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <vector>

#include "rd_log.h"
#include "display_label.h"
#include "../virtual_display/usbmmidd_virtual_display.h"
#include "../virtual_display/virtual_display_provisioner.h"

namespace crossdesk {

class WindowsSingleDisplayGuard {
 public:
  enum class Result { unchanged, virtual_display, physical_display };

  bool Tracked() const { return attempted_ || !saved_paths_.empty(); }
  const std::string& capture_display() const { return capture_display_; }
  void AllowManualMode() { use_tablet_mode_ = false; }

  Result Update() {
    if (provisioning_.valid()) {
      if (provisioning_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        return Result::unchanged;
      }
      if (provisioning_.get() && ApplyVirtualTopology()) {
        next_probe_ = GetTickCount64() + 1000;
        return Result::virtual_display;
      }
      LOG_WARN("Tablet display: 3000x2120 virtual topology could not be applied");
      return RestoreTopology() ? Result::physical_display : Result::unchanged;
    }

    if (!capture_display_.empty()) {
      const auto now = GetTickCount64();
      if (now >= next_probe_) {
        next_probe_ = now + 1000;
        if (!VirtualTargetActive() || (use_tablet_mode_ && !EnsureTabletMode())) {
          LOG_WARN("Tablet display: virtual target or mode is unavailable; restoring physical displays");
          if (RestoreTopology()) return Result::physical_display;
        }
      }
    }
    if (attempted_ || !saved_paths_.empty()) return Result::unchanged;
    if (release_.valid()) {
      if (release_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        return Result::unchanged;
      }
      release_.get();
    }

    attempted_ = true;
    if (GetSystemMetrics(SM_REMOTESESSION) ||
        !ReadTopology(QDC_ONLY_ACTIVE_PATHS, &saved_paths_, &saved_modes_) ||
        PhysicalPathIndex() == saved_paths_.size()) {
      saved_paths_.clear();
      saved_modes_.clear();
      return Result::unchanged;
    }

    if (FindVirtualPath() && ApplyVirtualTopology()) {
      next_probe_ = GetTickCount64() + 1000;
      return Result::virtual_display;
    }
    provisioner_.PrepareAcquire();
    provisioning_ = std::async(std::launch::async, [this] {
      std::string error;
      return provisioner_.Acquire(kTabletMode, &error);
    });
    return Result::unchanged;
  }

  bool Restore() {
    if (provisioning_.valid()) {
      provisioner_.RequestCancel();
      if (provisioning_.wait_for(std::chrono::seconds(0)) !=
          std::future_status::ready) {
        return false;
      }
      provisioning_.get();
    }
    if (!RestoreTopology()) return false;
    attempted_ = false;
    return true;
  }

 private:
  static constexpr VirtualDisplayMode kTabletMode{3000, 2120};
  static constexpr UINT32 kSuppliedConfig =
      SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;

  static bool ReadTopology(UINT32 flags,
                           std::vector<DISPLAYCONFIG_PATH_INFO>* paths,
                           std::vector<DISPLAYCONFIG_MODE_INFO>* modes) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      UINT32 count = 0, mode_count = 0;
      if (GetDisplayConfigBufferSizes(flags, &count, &mode_count) != ERROR_SUCCESS ||
          count == 0) return false;
      paths->resize(count);
      modes->resize(mode_count ? mode_count : 1);
      const LONG status = QueryDisplayConfig(flags, &count, paths->data(),
                                             &mode_count, modes->data(), nullptr);
      if (status == ERROR_INSUFFICIENT_BUFFER) continue;
      if (status != ERROR_SUCCESS) return false;
      paths->resize(count);
      modes->resize(mode_count);
      return count > 0;
    }
    return false;
  }

  static std::wstring SourceName(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
    name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    name.header.size = sizeof(name);
    name.header.adapterId = path.sourceInfo.adapterId;
    name.header.id = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS) return {};
    return name.viewGdiDeviceName;
  }

  size_t PhysicalPathIndex() const {
    size_t physical = saved_paths_.size();
    for (size_t index = 0; index < saved_paths_.size(); ++index) {
      const auto& path = saved_paths_[index];
      const auto name = SourceName(path);
      const auto mode = path.sourceInfo.modeInfoIdx;
      const auto technology = path.targetInfo.outputTechnology;
      if (name.empty() || IsUsbmmiddDisplayDevice(name) ||
          technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL ||
          mode >= saved_modes_.size() ||
          saved_modes_[mode].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        continue;
      }
      if (technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
          technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS ||
          technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
          technology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED) {
        return index;
      }
      if (physical == saved_paths_.size()) physical = index;
    }
    return physical;
  }

  static std::optional<DISPLAYCONFIG_PATH_INFO> FindVirtualPath() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ALL_PATHS, &paths, &modes)) return std::nullopt;
    // Windows returns active paths first. A previously detached but still
    // available target can also be reused without plugging another monitor.
    for (const auto& path : paths) {
      if (path.targetInfo.targetAvailable &&
          IsUsbmmiddDisplayDevice(SourceName(path), false)) return path;
    }
    return std::nullopt;
  }

  bool ApplyVirtualTopology() {
    const auto target = FindVirtualPath();
    const auto physical_index = PhysicalPathIndex();
    if (!target || physical_index == saved_paths_.size()) return false;
    const auto& path = *target;
    const auto name = SourceName(path);
    auto physical = saved_paths_[physical_index];
    auto physical_mode = saved_modes_[physical.sourceInfo.modeInfoIdx];
    physical_mode.sourceMode.position = {kTabletMode.width, 0};

    DISPLAYCONFIG_MODE_INFO virtual_mode{};
    virtual_mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    virtual_mode.id = path.sourceInfo.id;
    virtual_mode.adapterId = path.sourceInfo.adapterId;
    virtual_mode.sourceMode.width = kTabletMode.width;
    virtual_mode.sourceMode.height = kTabletMode.height;
    virtual_mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    DISPLAYCONFIG_PATH_INFO paths[] = {path, physical};
    DISPLAYCONFIG_MODE_INFO source_modes[] = {virtual_mode, physical_mode};
    for (UINT32 index = 0; index < 2; ++index) {
      paths[index].flags |= DISPLAYCONFIG_PATH_ACTIVE;
      paths[index].sourceInfo.modeInfoIdx = index;
      paths[index].targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    }

    LONG status = SetDisplayConfig(2, paths, 2, source_modes,
                                   SDC_VALIDATE | kSuppliedConfig);
    if (status == ERROR_SUCCESS) {
      status = SetDisplayConfig(2, paths, 2, source_modes,
                                SDC_APPLY | kSuppliedConfig);
    }
    if (status != ERROR_SUCCESS) {
      LOG_WARN("Tablet display: SetDisplayConfig failed, status={}", status);
      return false;
    }
    virtual_device_ = name;
    if (!VirtualTargetActive() || !EnsureTabletMode()) return false;
    capture_display_ = GetDisplayLabel(virtual_device_);
    use_tablet_mode_ = true;
    LOG_INFO("Tablet display: {} is the 3000x2120 virtual primary; physical display retained",
             capture_display_);
    return !capture_display_.empty();
  }

  bool VirtualTargetActive() const {
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    return !virtual_device_.empty() &&
           FindUsbmmiddDisplayDeviceName() == virtual_device_ &&
           EnumDisplaySettingsW(virtual_device_.c_str(), ENUM_CURRENT_SETTINGS,
                                &current) &&
           current.dmPosition.x == 0 && current.dmPosition.y == 0;
  }

  bool EnsureTabletMode() const {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    const auto read_exact_mode = [&] {
      return EnumDisplaySettingsW(virtual_device_.c_str(), ENUM_CURRENT_SETTINGS,
                                  &mode) &&
             mode.dmPelsWidth == kTabletMode.width &&
             mode.dmPelsHeight == kTabletMode.height;
    };
    if (read_exact_mode()) return true;

    mode = {};
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    mode.dmPelsWidth = kTabletMode.width;
    mode.dmPelsHeight = kTabletMode.height;
    mode.dmBitsPerPel = 32;
    return ChangeDisplaySettingsExW(virtual_device_.c_str(), &mode, nullptr,
                                    CDS_TEST, nullptr) == DISP_CHANGE_SUCCESSFUL &&
           ChangeDisplaySettingsExW(virtual_device_.c_str(), &mode, nullptr,
                                    0, nullptr) == DISP_CHANGE_SUCCESSFUL &&
           read_exact_mode();
  }

  bool RestoreTopology() {
    if (!saved_paths_.empty()) {
      const auto status = SetDisplayConfig(
          static_cast<UINT32>(saved_paths_.size()), saved_paths_.data(),
          static_cast<UINT32>(saved_modes_.size()), saved_modes_.data(),
          SDC_APPLY | kSuppliedConfig);
      if (status != ERROR_SUCCESS) {
        LOG_WARN("Tablet display: could not restore the original display topology, status={}", status);
        return false;
      }
      saved_paths_.clear();
      saved_modes_.clear();
    }
    capture_display_.clear();
    virtual_device_.clear();
    if (!release_.valid() && provisioner_.NeedsRelease()) {
      release_ = std::async(std::launch::async, [this] { provisioner_.Release(); });
    }
    return true;
  }

  bool attempted_ = false;
  bool use_tablet_mode_ = true;
  ULONGLONG next_probe_ = 0;
  std::wstring virtual_device_;
  std::string capture_display_;
  std::vector<DISPLAYCONFIG_PATH_INFO> saved_paths_;
  std::vector<DISPLAYCONFIG_MODE_INFO> saved_modes_;
  VirtualDisplayProvisioner provisioner_;
  std::future<bool> provisioning_;
  std::future<void> release_;
};

}  // namespace crossdesk

#endif
