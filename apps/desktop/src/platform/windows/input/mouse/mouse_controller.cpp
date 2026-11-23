#include "mouse_controller.h"

#include <Windows.h>
#include <remote_action.h>

#include <algorithm>
#include <cmath>

#include "rd_log.h"
#include "windows_input_injector.h"
#include "windows_input_marker.h"
#include "windows_thread_dpi.h"

namespace crossdesk {

PlatformMouseController::PlatformMouseController() {}

PlatformMouseController::~PlatformMouseController() { Destroy(); }

int PlatformMouseController::Init(std::vector<DisplayInfo> display_info_list) {
  std::lock_guard<std::mutex> lock(display_info_mutex_);
  display_info_list_ = display_info_list;

  return 0;
}

void PlatformMouseController::UpdateDisplayInfoList(
    const std::vector<DisplayInfo>& display_info_list) {
  if (display_info_list.empty()) return;
  std::lock_guard<std::mutex> lock(display_info_mutex_);
  if (std::equal(display_info_list.begin(), display_info_list.end(),
                 display_info_list_.begin(), display_info_list_.end(),
                 [](const auto& a, const auto& b) {
                   return a.handle == b.handle && a.left == b.left &&
                          a.top == b.top && a.width == b.width &&
                          a.height == b.height;
                 })) return;
  display_info_list_ = display_info_list;
  LOG_INFO("Windows mouse display geometry updated: displays={}",
           display_info_list_.size());
}

int PlatformMouseController::Destroy() {
  CancelTouchInputs();
  return ReleasePressedButtons() == 7 ? 0 : -1;
}

void PlatformMouseController::CancelTouchInputs() {
  std::lock_guard<std::mutex> lock(touch_mutex_);
  if (!touch_initialized_ || active_touch_points_.empty()) {
    active_touch_points_.clear();
    return;
  }
  ScopedWindowsPhysicalCoordinates physical_coordinates;
  std::vector<POINTER_TOUCH_INFO> contacts;
  contacts.reserve(active_touch_points_.size());
  for (const auto& [id, pt] : active_touch_points_) {
    POINTER_TOUCH_INFO info{};
    info.pointerInfo.pointerType = PT_TOUCH;
    info.pointerInfo.pointerId = id;
    info.pointerInfo.ptPixelLocation = pt;
    info.pointerInfo.pointerFlags = POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    info.touchFlags = TOUCH_FLAG_NONE;
    info.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    info.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
    info.orientation = 90;
    info.pressure = 32000;
    contacts.push_back(info);
  }
  InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data());
  active_touch_points_.clear();
}

