/**
 * @file        rex/ui/overlay/settings_overlay.h
 *
 * @brief       ImGui settings overlay dialog for cvar editing with save-to-config.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <filesystem>
#include <string>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~SettingsDialog();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  std::string selected_category_;
  std::string capturing_bind_name_;
  // The string cvar currently being typed into, and its buffer. Kept across
  // frames so the text is not refilled from the cvar while it is being edited.
  std::string editing_text_name_;
  char text_buf_[512] = {};
};

}  // namespace rex::ui
