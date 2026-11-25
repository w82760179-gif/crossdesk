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
#include "tablet_display_process.h"
#include "rd_log.h"

namespace crossdesk {

// Only the capture owner may request a tablet session. The independent helper
// holds the display lease until both original layouts have been restored.
class WindowsSingleDisplayGuard {
 public:
  static bool ExclusiveDesktopActive() {
    return GetSystemMetrics(SM_CMONITORS) == 1 && ActiveVirtualSource().has_value();
  }
  bool Tracked() const { return worker_.started(); }
  static bool DriverPresent() {
    for (DWORD i = 0;; ++i) {
      DISPLAY_DEVICEW adapter{};
      adapter.cb = sizeof(adapter);
      if (!EnumDisplayDevicesW(nullptr, i, &adapter, 0)) return false;
      if (IsVirtualAdapter(adapter)) return true;
    }
  }
  const std::string& capture_display() const { return capture_display_; }
  void SetPreferredMode(UINT32 width, UINT32 height) {
    if (width >= 640 && height >= 480 && width <= 32768 && height <= 32768) {
      preferred_width_ = width;
      preferred_height_ = height;
    }
  }
  void ClearPreferredMode() {
    preferred_width_ = 0;
    preferred_height_ = 0;
  }
  void NoteModeTransition() {
    missing_output_polls_ = 0;
    next_probe_ = GetTickCount64() + 1200;
  }

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
    if (stopping_) {
      if (!Restore()) return false;
    }
    if (!worker_.started()) {
      if (now < next_probe_) return false;
      next_probe_ = now + 2000;
      missing_output_polls_ = 0;
      if (!worker_.Start(preferred_width_, preferred_height_)) return false;
      deadline_ = now + 12000;
    }
    if (!worker_.running() || (!worker_.ready() && now >= deadline_))
      return Fail("display helper did not become ready");
    if (!worker_.ready()) return false;
    if (!capture_display_.empty() && now < next_probe_) return true;
    next_probe_ = now + 500;
    const auto active = ActiveVirtualDisplayName();
    if (active.empty() || (!capture_display_.empty() && active != capture_display_)) {
      if (!capture_display_.empty() && ++missing_output_polls_ < 4) {
        LOG_WARN("Tablet display: transient virtual output miss ({}/4); holding active display",
                 missing_output_polls_);
        return true;
      }
      return Fail("exclusive virtual output changed");
    }
    missing_output_polls_ = 0;
    if (capture_display_.empty())
      LOG_INFO("Tablet display: {} ready (exclusive)", active);
    capture_display_ = active;
    return true;
  }

  bool Restore(DWORD wait_ms = 0) {
    capture_display_.clear();
    missing_output_polls_ = 0;
    stopping_ = true;
    if (!worker_.Stop(wait_ms)) return false;
    stopping_ = false;
    return true;
  }

