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
#include <utility>
#include <vector>
#include <rex/ui/imgui_dialog.h>

namespace rex::ui {

// One row on a player-facing settings page. `cvar` is the flag it edits;
// `label` is what the row is called; `help` replaces the flag's description in
// the tooltip when set. `choices` turns a numeric or free-text flag into a
// drop-down of (value, label) pairs - "3" shown as "4x", say - and is what
// keeps the technical encodings out of the player's face.
struct SettingsItem {
  std::string cvar;
  std::string label;
  std::string help;
  std::vector<std::pair<std::string, std::string>> choices;
};

// A page in the left-hand list. Pages are shown in the order given, before
// the "Advanced" tree that holds every flag no page mentions.
struct SettingsPage {
  std::string title;
  std::vector<SettingsItem> items;
};

// The player-facing layout of the settings window. Without one the dialog
// falls back to the raw category tree, which is right for the SDK's own tools
// and wrong for a game that ships to people who have never heard of a cvar.
struct SettingsPresentation {
  std::vector<SettingsPage> pages;
  // Title of the tree holding everything the pages leave out.
  std::string advanced_title = "Advanced";
  // Flags never shown, not even under Advanced.
  std::vector<std::string> hidden;
};

class SettingsDialog : public ImGuiDialog {
 public:
  // config_path: where "Save to config" writes (e.g. exe_dir / "app.toml")
  SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path);
  ~SettingsDialog();

  // Installs the player-facing layout for every settings window opened from
  // now on. Call once at startup, from the app.
  static void SetPresentation(SettingsPresentation presentation);
  static const SettingsPresentation& presentation();

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  std::filesystem::path config_path_;
  char search_buf_[128] = {};
  // Either a page title (prefixed with "page:") or a raw category path.
  std::string selected_category_;
  std::string capturing_bind_name_;
  // The string cvar currently being typed into, and its buffer. Kept across
  // frames so the text is not refilled from the cvar while it is being edited.
  std::string editing_text_name_;
  char text_buf_[512] = {};
};

}  // namespace rex::ui
