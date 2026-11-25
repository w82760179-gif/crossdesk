#pragma once

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../privacy/privacy_cursor_process.h"

namespace crossdesk {

// The helper owns the display lease and both snapshots. A signaled stop event
// or capture host termination releases the remote layout independently of GUI cleanup.
class TabletDisplayProcess {
 public:
  TabletDisplayProcess() = default;
  TabletDisplayProcess(const TabletDisplayProcess&) = delete;
  TabletDisplayProcess& operator=(const TabletDisplayProcess&) = delete;
  ~TabletDisplayProcess() {
    if (stop_.value) SetEvent(stop_.value);
  }

  bool started() const { return process_.value != nullptr; }
  bool running() const {
    return started() && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT;
  }
  bool ready() const {
    return running() && WaitForSingleObject(ready_.value, 0) == WAIT_OBJECT_0;
  }

  bool Start(UINT32 preferred_width = 0, UINT32 preferred_height = 0) {
    if (started()) return false;
    std::vector<wchar_t> module(32768);
    DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) return false;
    auto helper = std::filesystem::path(module.data()).parent_path() / L"crossdesk_session_helper.exe";
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle parent;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(),
                         &parent.value, SYNCHRONIZE, TRUE, 0)) return false;
    stop_.reset(CreateEventW(&sa, TRUE, FALSE, nullptr));
    ready_.reset(CreateEventW(&sa, TRUE, FALSE, nullptr));
    if (!stop_.value || !ready_.value) return false;

    HANDLE handles[] = {parent.value, stop_.value, ready_.value};
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) return false;
    bool ok = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                    handles, sizeof(handles), nullptr, nullptr) != FALSE;
    PROCESS_INFORMATION child{};
    if (ok) {
      std::wstring args = L"\"" + helper.wstring() + L"\" --tablet-display";
      for (auto handle : handles)
        args += L" " + std::to_wstring(reinterpret_cast<uintptr_t>(handle));
      if (preferred_width >= 640 && preferred_height >= 480 &&
          preferred_width <= 32768 && preferred_height <= 32768) {
        args += L" " + std::to_wstring(preferred_width) + L" " +
                std::to_wstring(preferred_height);
      }
      bool fallback = false;
      ok = CreatePrivacyCursorProcess(helper.c_str(), args, startup, child, fallback);
    }
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!ok) return false;
    CloseHandle(child.hThread);
    process_.reset(child.hProcess);
    return true;
  }

  bool Stop(DWORD wait_ms = 0) {
    if (!started()) return true;
    SetEvent(stop_.value);
    if (WaitForSingleObject(process_.value, wait_ms) != WAIT_OBJECT_0) return false;
    process_.reset();
    ready_.reset();
    stop_.reset();
    return true;
  }

 private:
  struct Handle {
    HANDLE value = nullptr;
    void reset(HANDLE next = nullptr) {
      if (value) CloseHandle(value);
      value = next;
    }
    ~Handle() { reset(); }
  };
  Handle process_, stop_, ready_;
};

}  // namespace crossdesk
