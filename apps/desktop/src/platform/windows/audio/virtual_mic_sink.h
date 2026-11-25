#ifndef _CROSSDESK_VIRTUAL_MIC_SINK_H_
#define _CROSSDESK_VIRTUAL_MIC_SINK_H_

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rd_log.h"

namespace crossdesk {

// Plays incoming remote-controller microphone PCM (48 kHz mono S16LE) ONLY to a
// virtual audio cable render endpoint (e.g. "CABLE Input (VB-Audio Virtual Cable)").
// Never plays to physical speakers (avoiding office noise and speaker-loopback echo).
// While active, best-effort switches the Windows default capture device to the
// matching virtual microphone (e.g. "CABLE Output") and restores the previous
// default microphone automatically on silence or disconnect.
class WindowsVirtualMicSink {
 public:
  WindowsVirtualMicSink() = default;
  ~WindowsVirtualMicSink() { Shutdown(); }

  WindowsVirtualMicSink(const WindowsVirtualMicSink&) = delete;
  WindowsVirtualMicSink& operator=(const WindowsVirtualMicSink&) = delete;

  void Push(const char* data, size_t size) {
    if (!data || size == 0 || (size % 2) != 0 || size > 19200) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    constexpr size_t kMaxBufferedBytes = 19200;  // 200 ms @ 48 kHz mono S16
    if (buffer_.size() + size > kMaxBufferedBytes) {
      size_t drop = (buffer_.size() + size) - kMaxBufferedBytes;
      if (drop % 2 != 0) ++drop;
      if (drop >= buffer_.size()) {
        buffer_.clear();
      } else {
        buffer_.erase(buffer_.begin(),
                      buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
      }
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
    last_push_ms_ = GetTickCount64();
    reset_stream_ = false;
    if (!worker_.joinable()) {
      worker_ = std::thread(&WindowsVirtualMicSink::Run, this);
    }
    cv_.notify_one();
  }

  void Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.clear();
    reset_stream_ = true;
    cv_.notify_one();
  }

  void Shutdown() {
    std::thread to_join;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
      reset_stream_ = true;
      buffer_.clear();
      if (worker_.joinable()) {
        to_join = std::move(worker_);
      }
      cv_.notify_all();
    }
    if (to_join.joinable()) {
      to_join.join();
    }
  }

 private:
  struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8"))
      IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT,
                                                      WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*,
                                                      WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64,
                                                          PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR,
                                                       const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR,
                                                       const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId,
                                                         ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
  };