int PlatformMouseController::SendTouchCommand(const RemoteAction& remote_action,
                                              int display_index) {
  if (remote_action.type != ControlType::touch) return 0;
  std::unique_lock<std::mutex> geometry_lock(display_info_mutex_);
  if (display_info_list_.empty()) return -1;
  if (display_index < 0 ||
      display_index >= static_cast<int>(display_info_list_.size())) {
    display_index = 0;
  }
  const auto display = display_info_list_[display_index];
  geometry_lock.unlock();
  if (display.width <= 0 || display.height <= 0) return -1;

  std::lock_guard<std::mutex> lock(touch_mutex_);
  ScopedWindowsPhysicalCoordinates physical_coordinates;
  if (!touch_initialized_) {
    if (!InitializeTouchInjection(10, TOUCH_FEEDBACK_DEFAULT)) {
      LOG_WARN("InitializeTouchInjection failed, error={}", GetLastError());
      return -1;
    }
    touch_initialized_ = true;
  }

  auto make_info = [](uint32_t id, POINT p, POINTER_FLAGS flags) {
    POINTER_TOUCH_INFO info{};
    info.pointerInfo.pointerType = PT_TOUCH;
    info.pointerInfo.pointerId = id;
    info.pointerInfo.ptPixelLocation = p;
    info.pointerInfo.pointerFlags = flags;
    info.touchFlags = TOUCH_FLAG_NONE;
    info.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    info.rcContact = {p.x - 2, p.y - 2, p.x + 2, p.y + 2};
    info.orientation = 90;
    info.pressure = 32000;
    return info;
  };

  if (remote_action.touch_points.empty()) {
    if (active_touch_points_.empty()) return 0;
    std::vector<POINTER_TOUCH_INFO> cancel_contacts;
    cancel_contacts.reserve(active_touch_points_.size());
    for (const auto& [id, pt] : active_touch_points_) {
      cancel_contacts.push_back(make_info(id, pt, POINTER_FLAG_UP | POINTER_FLAG_CANCELED));
    }
    InjectTouchInput(static_cast<UINT32>(cancel_contacts.size()), cancel_contacts.data());
    active_touch_points_.clear();
    return 0;
  }

  std::unordered_map<uint32_t, RemoteAction::TouchPoint> incoming;
  for (const auto& pt : remote_action.touch_points) incoming[pt.id] = pt;

  std::vector<POINTER_TOUCH_INFO> contacts;
  contacts.reserve(10);
  std::unordered_map<uint32_t, POINT> next_active;

  for (const auto& [id, pt] : incoming) {
    if (contacts.size() >= 10) break;
    POINT px{};
    px.x = static_cast<LONG>(std::clamp(pt.x, 0.0f, 1.0f) * (display.width - 1)) + display.left;
    px.y = static_cast<LONG>(std::clamp(pt.y, 0.0f, 1.0f) * (display.height - 1)) + display.top;
    const bool was_active = active_touch_points_.count(id) != 0;
    if (pt.phase == 0 || pt.phase == 1) {
      POINTER_FLAGS flags = was_active
          ? (POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT)
          : (POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
      contacts.push_back(make_info(id, px, flags));
      next_active[id] = px;
    } else if (pt.phase == 2 || pt.phase == 3) {
      if (was_active) {
        POINTER_FLAGS flags = pt.phase == 3
            ? (POINTER_FLAG_UP | POINTER_FLAG_CANCELED)
            : POINTER_FLAG_UP;
        contacts.push_back(make_info(id, px, flags));
      }
    }
  }

  for (const auto& [id, prev_pt] : active_touch_points_) {
    if (incoming.count(id) == 0 && contacts.size() < 10) {
      contacts.push_back(make_info(id, prev_pt, POINTER_FLAG_UP | POINTER_FLAG_CANCELED));
    }
  }

  if (contacts.empty()) {
    active_touch_points_.clear();
    return 0;
  }

  if (!InjectTouchInput(static_cast<UINT32>(contacts.size()), contacts.data())) {
    const DWORD err = GetLastError();
    LOG_WARN("InjectTouchInput failed, count={}, err={}", contacts.size(), err);
    active_touch_points_.clear();
    return -1;
  }
  active_touch_points_ = std::move(next_active);
  return 0;
}

unsigned PlatformMouseController::ReleasePressedButtons(unsigned mask) {
  mask &= 7;
  const unsigned pressed = pressed_buttons_.load() & mask;
  if (!pressed) return mask;
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwExtraInfo = kInjectedMouseInputMarker;
  if (pressed & 1) input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
  if (pressed & 2) input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
  if (pressed & 4) input.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
  if (SendInputOnUserDesktop(input) != 1) {
    LOG_WARN("Release remote mouse buttons failed, error={}", GetLastError());
    return mask & ~pressed;
  }
  pressed_buttons_.fetch_and(~pressed);
  return mask;
}

int PlatformMouseController::SendMouseCommand(RemoteAction remote_action,
                                              int display_index) {
  std::unique_lock<std::mutex> geometry_lock(display_info_mutex_);
  if (display_index < 0 ||
      display_index >= static_cast<int>(display_info_list_.size()) ||
      display_info_list_[display_index].width <= 0 ||
      display_info_list_[display_index].height <= 0) {
    LOG_WARN("Mouse command skipped, invalid display_index={}, displays={}",
             display_index, display_info_list_.size());
    return -1;
  }

  const auto display = display_info_list_[display_index];
  geometry_lock.unlock();

  INPUT ip = {0};
  ScopedWindowsPhysicalCoordinates physical_coordinates;

  if (remote_action.type != ControlType::mouse) return 0;
  if (!std::isfinite(remote_action.m.x) || !std::isfinite(remote_action.m.y))
    return -1;
  ip.type = INPUT_MOUSE;
  ip.mi.dx = (LONG)(std::clamp(remote_action.m.x, 0.0f, 1.0f) *
                    (display.width - 1)) + display.left;
  ip.mi.dy = (LONG)(std::clamp(remote_action.m.y, 0.0f, 1.0f) *
                    (display.height - 1)) + display.top;

  switch (remote_action.m.flag) {
    case MouseFlag::left_down:
      ip.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
      break;
    case MouseFlag::left_up:
      ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
      break;
    case MouseFlag::right_down:
      ip.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
      break;
    case MouseFlag::right_up:
      ip.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
      break;
    case MouseFlag::middle_down:
      ip.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
      break;
    case MouseFlag::middle_up:
      ip.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
      break;
    case MouseFlag::wheel_vertical:
      ip.mi.dwFlags = MOUSEEVENTF_WHEEL;
      ip.mi.mouseData = remote_action.m.s * 120;
      break;
    case MouseFlag::wheel_horizontal:
      ip.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      ip.mi.mouseData = remote_action.m.s * 120;
      break;
    default:
      break;
  }

  const int virtual_left = GetSystemMetrics(SM_XVIRTUALSCREEN);
  const int virtual_top = GetSystemMetrics(SM_YVIRTUALSCREEN);
  const int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  const int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (virtual_width <= 0 || virtual_height <= 0) {
    LOG_WARN("Remote mouse skipped: invalid virtual desktop geometry");
    return -1;
  }
  // Pixel centres in the virtual desktop, including negative monitor origins.
  // SetCursorPos cannot carry dwExtraInfo through the privacy mouse hook.
  const int virtual_width_range = (std::max)(1, virtual_width - 1);
  const int virtual_height_range = (std::max)(1, virtual_height - 1);
  ip.mi.dx = static_cast<LONG>(std::clamp<int64_t>(
      (static_cast<int64_t>(ip.mi.dx - virtual_left) * 65535 +
       virtual_width_range / 2) /
          virtual_width_range,
      0, 65535));
  ip.mi.dy = static_cast<LONG>(std::clamp<int64_t>(
      (static_cast<int64_t>(ip.mi.dy - virtual_top) * 65535 +
       virtual_height_range / 2) /
          virtual_height_range,
      0, 65535));
  ip.mi.dwFlags |=
      MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
  ip.mi.dwExtraInfo = kInjectedMouseInputMarker;
  const UINT sent = SendInputOnUserDesktop(ip);
  if (sent != 1) {
    const DWORD error = GetLastError();
    LOG_WARN("SendInput failed for mouse x={}, y={}, wheel={}, flag={}, err={}",
             ip.mi.dx, ip.mi.dy, remote_action.m.s,
             static_cast<int>(remote_action.m.flag), error);
    SetLastError(error);
    return -1;
  }
  if (ip.mi.dwFlags & MOUSEEVENTF_LEFTDOWN) pressed_buttons_.fetch_or(1);
  if (ip.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN) pressed_buttons_.fetch_or(2);
  if (ip.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN) pressed_buttons_.fetch_or(4);
  if (ip.mi.dwFlags & MOUSEEVENTF_LEFTUP) pressed_buttons_.fetch_and(~1u);
  if (ip.mi.dwFlags & MOUSEEVENTF_RIGHTUP) pressed_buttons_.fetch_and(~2u);
  if (ip.mi.dwFlags & MOUSEEVENTF_MIDDLEUP) pressed_buttons_.fetch_and(~4u);

  return 0;
}
}  // namespace crossdesk