  // Called only in the display helper process. Its parent supplies inherited
  // handles, so a recycled PID cannot keep a disconnected desktop black.
  static int RunDisplayHelper(HANDLE parent, HANDLE stop, HANDLE ready,
                              UINT32 preferred_width = 0,
                              UINT32 preferred_height = 0) {
    HANDLE signals[] = {parent, stop};
    if (WaitForMultipleObjects(2, signals, FALSE, 0) != WAIT_TIMEOUT) return 1;
    HANDLE lease = AcquireLease();
    if (!lease) return 1;
    // Process exit releases the lease on every return path.
    Layout active, database;
    if (!active.Read(QDC_ONLY_ACTIVE_PATHS) || !database.Read(QDC_DATABASE_CURRENT) ||
        std::all_of(active.paths.begin(), active.paths.end(), IsMttVdd) ||
        !active.Validate() || !database.Validate()) return 1;
    if (WaitForMultipleObjects(2, signals, FALSE, 0) != WAIT_TIMEOUT) return 0;
    const bool applied = ApplyVirtualTopology(preferred_width, preferred_height);
    if (!applied && active.Matches(QDC_ONLY_ACTIVE_PATHS) && database.Matches(QDC_DATABASE_CURRENT))
      return 1;
    bool confirmed = false;
    for (unsigned i = 0; applied && i < 20; ++i) {
      if (WaitForMultipleObjects(2, signals, FALSE, 0) != WAIT_TIMEOUT) break;
      if (ActiveVirtualSource(QDC_ONLY_ACTIVE_PATHS) && ActiveVirtualSource(QDC_DATABASE_CURRENT)) {
        confirmed = SetEvent(ready) != FALSE;
        break;
      }
      Sleep(100);
    }
    if (confirmed) WaitForMultipleObjects(2, signals, FALSE, INFINITE);
    // Applying the saved configuration also changes the live layout. Always
    // restore it first, then put the user's transient desktop back on top.
    for (unsigned i = 0;; ++i) {
      if ((database.Matches(QDC_DATABASE_CURRENT) || database.Apply(true)) &&
          (active.Matches(QDC_ONLY_ACTIVE_PATHS) || active.Apply(false))) {
        Sleep(350);
        if (active.Matches(QDC_ONLY_ACTIVE_PATHS) && database.Matches(QDC_DATABASE_CURRENT)) {
          LOG_INFO("Tablet display: original active and saved layouts restored");
          return 0;
        }
      }
      // Never run SDC_TOPOLOGY_EXTEND while all original physical monitors are
      // still attached: temporary lock-screen denials or slow DPMS wakeups
      // would otherwise scramble primary/secondary assignment and coordinates.
      // Only fall back to available physical monitors when a monitor was
      // genuinely unplugged, and never overwrite the saved display database.
      if (i >= 9 && MissingPhysicalTarget(active)) {
        SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        DetachLiveVirtual();
        Layout available;
        if (available.Read(QDC_ONLY_ACTIVE_PATHS) &&
            KeepPhysicalPaths(available.paths, available.modes) &&
            available.Apply(false) && available.Matches(QDC_ONLY_ACTIVE_PATHS)) {
          LOG_WARN("Tablet display: restored available physical outputs after monitor hotplug (preserved saved DB)");
          return 0;
        }
      }
      if (i == 2 || i % 60 == 59) LOG_WARN("Tablet display: retrying display layout restoration");
      Sleep(i < 5 ? 350 : 2000);
    }
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
  static constexpr UINT32 kHighWidth = 2400;
  static constexpr UINT32 kHighHeight = 1696;
  static constexpr UINT32 kBalancedWidth = 2016;
  static constexpr UINT32 kBalancedHeight = 1424;
  static constexpr UINT32 kCompactWidth = 1650;
  static constexpr UINT32 kCompactHeight = 1166;
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
      DISPLAYCONFIG_TOPOLOGY_ID topology{};
      const LONG status = QueryDisplayConfig(flags, &count, paths->data(),
                                             &mode_count, modes->data(),
                                             flags == QDC_DATABASE_CURRENT ? &topology : nullptr);
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

  struct Layout {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;

    bool Read(UINT32 flags) { return ReadTopology(flags, &paths, &modes); }
    LONG Set(UINT32 flags) {
      return SetDisplayConfig(static_cast<UINT32>(paths.size()), paths.data(),
                              static_cast<UINT32>(modes.size()), modes.data(), kConfig | flags);
    }
    bool Validate() { return Set(SDC_VALIDATE) == ERROR_SUCCESS; }
    bool Apply(bool persist) { return Set(SDC_APPLY | (persist ? SDC_SAVE_TO_DATABASE : 0)) == ERROR_SUCCESS; }

    bool Matches(UINT32 query) const {
      Layout actual;
      if (!actual.Read(query) || actual.paths.size() != paths.size()) return false;
      for (const auto& path : paths) {
        auto found = std::find_if(actual.paths.begin(), actual.paths.end(), [&](const auto& p) {
          return SameTarget(p, path);
        });
        if (found == actual.paths.end() || found->targetInfo.rotation != path.targetInfo.rotation ||
            path.sourceInfo.modeInfoIdx >= modes.size() ||
            found->sourceInfo.modeInfoIdx >= actual.modes.size()) return false;
        const auto& rate = path.targetInfo.refreshRate;
        const auto& actual_rate = found->targetInfo.refreshRate;
        if (rate.Numerator && rate.Denominator && actual_rate.Numerator && actual_rate.Denominator &&
            uint64_t(rate.Numerator) * actual_rate.Denominator !=
                uint64_t(actual_rate.Numerator) * rate.Denominator) return false;
        if (modes[path.sourceInfo.modeInfoIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            actual.modes[found->sourceInfo.modeInfoIdx].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
          return false;
        const auto& a = modes[path.sourceInfo.modeInfoIdx].sourceMode;
        const auto& b = actual.modes[found->sourceInfo.modeInfoIdx].sourceMode;
        if (a.width != b.width || a.height != b.height ||
            a.position.x != b.position.x || a.position.y != b.position.y) return false;
      }
      return true;
    }
  };

  static bool SameTarget(const DISPLAYCONFIG_PATH_INFO& a, const DISPLAYCONFIG_PATH_INFO& b) {
    return a.targetInfo.id == b.targetInfo.id &&
           a.targetInfo.adapterId.HighPart == b.targetInfo.adapterId.HighPart &&
           a.targetInfo.adapterId.LowPart == b.targetInfo.adapterId.LowPart;
  }
  static bool MissingPhysicalTarget(const Layout& before) {
    Layout all;
    if (!all.Read(QDC_ALL_PATHS)) return false;
    return std::any_of(before.paths.begin(), before.paths.end(), [&](const auto& path) {
      return !IsMttVdd(path) && std::none_of(all.paths.begin(), all.paths.end(), [&](const auto& p) {
        return p.targetInfo.targetAvailable && SameTarget(p, path);
      });
    });
  }

  static bool ApplyVirtualTopology(UINT32 preferred_width = 0,
                                   UINT32 preferred_height = 0) {
    auto target = FindVirtualPath();
    if (!target) return false;
    target->flags |= DISPLAYCONFIG_PATH_ACTIVE;
    target->sourceInfo.modeInfoIdx = 0;
    target->targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    DISPLAYCONFIG_MODE_INFO mode{};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.id = target->sourceInfo.id;
    mode.adapterId = target->sourceInfo.adapterId;
    // Prefer the controller's remembered/active resolution when supplied so
    // MttVDD boots directly into it without a 1650->target double switch or
    // fallback reset on restart; otherwise honor tablet-display.ini or 1650x1166.
    if (preferred_width >= 640 && preferred_height >= 480 &&
        preferred_width <= 32768 && preferred_height <= 32768) {
      mode.sourceMode.width = preferred_width;
      mode.sourceMode.height = preferred_height;
    } else {
      wchar_t module[32768]{};
      DWORD length = GetModuleFileNameW(nullptr, module, 32768);
      auto config = std::filesystem::path(module).parent_path() / L"tablet-display.ini";
      const UINT32 configured_width =
          length ? GetPrivateProfileIntW(L"tablet", L"width", kCompactWidth, config.c_str())
                 : kCompactWidth;
      if (configured_width == kWidth) {
        mode.sourceMode.width = kWidth;
        mode.sourceMode.height = kHeight;
      } else if (configured_width == kHighWidth) {
        mode.sourceMode.width = kHighWidth;
        mode.sourceMode.height = kHighHeight;
      } else if (configured_width == kBalancedWidth) {
        mode.sourceMode.width = kBalancedWidth;
        mode.sourceMode.height = kBalancedHeight;
      } else {
        mode.sourceMode.width = kCompactWidth;
        mode.sourceMode.height = kCompactHeight;
      }
    }
    mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    Layout exclusive{{*target}, {mode}};
    auto status = exclusive.Set(SDC_VALIDATE);
    if (status != ERROR_SUCCESS && mode.sourceMode.width == kWidth) {
      LOG_WARN("Tablet display: {}x{} validation failed (status={}), falling back to {}x{}",
               mode.sourceMode.width, mode.sourceMode.height, status,
               kHighWidth, kHighHeight);
      mode.sourceMode.width = kHighWidth;
      mode.sourceMode.height = kHighHeight;
      exclusive = Layout{{*target}, {mode}};
      status = exclusive.Set(SDC_VALIDATE);
    }
    if (status != ERROR_SUCCESS &&
        (mode.sourceMode.width != kCompactWidth ||
         mode.sourceMode.height != kCompactHeight)) {
      LOG_WARN("Tablet display: {}x{} validation failed (status={}), falling back to {}x{}",
               mode.sourceMode.width, mode.sourceMode.height, status,
               kCompactWidth, kCompactHeight);
      mode.sourceMode.width = kCompactWidth;
      mode.sourceMode.height = kCompactHeight;
      exclusive = Layout{{*target}, {mode}};
      status = exclusive.Set(SDC_VALIDATE);
    }
    if (status == ERROR_SUCCESS) status = exclusive.Set(SDC_APPLY | SDC_SAVE_TO_DATABASE);
    if (status != ERROR_SUCCESS) {
      LOG_WARN("Tablet display: SetDisplayConfig failed, status={}", status);
      return false;
    }
    PromoteVirtualRefreshRate(SourceName(*target), mode.sourceMode.width,
                              mode.sourceMode.height, 90);
    LOG_INFO("Tablet display: applied {}x{} exclusive mode",
             mode.sourceMode.width, mode.sourceMode.height);
    return true;
  }

  static void PromoteVirtualRefreshRate(const std::wstring& device,
                                        UINT32 width, UINT32 height,
                                        DWORD preferred_hz) {
    if (device.empty()) return;
    DEVMODEW current{};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &current, 0))
      return;
    DEVMODEW best{};
    DWORD best_hz = current.dmDisplayFrequency;
    bool exact = (current.dmDisplayFrequency == preferred_hz);
    for (DWORD i = 0;; ++i) {
      DEVMODEW dm{};
      dm.dmSize = sizeof(dm);
      if (!EnumDisplaySettingsExW(device.c_str(), i, &dm, 0)) break;
      if (dm.dmPelsWidth != width || dm.dmPelsHeight != height ||
          dm.dmBitsPerPel != 32)
        continue;
      if (dm.dmDisplayFrequency == preferred_hz) {
        best = dm;
        best_hz = dm.dmDisplayFrequency;
        exact = true;
        break;
      }
      if (!exact && dm.dmDisplayFrequency > best_hz &&
          dm.dmDisplayFrequency <= 120) {
        best = dm;
        best_hz = dm.dmDisplayFrequency;
      }
    }
    if (best_hz <= current.dmDisplayFrequency || best.dmSize == 0) return;
    best.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    const LONG rc = ChangeDisplaySettingsExW(device.c_str(), &best, nullptr,
                                             CDS_UPDATEREGISTRY, nullptr);
    if (rc == DISP_CHANGE_SUCCESSFUL) {
      LOG_INFO("Tablet display: promoted {}x{} refresh rate {} -> {} Hz",
               width, height, current.dmDisplayFrequency, best_hz);
    } else {
      LOG_WARN("Tablet display: failed to promote refresh rate to {} Hz (rc={})",
               best_hz, rc);
    }
  }

  static std::optional<DISPLAYCONFIG_PATH_INFO> ActiveVirtualSource(UINT32 query = QDC_ONLY_ACTIVE_PATHS) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(query, &paths, &modes) || paths.size() != 1) return {};
    const auto target = std::find_if(paths.begin(), paths.end(), IsMttVdd);
    if (target == paths.end() || std::any_of(std::next(target), paths.end(), IsMttVdd))
      return {};
    const auto index = target->sourceInfo.modeInfoIdx;
    if (index >= modes.size() || modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
      return {};
    const auto& mode = modes[index].sourceMode;
    if (mode.width < 640 || mode.height < 480 ||
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
    missing_output_polls_ = 0;
    next_probe_ = GetTickCount64() + 2000;
    Restore();
    return false;
  }

  static bool DetachLiveVirtual() {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!ReadTopology(QDC_ONLY_ACTIVE_PATHS, &paths, &modes)) return false;
    const auto count = paths.size();
    if (!KeepPhysicalPaths(paths, modes)) return false;
    return count == paths.size() || ApplyPhysicalTopology(paths, modes);
  }

  TabletDisplayProcess worker_;
  bool stopping_ = false;
  ULONGLONG next_probe_ = 0;
  ULONGLONG deadline_ = 0;
  unsigned missing_output_polls_ = 0;
  UINT32 preferred_width_ = 0;
  UINT32 preferred_height_ = 0;
  std::string capture_display_;
};

}  // namespace crossdesk

#endif