  static constexpr CLSID kClsidPolicyConfigClient = {
      0x870af99c,
      0x171d,
      0x4f9e,
      {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

  static constexpr PROPERTYKEY kPkeyDeviceFriendlyName = {
      {0xa45c254e,
       0xdf1c,
       0x4efd,
       {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      14};

  static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
  }

  static std::string NarrowAsciiOrUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                           static_cast<int>(w.size()), nullptr,
                                           0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
  }

  static std::wstring ReadConfigString(const wchar_t* key,
                                       const wchar_t* fallback) {
    wchar_t module[32768]{};
    const DWORD len = GetModuleFileNameW(nullptr, module, 32768);
    if (!len) return fallback;
    const auto ini = std::filesystem::path(module).parent_path() / L"virtual-mic.ini";
    wchar_t value[256]{};
    GetPrivateProfileStringW(L"mic", key, fallback, value, 256, ini.c_str());
    return value[0] ? std::wstring(value) : std::wstring(fallback);
  }

  static std::wstring GetDeviceFriendlyName(IMMDevice* device) {
    if (!device) return {};
    IPropertyStore* store = nullptr;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store)) || !store) return {};
    PROPVARIANT var;
    PropVariantInit(&var);
    std::wstring name;
    if (SUCCEEDED(store->GetValue(kPkeyDeviceFriendlyName, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
      name = var.pwszVal;
    }
    PropVariantClear(&var);
    store->Release();
    return name;
  }

  static std::wstring GetDeviceIdString(IMMDevice* device) {
    if (!device) return {};
    LPWSTR raw = nullptr;
    if (FAILED(device->GetId(&raw)) || !raw) return {};
    std::wstring id(raw);
    CoTaskMemFree(raw);
    return id;
  }

  static IMMDevice* FindMatchingDevice(IMMDeviceEnumerator* enumerator,
                                       EDataFlow flow,
                                       const std::wstring& needle,
                                       std::wstring* matched_name,
                                       std::wstring* matched_id) {
    if (!enumerator || needle.empty()) return nullptr;
    IMMDeviceCollection* collection = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE,
                                              &collection)) ||
        !collection) {
      return nullptr;
    }
    const std::wstring lower_needle = ToLower(needle);
    UINT count = 0;
    collection->GetCount(&count);
    IMMDevice* result = nullptr;
    for (UINT i = 0; i < count; ++i) {
      IMMDevice* dev = nullptr;
      if (FAILED(collection->Item(i, &dev)) || !dev) continue;
      const std::wstring name = GetDeviceFriendlyName(dev);
      if (ToLower(name).find(lower_needle) != std::wstring::npos) {
        if (matched_name) *matched_name = name;
        if (matched_id) *matched_id = GetDeviceIdString(dev);
        result = dev;
        break;
      }
      dev->Release();
    }
    collection->Release();
    return result;
  }

  static bool SetDefaultCaptureEndpoint(const std::wstring& device_id) {
    if (device_id.empty()) return false;
    IPolicyConfig* policy = nullptr;
    HRESULT hr = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig),
                                  reinterpret_cast<void**>(&policy));
    if (FAILED(hr) || !policy) return false;
    const HRESULT hr1 = policy->SetDefaultEndpoint(device_id.c_str(), eConsole);
    const HRESULT hr2 =
        policy->SetDefaultEndpoint(device_id.c_str(), eCommunications);
    policy->Release();
    return SUCCEEDED(hr1) || SUCCEEDED(hr2);
  }

  static std::wstring GetDefaultCaptureEndpointId(
      IMMDeviceEnumerator* enumerator) {
    if (!enumerator) return {};
    IMMDevice* dev = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &dev)) ||
        !dev) {
      return {};
    }
    std::wstring id = GetDeviceIdString(dev);
    dev->Release();
    return id;
  }

  void Run() {
    const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ULONGLONG last_missing_log_ms = 0;
    ULONGLONG next_probe_ms = 0;

    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return shutdown_ || !buffer_.empty(); });
        if (shutdown_) break;
      }

      const ULONGLONG now = GetTickCount64();
      if (now < next_probe_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        buffer_.clear();
        cv_.wait_for(lock, std::chrono::milliseconds(100),
                     [&] { return shutdown_; });
        continue;
      }

      IMMDeviceEnumerator* enumerator = nullptr;
      HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                    reinterpret_cast<void**>(&enumerator));
      if (FAILED(hr) || !enumerator) {
        next_probe_ms = GetTickCount64() + 5000;
        continue;
      }

      const std::wstring render_needle =
          ReadConfigString(L"render_device", L"CABLE Input");
      const std::wstring capture_needle =
          ReadConfigString(L"capture_device", L"CABLE Output");

      std::wstring render_name;
      IMMDevice* render_dev = FindMatchingDevice(
          enumerator, eRender, render_needle, &render_name, nullptr);
      if (!render_dev) {
        if (last_missing_log_ms == 0 || now - last_missing_log_ms >= 30000) {
          LOG_WARN(
              "Virtual mic sink: no active virtual audio cable render device "
              "matching '{}' found (install VB-Audio Virtual Cable to enable "
              "remote microphone)",
              NarrowAsciiOrUtf8(render_needle));
          last_missing_log_ms = now;
        }
        enumerator->Release();
        next_probe_ms = GetTickCount64() + 5000;
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_.clear();
        continue;
      }

      IAudioClient* audio_client = nullptr;
      IAudioRenderClient* render_client = nullptr;
      std::wstring previous_capture_id;
      bool switched_capture = false;

      hr = render_dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&audio_client));
      if (SUCCEEDED(hr) && audio_client) {
        WAVEFORMATEX wfx{};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = 2;
        wfx.nAvgBytesPerSec = 96000;
        constexpr DWORD kAutoConvert = 0x80000000u;  // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        constexpr DWORD kSrcQuality = 0x08000000u;   // AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
        const REFERENCE_TIME duration = 800000;      // 80 ms
        hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                      kAutoConvert | kSrcQuality, duration, 0,
                                      &wfx, nullptr);
      }
      UINT32 buffer_frames = 0;
      if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetBufferSize(&buffer_frames);
      }
      if (SUCCEEDED(hr) && audio_client) {
        hr = audio_client->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void**>(&render_client));
      }
      if (SUCCEEDED(hr) && audio_client && render_client && buffer_frames > 0) {
        std::wstring capture_name, capture_id;
        IMMDevice* capture_dev = FindMatchingDevice(
            enumerator, eCapture, capture_needle, &capture_name, &capture_id);
        if (capture_dev) {
          previous_capture_id = GetDefaultCaptureEndpointId(enumerator);
          if (!capture_id.empty() && capture_id != previous_capture_id) {
            if (SetDefaultCaptureEndpoint(capture_id)) {
              switched_capture = true;
              LOG_INFO("Virtual mic sink: switched default microphone to '{}'",
                       NarrowAsciiOrUtf8(capture_name));
            }
          }
          capture_dev->Release();
        }

        hr = audio_client->Start();
        if (SUCCEEDED(hr)) {
          LOG_INFO("Virtual mic sink: streaming remote mic to '{}'",
                   NarrowAsciiOrUtf8(render_name));
          std::vector<uint8_t> chunk;
          chunk.reserve(4800);
          for (;;) {
            {
              std::unique_lock<std::mutex> lock(mutex_);
              cv_.wait_for(lock, std::chrono::milliseconds(10), [&] {
                return shutdown_ || reset_stream_ || buffer_.size() >= 960;
              });
              if (shutdown_ || reset_stream_) break;
              if (buffer_.empty()) {
                if (GetTickCount64() - last_push_ms_ > 2500) break;
                continue;
              }
            }

            UINT32 padding = 0;
            if (FAILED(audio_client->GetCurrentPadding(&padding))) break;
            if (padding >= buffer_frames) {
              Sleep(5);
              continue;
            }
            const UINT32 avail_frames = buffer_frames - padding;
            const size_t max_bytes = static_cast<size_t>(avail_frames) * 2;
            chunk.clear();
            {
              std::lock_guard<std::mutex> lock(mutex_);
              if (shutdown_ || reset_stream_) break;
              const size_t take =
                  (std::min)(max_bytes, buffer_.size() & ~static_cast<size_t>(1));
              if (take == 0) continue;
              chunk.assign(buffer_.begin(),
                           buffer_.begin() + static_cast<std::ptrdiff_t>(take));
              buffer_.erase(buffer_.begin(),
                            buffer_.begin() + static_cast<std::ptrdiff_t>(take));
            }
            const UINT32 frames = static_cast<UINT32>(chunk.size() / 2);
            if (frames == 0) continue;
            BYTE* dst = nullptr;
            if (FAILED(render_client->GetBuffer(frames, &dst)) || !dst) break;
            std::memcpy(dst, chunk.data(), chunk.size());
            if (FAILED(render_client->ReleaseBuffer(frames, 0))) break;
          }
          audio_client->Stop();
          LOG_INFO("Virtual mic sink: stopped remote mic stream");
        }
      } else {
        LOG_WARN("Virtual mic sink: WASAPI init failed for '{}', hr=0x{:08x}",
                 NarrowAsciiOrUtf8(render_name), static_cast<uint32_t>(hr));
        next_probe_ms = GetTickCount64() + 5000;
      }

      if (switched_capture && !previous_capture_id.empty()) {
        if (SetDefaultCaptureEndpoint(previous_capture_id)) {
          LOG_INFO("Virtual mic sink: restored previous default microphone");
        }
      }
      if (render_client) render_client->Release();
      if (audio_client) audio_client->Release();
      render_dev->Release();
      enumerator->Release();

      std::lock_guard<std::mutex> lock(mutex_);
      reset_stream_ = false;
    }

    if (SUCCEEDED(co_hr)) {
      CoUninitialize();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<uint8_t> buffer_;
  ULONGLONG last_push_ms_ = 0;
  bool reset_stream_ = false;
  bool shutdown_ = false;
  std::thread worker_;
};

}  // namespace crossdesk

#endif  // _CROSSDESK_VIRTUAL_MIC_SINK_H_
