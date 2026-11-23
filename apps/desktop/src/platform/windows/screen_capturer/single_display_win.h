#ifndef _SINGLE_DISPLAY_WIN_H_
#define _SINGLE_DISPLAY_WIN_H_

#include <Windows.h>
#include <sddl.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "display_label.h"
#include "rd_log.h"

namespace crossdesk {

// A connected viewer can receive input and video only while this exact
// virtual desktop is active. Keep the original topology in memory for teardown.
class WindowsSingleDisplayGuard {
 public:
  bool Tracked() const { return lease_ != nullptr; }
  static bool DriverPresent() {
    for (DWORD i = 0;; ++i) {
      DISPLAY_DEVICEW adapter{};
      adapter.cb = sizeof(adapter);
      if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0)) return false;
      if (IsVirtualAdapter(adapter)) return true;
    }
  }
  const std::string& capture_display() const { return capture_display_; }

  struct Scale {
    int percent = 0;
    bool accepted = false;
    std::vector<int> options;
  };

  Scale DisplayScale(int requested) const {
    if (capture_display_.empty()) return {};
    const auto source = ActiveVirtualSource();
    if (!source || GetDisplayLabel(SourceName(*source)) != capture_display_) return {};
    const auto dpi = ReadScale(source->sourceInfo);
    if (!dpi) return {};
    Scale result;
    result.percent = kScales[dpi->current - dpi->minimum];
    result.options.assign(std::begin(kScales),
                          std::begin(kScales) + dpi->maximum - dpi->minimum + 1);
    if (requested == 0 || requested == result.percent) {
      result.accepted = true;
      return result;
    }
    const auto option = std::find(result.options.begin(), result.options.end(), requested);
    if (option == result.options.end()) return result;
    const auto& identity = source->sourceInfo;
    DpiSet set{{static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(UINT32(-4)), sizeof(DpiSet),
                identity.adapterId, identity.id},
               dpi->minimum + static_cast<int>(option - result.options.begin())};
    if (DisplayConfigSetDeviceInfo(&set.header) != ERROR_SUCCESS) return result;
    const auto active = ActiveVirtualSource();
    if (!active || active->sourceInfo.id != identity.id ||
        active->sourceInfo.adapterId.LowPart != identity.adapterId.LowPart ||
        active->sourceInfo.adapterId.HighPart != identity.adapterId.HighPart)
      return {};
    const auto actual = ReadScale(active->sourceInfo);
    if (!actual) return {};
    result.percent = kScales[actual->current - actual->minimum];
    result.options.assign(std::begin(kScales),
                          std::begin(kScales) + actual->maximum - actual->minimum + 1);
    result.accepted = result.percent == requested;
    return result;
  }

  bool Update() {
    const auto now = GetTickCount64();
    if (!lease_) {
      if (now < next_probe_) return false;
      lease_ = AcquireLease();
      if (!lease_) { next_probe_ = now + 1000; return false; }
    }
    if (attempted_ && !switching_ && capture_display_.empty()) {
      if (now < next_probe_) return false;
      next_probe_ = now + 2000;
      if (!RestoreTopology()) return false;
      attempted_ = false;
    }
    if (!attempted_) {
      attempted_ = true;
      if (!ReadTopology(QDC_ONLY_ACTIVE_PATHS, &saved_paths_, &saved_modes_) ||
          !KeepPhysicalPaths(saved_paths_, saved_modes_))
        return Fail("could not save a physical display layout");
      saved_primary_.clear();
      for (const auto& path : saved_paths_) {
        const auto index = path.sourceInfo.modeInfoIdx;
        if (index >= saved_modes_.size()) continue;
        const auto& p = saved_modes_[index].sourceMode.position;
        if (p.x == 0 && p.y == 0) { saved_primary_ = TargetPath(path); break; }
      }
      keep_physical_outputs_ = saved_paths_.size() > 1;
      if (!ApplyVirtualTopology())
        return Fail("MttVDD did not offer an available 3000x2120 display");
      switching_ = true;
      stable_since_ = 0;
      next_probe_ = now + 250;
      deadline_ = now + 12000;
      return false;
    }
    if (now < next_probe_) return !capture_display_.empty();
    next_probe_ = now + (switching_ ? 250 : 500);
    const auto active = ActiveVirtualDisplayName();
    if (active.empty()) {
      stable_since_ = 0;
      if (!switching_ || now >= deadline_)
        return Fail("the requested 3000x2120 virtual layout did not stay stable");
      return false;
    }

    if (!switching_) {
      if (active != capture_display_) return Fail("virtual screen identity changed");
      return true;
    }
    if (stable_since_ == 0) stable_since_ = now;
    // Some display drivers revert a newly applied topology several seconds
    // later. Wait through that transition before opening either media or input.
    if (now - stable_since_ < (keep_physical_outputs_ ? 500u : 7000u)) {
      if (now >= deadline_) return Fail("the virtual screen did not stabilize");
      return false;
    }
    switching_ = false;
    capture_display_ = active;
    LOG_INFO("Tablet display: {} ready at 3000x2120 ({})",
             capture_display_, keep_physical_outputs_ ? "physical outputs retained" : "exclusive");
    return true;
  }

  bool Restore() {
    capture_display_.clear();
    switching_ = false;
    if (!RestoreTopology()) return false;
    attempted_ = false;
    next_probe_ = 0;
    if (lease_) {
      ReleaseMutex(lease_);
      CloseHandle(lease_);
      lease_ = nullptr;
    }
    return true;
  }

  static void RemoveIdleVirtualDisplay() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return;
    const auto active_paths = paths.size();
    if (!KeepPhysicalPaths(paths, modes) || paths.size() == active_paths) return;
    HANDLE lease = AcquireLease();
    if (!lease) return;
    // Re-read under the lease; a new session may have ended since the probe.
    if (DetachLiveVirtual())
      LOG_INFO("Tablet display: detached an idle virtual output");
    ReleaseMutex(lease);
    CloseHandle(lease);
  }

 private:
  // A direct LAN viewer and the service-owned RTC host live in separate
  // processes on the same interactive desktop. Only the capture owner switches it.
  static HANDLE AcquireLease() {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x100001;;;IU)(A;;0x100001;;;SY)(A;;0x100001;;;BA)",
            SDDL_REVISION_1, &sd, nullptr)) return nullptr;
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), sd, FALSE};
    HANDLE handle = CreateMutexExW(&sa, L"Local\\CrossDesk.TabletDisplay", 0,
                                   SYNCHRONIZE | MUTEX_MODIFY_STATE);
    LocalFree(sd);
    if (!handle) return nullptr;
    DWORD status = WaitForSingleObject(handle, 0);
    if (status == WAIT_OBJECT_0 || status == WAIT_ABANDONED) return handle;
    CloseHandle(handle);
    return nullptr;
  }

  static bool IsVirtualAdapter(const DISPLAY_DEVICEW& device) {
    return _wcsicmp(device.DeviceString, L"Virtual Display Driver") == 0 ||
           _wcsicmp(device.DeviceString, L"MttVDD Virtual Display Driver") == 0;
  }

  static constexpr UINT32 kWidth = 3000;
  static constexpr UINT32 kHeight = 2120;
  static constexpr int kScales[] = {100, 125, 150, 175, 200, 225,
                                    250, 300, 350, 400, 450, 500};
  // Windows uses private DisplayConfig packet types for per-source DPI.
  // Fail closed if the OS rejects these packets or returns an unknown range.
  struct DpiGet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    LONG minimum, current, maximum;
  };
  struct DpiSet {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    LONG requested;
  };
  static_assert(sizeof(DpiGet) == 32 && sizeof(DpiSet) == 24);
  static std::optional<DpiGet> ReadScale(const DISPLAYCONFIG_PATH_SOURCE_INFO& source) {
    DpiGet dpi{{static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(UINT32(-3)), sizeof(DpiGet),
                source.adapterId, source.id}, 0, 0, 0};
    constexpr int count = static_cast<int>(std::size(kScales));
    if (DisplayConfigGetDeviceInfo(&dpi.header) != ERROR_SUCCESS ||
        dpi.minimum > 0 || dpi.minimum <= -count ||
        dpi.maximum < 0 || dpi.maximum >= count ||
        dpi.maximum - dpi.minimum >= count ||
        dpi.current < dpi.minimum || dpi.current > dpi.maximum) return std::nullopt;
    return dpi;
  }
  static constexpr UINT32 kConfig =
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


  static bool IsMttVdd(const DISPLAYCONFIG_PATH_INFO& path) {
    const auto name = SourceName(path);
    if (name.empty()) return false;
    for (DWORD index = 0;; ++index) {
      DISPLAY_DEVICEW device{};
      device.cb = sizeof(device);
      if (!EnumDisplayDevicesW(nullptr, index, &device, 0)) return false;
      if (_wcsicmp(device.DeviceName, name.c_str()) != 0) continue;
      return IsVirtualAdapter(device);
    }
  }

  static bool KeepPhysicalPaths(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    const auto active_paths = paths.size();
    paths.erase(std::remove_if(paths.begin(), paths.end(), IsMttVdd), paths.end());
    if (paths.empty()) return false;
    if (paths.size() == active_paths) return true;
    std::vector<DISPLAYCONFIG_MODE_INFO> retained;
    std::vector<UINT32> indices(modes.size(), DISPLAYCONFIG_PATH_MODE_IDX_INVALID);
    auto retain = [&](UINT32& index) {
      if (index == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) return true;
      if (index >= modes.size()) return false;
      if (indices[index] == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
        indices[index] = static_cast<UINT32>(retained.size());
        retained.push_back(modes[index]);
      }
      index = indices[index];
      return true;
    };
    for (auto& path : paths)
      if (!retain(path.sourceInfo.modeInfoIdx) || !retain(path.targetInfo.modeInfoIdx))
        return false;
    modes.swap(retained);

    // Preserve monitor arrangement. Move the remaining group only if MttVDD
    // owned the origin, since Windows requires a desktop source at (0, 0).
    std::vector<UINT32> sources;
    for (const auto& path : paths) {
      const auto index = path.sourceInfo.modeInfoIdx;
      if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
        return false;
      if (std::find(sources.begin(), sources.end(), index) == sources.end())
        sources.push_back(index);
    }
    for (const auto index : sources) {
      const auto& position = modes[index].sourceMode.position;
      if (position.x == 0 && position.y == 0) return true;
    }
    const auto origin = modes[sources.front()].sourceMode.position;
    for (const auto index : sources) {
      auto& position = modes[index].sourceMode.position;
      position.x -= origin.x;
      position.y -= origin.y;
    }
    return true;
  }

  static bool ApplyPhysicalTopology(std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
                                    std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    const auto path_count = static_cast<UINT32>(paths.size());
    const auto mode_count = static_cast<UINT32>(modes.size());
    auto status = SetDisplayConfig(path_count, paths.data(), mode_count,
                                   modes.data(), SDC_VALIDATE | kConfig);
    if (status == ERROR_SUCCESS)
      status = SetDisplayConfig(path_count, paths.data(), mode_count,
                                modes.data(), SDC_APPLY | kConfig);
    if (status != ERROR_SUCCESS) {
      LOG_WARN("Tablet display: restoring physical topology failed, status={}", status);
      return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> active;
    std::vector<DISPLAYCONFIG_MODE_INFO> active_modes;
    return ReadTopology(QDC_ONLY_ACTIVE_PATHS, &active, &active_modes) &&
           std::none_of(active.begin(), active.end(), IsMttVdd);
  }

  static std::optional<DISPLAYCONFIG_PATH_INFO> FindVirtualPath() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ALL_PATHS, &paths, &modes)) return std::nullopt;
    for (const auto& path : paths)
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) && IsMttVdd(path)) return path;
    for (const auto& path : paths) {
      if (path.targetInfo.targetAvailable && IsMttVdd(path)) return path;
    }
    return std::nullopt;
  }

  bool ApplyVirtualTopology() {
    auto target = FindVirtualPath();
    if (!target) return false;
    target->flags |= DISPLAYCONFIG_PATH_ACTIVE;
    target->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;

    DISPLAYCONFIG_MODE_INFO mode{};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.id = target->sourceInfo.id;
    mode.adapterId = target->sourceInfo.adapterId;
    mode.sourceMode.width = kWidth;
    mode.sourceMode.height = kHeight;
    mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;

    // Some dual-screen setups reactivate physical outputs after an exclusive
    // switch. Preserve their original modes and add only the MttVDD output.
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (keep_physical_outputs_) {
      paths = saved_paths_;
      modes = saved_modes_;
      int64_t right_edge = 0;
      LONG top = 0;
      for (const auto& path : paths) {
        const auto index = path.sourceInfo.modeInfoIdx;
        if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
          return false;
        const auto& source = modes[index].sourceMode;
        const auto edge = static_cast<int64_t>(source.position.x) + source.width;
        if (edge > right_edge) {
          right_edge = edge;
          top = source.position.y;
        }
      }
      // Keep the physical monitors adjacent in their existing arrangement.
      // The tablet at the origin gets Explorer's main taskbar.
      for (auto& entry : modes) {
        if (entry.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) continue;
        auto& position = entry.sourceMode.position;
        const int64_t x = static_cast<int64_t>(position.x) - right_edge;
        const int64_t y = static_cast<int64_t>(position.y) - top;
        if (x < INT32_MIN || x > INT32_MAX || y < INT32_MIN || y > INT32_MAX)
          return false;
        position = {static_cast<LONG>(x), static_cast<LONG>(y)};
      }
    }
    target->sourceInfo.modeInfoIdx = static_cast<UINT32>(modes.size());
    paths.push_back(*target);
    modes.push_back(mode);
    const auto path_count = static_cast<UINT32>(paths.size());
    const auto mode_count = static_cast<UINT32>(modes.size());
    LONG status = SetDisplayConfig(path_count, paths.data(), mode_count,
                                   modes.data(), SDC_VALIDATE | kConfig);
    if (status == ERROR_SUCCESS)
      status = SetDisplayConfig(path_count, paths.data(), mode_count,
                                modes.data(), SDC_APPLY | kConfig);
    if (status != ERROR_SUCCESS) {
      LOG_WARN("Tablet display: SetDisplayConfig failed, status={}", status);
      return false;
    }
    changed_ = true;
    return true;
  }

  std::optional<DISPLAYCONFIG_PATH_INFO> ActiveVirtualSource() const {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return {};
    if (!keep_physical_outputs_ && paths.size() != 1) return {};
    const auto target = std::find_if(paths.begin(), paths.end(), IsMttVdd);
    if (target == paths.end() || std::any_of(std::next(target), paths.end(), IsMttVdd))
      return {};
    const auto index = target->sourceInfo.modeInfoIdx;
    if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
      return {};
    const auto& mode = modes[index].sourceMode;
    if (mode.width != kWidth || mode.height != kHeight ||
        mode.position.x != 0 || mode.position.y != 0)
      return {};
    return *target;
  }

  std::string ActiveVirtualDisplayName() const {
    const auto source = ActiveVirtualSource();
    return source ? GetDisplayLabel(SourceName(*source)) : std::string{};
  }

  bool Fail(const char* message) {
    LOG_WARN("Tablet display: {}; video and input remain paused", message);
    capture_display_.clear();
    switching_ = false;
    next_probe_ = GetTickCount64() + 2000;
    RestoreTopology();
    return false;
  }

  static std::wstring TargetPath(const DISPLAYCONFIG_PATH_INFO& path) {
    DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
    name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name),
                   path.targetInfo.adapterId, path.targetInfo.id};
    return DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS
        ? name.monitorDevicePath : L"";
  }

  static bool DetachLiveVirtual(const std::wstring& primary = {}) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return false;
    // If the driver re-enumerated its sources, prefer the saved physical
    // monitor by its device path when rebasing the live remaining group.
    if (!primary.empty()) {
      auto found = std::find_if(paths.begin(), paths.end(), [&](const auto& p) {
        return TargetPath(p) == primary;
      });
      if (found != paths.end()) std::rotate(paths.begin(), found, std::next(found));
    }
    const auto count = paths.size();
    if (!KeepPhysicalPaths(paths, modes)) return false;
    return count == paths.size() || ApplyPhysicalTopology(paths, modes);
  }

  bool RestoreTopology() {
    if (changed_) {
      // In extended mode physical paths stayed live. Their adapter IDs may
      // have been regenerated after lock/unlock or HDMI changes.
      if (!ApplyPhysicalTopology(saved_paths_, saved_modes_) &&
          !DetachLiveVirtual(saved_primary_)) return false;
      changed_ = false;
    }
    saved_paths_.clear();
    saved_modes_.clear();
    return true;
  }

  HANDLE lease_ = nullptr;
  bool attempted_ = false;
  bool switching_ = false;
  bool changed_ = false;
  bool keep_physical_outputs_ = false;
  ULONGLONG next_probe_ = 0;
  ULONGLONG deadline_ = 0;
  ULONGLONG stable_since_ = 0;
  std::wstring saved_primary_;
  std::string capture_display_;
  std::vector<DISPLAYCONFIG_PATH_INFO> saved_paths_;
  std::vector<DISPLAYCONFIG_MODE_INFO> saved_modes_;
};

}  // namespace crossdesk

#endif
