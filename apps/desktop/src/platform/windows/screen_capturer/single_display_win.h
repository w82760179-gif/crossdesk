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
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                    &mode_count) != ERROR_SUCCESS) {
      return false;
    }
    if (path_count <= 1) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                           &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
      return false;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    if (paths.size() <= 1) return false;

    size_t primary_index = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
      const UINT32 idx = paths[i].sourceInfo.modeInfoIdx;
      if (idx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && idx < modes.size() &&
          modes[idx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
          modes[idx].sourceMode.position.x == 0 &&
          modes[idx].sourceMode.position.y == 0) {
        primary_index = i;
        break;
      }
    }

    DISPLAYCONFIG_PATH_INFO single_path = paths[primary_index];
    std::vector<DISPLAYCONFIG_MODE_INFO> apply_modes = modes;
    LONG status = SetDisplayConfig(
        1, &single_path, static_cast<UINT32>(apply_modes.size()),
        apply_modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES);
    if (status != ERROR_SUCCESS) {
      status = SetDisplayConfig(0, nullptr, 0, nullptr,
                                SDC_TOPOLOGY_INTERNAL | SDC_APPLY);
    }
    if (status != ERROR_SUCCESS) return false;

    saved_paths_ = std::move(paths);
    saved_modes_ = std::move(modes);
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
      status = SetDisplayConfig(0, nullptr, 0, nullptr,
                                SDC_TOPOLOGY_EXTEND | SDC_APPLY);
    }
    if (status != ERROR_SUCCESS) return false;
    saved_paths_.clear();
    saved_modes_.clear();
    collapsed_ = false;
    return true;
  }

 private:
  bool collapsed_ = false;
  std::vector<DISPLAYCONFIG_PATH_INFO> saved_paths_;
  std::vector<DISPLAYCONFIG_MODE_INFO> saved_modes_;
};

}  // namespace crossdesk

#endif
