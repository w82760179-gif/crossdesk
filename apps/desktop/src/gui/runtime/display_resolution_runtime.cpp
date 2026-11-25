#include "runtime/gui_runtime.h"

#include <SDL3/SDL_timer.h>

#include <unordered_map>

#include "rd_log.h"

#ifdef _WIN32
#include "interactive_desktop.h"
#include "platform/windows/screen_capturer/display_resolution_win.h"
#include "platform/windows/screen_capturer/single_display_win.h"
#elif defined(__APPLE__)
#include "platform/macos/screen_capturer/display_resolution_mac.h"
#elif defined(__linux__) && !defined(__OHOS__)
#include "platform/linux/screen_capturer/display_resolution_linux.h"
#endif

namespace crossdesk {
namespace {
// Serialize native queries on transport threads with mode changes on the
// application thread (EnumDisplaySettings also caches its enumeration).
std::mutex native_display_resolution_mutex;
#ifdef _WIN32
WindowsSingleDisplayGuard windows_single_display_guard;
std::unordered_map<std::string, uint64_t> virtual_startup_retry_deadlines;

std::vector<DisplayInfo> EffectiveWindowsDisplayList(
    const std::vector<DisplayInfo> &list,
    const std::string &capture_display) {
  if (!capture_display.empty() &&
      (list.empty() || list[0].name != capture_display)) {
    return {DisplayInfo(nullptr, capture_display, true, 0, 0, 1, 1)};
  }
  return list;
}

bool MatchesVirtualDisplayKey(const std::string &candidate,
                              const std::string &remembered) {
  if (candidate == remembered) return true;
  const auto sep = remembered.find('|');
  if (sep == std::string::npos || sep + 1 >= remembered.size()) return false;
  const auto suffix = remembered.substr(sep);
  return candidate.size() >= suffix.size() &&
         candidate.compare(candidate.size() - suffix.size(), suffix.size(),
                           suffix) == 0;
}
#endif
} // namespace

std::vector<DisplayResolutionInfo> GuiRuntime::QueryHostDisplayResolutions() {
#ifdef _WIN32
  std::lock_guard lock(native_display_resolution_mutex);
  return QueryWindowsDisplayResolutions(EffectiveWindowsDisplayList(
      devices_.display_info_list(),
      windows_single_display_guard.capture_display()));
#elif defined(__APPLE__)
  std::lock_guard lock(native_display_resolution_mutex);
  return QueryMacDisplayResolutions(devices_.display_info_list());
#elif defined(__linux__) && !defined(__OHOS__)
  std::lock_guard lock(native_display_resolution_mutex);
  return QueryLinuxDisplayResolutions(devices_.display_info_list());
#else
  return {};
#endif
}

void GuiRuntime::QueueDisplayResolutionChange(
    const std::string &id, PeerEventHandler *source,
    const DisplayResolutionChange &change) {
  const auto epoch = display_resolution_epoch_.load();
  const auto events = ControllerEvents(id);
  if (events.get() != source || !IsAuthorizedController(id))
    return;
  std::lock_guard lock(display_resolution_commands_mutex_);
  if (display_resolution_commands_.size() >= 32)
    return;
  display_resolution_commands_.push_back({id, events, change, epoch});
}

void GuiRuntime::BeginDisplayResolutionStartup(const std::string &id,
                                               PeerEventHandler *source) {
  const auto events = ControllerEvents(id);
  if (events.get() != source) return;
  std::lock_guard lock(display_resolution_commands_mutex_);
  auto &initial = initial_display_resolutions_[id];
  if (initial.source.lock() != events) initial = {};
  initial.source = events;
  if (!initial.state.connected)
    initial.generation = ++next_display_resolution_startup_;
  initial.state.Begin(SDL_GetTicks());
}

void GuiRuntime::ReceiveDisplayResolutionStartup(
    const std::string &id, PeerEventHandler *source,
    std::vector<DisplayResolutionPreference> preferences) {
  const auto events = ControllerEvents(id);
  if (events.get() != source) return;
  std::lock_guard lock(display_resolution_commands_mutex_);
  auto &initial = initial_display_resolutions_[id];
  if (initial.source.lock() != events) initial = {};
  initial.source = events;
  if (!preferences.empty()) {
    LOG_INFO("Received {} remembered display resolution preference(s) from {}",
             preferences.size(), id);
  }
  initial.state.Receive(std::move(preferences));
}

bool GuiRuntime::DisplayResolutionsReadyForCapture(const std::string &id) {
  std::lock_guard lock(display_resolution_commands_mutex_);
  const auto now = SDL_GetTicks();
  if (!id.empty()) {
    const auto found = initial_display_resolutions_.find(id);
    return found == initial_display_resolutions_.end() || found->second.state.Ready(now);
  }
  return std::all_of(initial_display_resolutions_.begin(),
      initial_display_resolutions_.end(), [&](const auto &entry) {
        return entry.second.state.Ready(now);
      });
}

bool GuiRuntime::ApplyHostDisplayResolution(const DisplayResolutionChange &change) {
  const auto available = QueryHostDisplayResolutions();
  const auto selected = std::find_if(available.begin(), available.end(),
      [&](const auto &info) { return SupportsDisplayResolution(info, change); });
  // Unsupported or stale requests must never create a restoration record.
  if (selected == available.end()) return false;
  if (selected->current == change.resolution) return true;
  const auto displays = devices_.display_info_list();
  std::lock_guard lock(native_display_resolution_mutex);
#ifdef _WIN32
  if (!IsCurrentSessionUserDesktopActive()) return false;
  if (!windows_single_display_guard.capture_display().empty()) {
    // Virtual display topology is restored wholesale on disconnect by
    // windows_single_display_guard; never enqueue it into physical restoration.
    const auto effective = EffectiveWindowsDisplayList(
        displays, windows_single_display_guard.capture_display());
    return ApplyWindowsDisplayResolution(effective, change);
  }
  return display_resolution_restoration_.Apply(change.display_key,
      [&] { return CaptureWindowsDisplayResolution(displays, change); },
      [&] { return ApplyWindowsDisplayResolution(displays, change); });
#elif defined(__APPLE__)
  return display_resolution_restoration_.Apply(change.display_key,
      [&] { return CaptureMacDisplayResolution(displays, change); },
      [&] { return ApplyMacDisplayResolution(displays, change); });
#elif defined(__linux__) && !defined(__OHOS__)
  return display_resolution_restoration_.Apply(change.display_key,
      [&] { return CaptureLinuxDisplayResolution(displays, change); },
      [&] { return ApplyLinuxDisplayResolution(displays, change); });
#else
  return false;
#endif
}

void GuiRuntime::RestoreHostDisplayResolutions(bool force) {
  const auto epoch = display_resolution_epoch_.load();
  const bool disconnected = epoch != handled_display_resolution_epoch_;
  handled_display_resolution_epoch_ = epoch;
#ifdef _WIN32
  if (display_resolution_restoration_.Empty() &&
      !windows_single_display_guard.Tracked())
    return;
#else
  if (display_resolution_restoration_.Empty())
    return;
#endif
  const auto now = SDL_GetTicks();
  // A disconnect boundary must survive a rapid reconnect. Otherwise retry
  // failed restoration only while idle, without disrupting an active session.
  if (!disconnected && !force &&
      (now < next_display_resolution_restore_ ||
       !ConnectedControllers().empty()))
    return;
  next_display_resolution_restore_ = now + 2000;
  std::lock_guard lock(native_display_resolution_mutex);
#ifdef _WIN32
  if (!IsCurrentSessionUserDesktopActive()) {
    // The helper retains the snapshots and retries if Windows temporarily
    // denies mode changes on a locked desktop. Signal the disconnect now.
    if (windows_single_display_guard.Tracked() && windows_single_display_guard.Restore())
      devices_.SetWindowsSessionDisplay({});
    return;
  }
#endif
  if (!display_resolution_restoration_.Empty()) {
    if (display_resolution_restoration_.RestoreAll())
      LOG_INFO("Restored original display modes after remote control");
    else if (disconnected || force)
      LOG_WARN("Could not restore every display mode; will retry while idle");
  }
#ifdef _WIN32
  if (windows_single_display_guard.Tracked()) {
    if (windows_single_display_guard.Restore(force ? 5000 : 0)) {
      devices_.SetWindowsSessionDisplay({});
      LOG_INFO("Restored original multi-display topology after remote control");
    } else if (disconnected || force) {
      LOG_WARN("Display topology restoration pending");
    }
  }
#endif
  next_display_resolution_poll_ = 0;
}

#ifdef _WIN32
void GuiRuntime::QueueVirtualDisplayScale(
    const std::string& id, PeerEventHandler* source,
    const RemoteAction::VirtualDisplayScale& request) {
  const auto events = ControllerEvents(id);
  if (events.get() != source || !IsAuthorizedController(id))
    return;
  std::lock_guard lock(display_resolution_commands_mutex_);
  if (display_scaling_commands_.size() < 8)
    display_scaling_commands_.push_back({id, events, request});
}

void GuiRuntime::HandleVirtualDisplayScale() {
  std::deque<PendingDisplayScalingSettings> commands;
  {
    std::lock_guard lock(display_resolution_commands_mutex_);
    commands.swap(display_scaling_commands_);
  }
  for (const auto& command : commands) {
    const auto source = command.source.lock();
    if (stopping_ || !source || !source->IsActive() ||
        source != ControllerEvents(command.remote_id) ||
        !IsAuthorizedController(command.remote_id)) continue;
    RemoteAction reply{};
    reply.type = ControlType::virtual_display_scale_status;
    auto& response = reply.virtual_scale;
    response.request_id = command.request.request_id;
    if (devices_.WindowsTabletDisplayReady() && IsCurrentSessionUserDesktopActive()) {
      std::lock_guard lock(native_display_resolution_mutex);
      if (source->IsActive()) {
        auto result = windows_single_display_guard.DisplayScale(command.request.percent);
        response.percent = result.percent;
        response.accepted = result.accepted;
        response.options = std::move(result.options);
      }
    }
    if (command.request.percent != 0)
      LOG_INFO("Tablet display: scale requested={} actual={} accepted={}",
               command.request.percent, response.percent, response.accepted);
    const auto message = reply.to_json();
    SendControllerData(command.remote_id, message.data(), message.size(),
                       control_data_label_.c_str());
  }
}
#endif

bool GuiRuntime::HandleDisplayResolutionChanges() {
  RestoreHostDisplayResolutions();
#ifdef _WIN32
  HandleVirtualDisplayScale();
  const auto peers = ConnectedControllers();
  const bool owns_capture = !managed_host_frontend_ ||
      std::any_of(peers.begin(), peers.end(), [](const auto& id) {
        return id.rfind("lan/", 0) == 0;
      });
  // The attached GUI mirrors cloud connection state for privacy and audio.
  // Its physical monitor list must never overwrite the capture host's list.
  if (!owns_capture) {
    if (windows_single_display_guard.Tracked())
      RestoreHostDisplayResolutions(true);
    return true;
  }
  if (peers.empty()) service_tablet_driver_.reset();
  if (unattended_host_ && !peers.empty() && !service_tablet_driver_)
    service_tablet_driver_ = WindowsSingleDisplayGuard::DriverPresent();
  const bool tablet = !unattended_host_ || service_tablet_driver_.value_or(false);
  devices_.RequireTabletDisplay(tablet);
  if (!stopping_ && tablet && !peers.empty()) {
    if (!IsCurrentSessionUserDesktopActive()) return false;
    std::lock_guard lock(native_display_resolution_mutex);
    if (!windows_single_display_guard.Update()) return false;
    if (devices_.SetWindowsSessionDisplay(windows_single_display_guard.capture_display())) {
      selected_display_ = 0;
      last_display_resolutions_.clear();
      // The device manager reinitializes the capture list after this tick.
      next_display_resolution_poll_ = SDL_GetTicks() + 100;
    }
  } else if (!stopping_ && peers.empty() && !managed_host_frontend_ &&
             SDL_GetTicks() >= next_idle_virtual_display_check_) {
    next_idle_virtual_display_check_ = SDL_GetTicks() + 2000;
    if (IsCurrentSessionUserDesktopActive()) {
      std::lock_guard lock(native_display_resolution_mutex);
      if (!windows_single_display_guard.Tracked())
        windows_single_display_guard.RemoveIdleVirtualDisplay();
    }
  }
#endif
  std::vector<std::pair<std::string, InitialDisplayResolutions>> initial;
  {
    std::lock_guard lock(display_resolution_commands_mutex_);
    for (auto &[id, startup] : initial_display_resolutions_) {
      if (!startup.state.NeedsApply()) continue;
      initial.emplace_back(id, startup);
    }
  }
  for (const auto &[id, startup] : initial) {
    const auto source = startup.source.lock();
    const auto active = [&] {
      if (stopping_ || !source || !source->IsActive() ||
          source != ControllerEvents(id) || !IsAuthorizedController(id))
        return false;
      std::lock_guard lock(display_resolution_commands_mutex_);
      const auto found = initial_display_resolutions_.find(id);
      return found != initial_display_resolutions_.end() &&
             found->second.generation == startup.generation &&
             found->second.source.lock() == source;
    };
    // Connected and metadata callbacks can interleave with this tick. Keep
    // early preferences pending until admission has actually been committed.
    if (!active()) continue;
    bool use_virtual_match = false;
#ifdef _WIN32
    {
      std::lock_guard lock(native_display_resolution_mutex);
      if (tablet && windows_single_display_guard.capture_display().empty())
        continue;
      use_virtual_match = !windows_single_display_guard.capture_display().empty();
    }
#endif
#ifdef _WIN32
    if (use_virtual_match && !startup.state.preferences.empty()) {
      const auto probe = QueryHostDisplayResolutions();
      if (probe.empty()) {
        const auto now_tick = SDL_GetTicks();
        auto &retry_until = virtual_startup_retry_deadlines[id];
        if (retry_until == 0) retry_until = now_tick + 3000;
        if (now_tick < retry_until) continue;
        LOG_WARN("Remembered virtual display resolution wait timed out with no enumerable displays");
      }
      virtual_startup_retry_deadlines.erase(id);
    }
#endif
    for (const auto &preference : startup.state.preferences) {
      RestoreHostDisplayResolutions();
      if (!active()) break;
      const auto available = QueryHostDisplayResolutions();
      const auto display = std::find_if(available.begin(), available.end(),
          [&](const auto &info) {
            if (info.display_key == preference.display_key) return true;
#ifdef _WIN32
            if (use_virtual_match &&
                MatchesVirtualDisplayKey(info.display_key, preference.display_key))
              return true;
#endif
            return false;
          });
      if (display == available.end()) {
        LOG_INFO("Remembered display '{}' not matched among {} display(s)",
                 preference.display_key, available.size());
        continue;
      }
      // Resolve a fresh slot from the native identity: hotplug can reorder the
      // list, and another display must never inherit the remembered mode.
      const DisplayResolutionChange change{display->display_id,
          display->display_key, preference.resolution};
      if (!ApplyHostDisplayResolution(change)) {
        LOG_INFO("Remembered display resolution {}x{} is unavailable; keeping current mode",
                 preference.resolution.width, preference.resolution.height);
      } else {
        LOG_INFO("Applied remembered display resolution {}x{} on {}",
                 preference.resolution.width, preference.resolution.height,
                 display->display_key);
        last_display_resolutions_.clear();
        next_display_resolution_poll_ = 0;
      }
    }
    RestoreHostDisplayResolutions();
    {
      std::lock_guard lock(display_resolution_commands_mutex_);
      const auto found = initial_display_resolutions_.find(id);
      if (found != initial_display_resolutions_.end() &&
          found->second.source.lock() == source &&
          found->second.generation == startup.generation) {
        found->second.state.applied = true;
        found->second.state.preferences.clear();
      }
    }
    if (active() && !startup.state.preferences.empty())
      AnnounceHostCapabilities(id);
  }
  std::deque<PendingDisplayResolution> commands;
  {
    std::lock_guard lock(display_resolution_commands_mutex_);
    commands.swap(display_resolution_commands_);
  }
  for (const auto &command : commands) {
    RestoreHostDisplayResolutions();
    const auto source = command.source.lock();
    if (stopping_ || command.epoch != display_resolution_epoch_.load() ||
        !source || !source->IsActive() ||
        source != ControllerEvents(command.remote_id) ||
        !IsAuthorizedController(command.remote_id))
      continue;
    RemoteAction reply;
    reply.type = ControlType::display_resolution_status;
    reply.resolution_change = command.change;
    reply.resolution_change.accepted =
        ApplyHostDisplayResolution(command.change);
    reply.display_resolutions = QueryHostDisplayResolutions();
    const auto message = reply.to_json();
    SendControllerData(command.remote_id, message.data(), message.size(),
                       control_data_label_.c_str());
    next_display_resolution_poll_ = 0;
  }
  // The transport can disconnect while a synchronous native mode switch is
  // in progress. Restore that change on this tick as well.
  RestoreHostDisplayResolutions();
  const auto now = SDL_GetTicks();
  if (now < next_display_resolution_poll_)
    return true;
  next_display_resolution_poll_ = now + 2000;
  const auto controllers = ConnectedControllers();
  if (controllers.empty())
    return true;
  auto current = QueryHostDisplayResolutions();
  if (current == last_display_resolutions_)
    return true;
  last_display_resolutions_ = std::move(current);
  // Re-advertise after local changes/hotplug as well as remote requests. The
  // optional host-info extension is harmless to pre-feature controllers.
  const auto message = BuildHostInformation();
  for (const auto &id : controllers) {
    if (IsAuthorizedController(id) && (!managed_host_frontend_ || id.rfind("lan/", 0) == 0))
      SendControllerData(id, message.data(), message.size(),
                         control_data_label_.c_str());
  }
  return true;
}

} // namespace crossdesk
