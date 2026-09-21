/**
 * @file        ui/overlay/settings_overlay.cpp
 *
 * @brief       Settings overlay implementation. See settings_overlay.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/settings_overlay.h>
#include <rex/cvar.h>
#include <rex/string.h>
#include <rex/string/numeric.h>
#include <rex/ui/keybinds.h>
#include <imgui.h>

#include <cstring>
#include <algorithm>
#include <cctype>
#include <functional>
#include <utility>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rex::ui {

namespace {

// The control column and the widget widths below were tuned as constants
// against the SDK's built-in 13 px font. An app that installs a larger UI font
// through OnConfigureFonts pushes the longest cvar names past the column, and
// the widgets end up drawn on top of the labels. Scaling by the live font size
// keeps the layout proportional instead of pinning it to one font.
constexpr float kTunedFontSize = 13.0f;

float FontScale() { return std::max(1.0f, ImGui::GetFontSize() / kTunedFontSize); }

float ControlColumnX() { return 360.0f * FontScale(); }

}  // namespace

SettingsDialog::SettingsDialog(ImGuiDrawer* imgui_drawer, std::filesystem::path config_path)
    : ImGuiDialog(imgui_drawer), config_path_(std::move(config_path)) {}

SettingsDialog::~SettingsDialog() {}

static const char* LifecycleBadge(rex::cvar::Lifecycle lc) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return " [live]";
    case rex::cvar::Lifecycle::kRequiresRestart:
      return " [restart]";
    case rex::cvar::Lifecycle::kInitOnly:
      return " [init-only]";
  }
  return "";
}

static ImVec4 LifecycleColor(rex::cvar::Lifecycle lc, const SettingsStyle& style) {
  switch (lc) {
    case rex::cvar::Lifecycle::kHotReload:
      return style.lifecycle_live;
    case rex::cvar::Lifecycle::kRequiresRestart:
      return style.lifecycle_restart;
    case rex::cvar::Lifecycle::kInitOnly:
      return style.lifecycle_init_only;
  }
  return style.lifecycle_unknown;
}

static rex::ui::VirtualKey ImGuiKeyToVirtualKey(ImGuiKey key) {
  using VK = rex::ui::VirtualKey;
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kA) + (key - ImGuiKey_A));
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::k0) + (key - ImGuiKey_0));
  }
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F24) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kF1) + (key - ImGuiKey_F1));
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return static_cast<VK>(static_cast<uint16_t>(VK::kNumpad0) + (key - ImGuiKey_Keypad0));
  }
  switch (key) {
    case ImGuiKey_Space:
      return VK::kSpace;
    case ImGuiKey_Enter:
      return VK::kReturn;
    case ImGuiKey_Escape:
      return VK::kEscape;
    case ImGuiKey_Tab:
      return VK::kTab;
    case ImGuiKey_Backspace:
      return VK::kBack;
    case ImGuiKey_Delete:
      return VK::kDelete;
    case ImGuiKey_Insert:
      return VK::kInsert;
    case ImGuiKey_Home:
      return VK::kHome;
    case ImGuiKey_End:
      return VK::kEnd;
    case ImGuiKey_PageUp:
      return VK::kPrior;
    case ImGuiKey_PageDown:
      return VK::kNext;
    case ImGuiKey_LeftArrow:
      return VK::kLeft;
    case ImGuiKey_RightArrow:
      return VK::kRight;
    case ImGuiKey_UpArrow:
      return VK::kUp;
    case ImGuiKey_DownArrow:
      return VK::kDown;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift:
      return VK::kShift;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl:
      return VK::kControl;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt:
      return VK::kMenu;
    case ImGuiKey_CapsLock:
      return VK::kCapital;
    case ImGuiKey_NumLock:
      return VK::kNumLock;
    case ImGuiKey_ScrollLock:
      return VK::kScroll;
    case ImGuiKey_PrintScreen:
      return VK::kSnapshot;
    case ImGuiKey_Pause:
      return VK::kPause;
    case ImGuiKey_GraveAccent:
      return VK::kOem3;
    case ImGuiKey_Minus:
      return VK::kOemMinus;
    case ImGuiKey_Equal:
      return VK::kOemPlus;
    case ImGuiKey_LeftBracket:
      return VK::kOem4;
    case ImGuiKey_RightBracket:
      return VK::kOem6;
    case ImGuiKey_Backslash:
      return VK::kOem5;
    case ImGuiKey_Semicolon:
      return VK::kOem1;
    case ImGuiKey_Apostrophe:
      return VK::kOem7;
    case ImGuiKey_Comma:
      return VK::kOemComma;
    case ImGuiKey_Period:
      return VK::kOemPeriod;
    case ImGuiKey_Slash:
      return VK::kOem2;
    case ImGuiKey_KeypadDecimal:
      return VK::kDecimal;
    case ImGuiKey_KeypadDivide:
      return VK::kDivide;
    case ImGuiKey_KeypadMultiply:
      return VK::kMultiply;
    case ImGuiKey_KeypadSubtract:
      return VK::kSubtract;
    case ImGuiKey_KeypadAdd:
      return VK::kAdd;
    case ImGuiKey_KeypadEnter:
      return VK::kReturn;
    default:
      return VK::kNone;
  }
}

namespace {

SettingsPresentation& MutablePresentation() {
  static SettingsPresentation presentation;
  return presentation;
}

constexpr const char* kPagePrefix = "page:";

}  // namespace

void SettingsDialog::SetPresentation(SettingsPresentation presentation) {
  MutablePresentation() = std::move(presentation);
}

const SettingsPresentation& SettingsDialog::presentation() { return MutablePresentation(); }

void SettingsDialog::OnDraw(ImGuiIO& /*io*/) {
  auto& registry = rex::cvar::GetRegistry();
  const SettingsPresentation& pres = presentation();
  const SettingsStyle& style = imgui_drawer()->style().settings;

  // Flags a page already shows, and flags hidden outright: neither belongs in
  // the Advanced tree, or the same setting would be reachable twice.
  std::set<std::string> presented;
  for (const auto& page : pres.pages) {
    for (const auto& item : page.items) {
      presented.insert(item.cvar);
    }
  }
  std::set<std::string> hidden(pres.hidden.begin(), pres.hidden.end());
  auto in_advanced = [&](const rex::cvar::FlagEntry& entry) {
    return !presented.count(entry.name) && !hidden.count(entry.name);
  };

  // With no pages at all the old behaviour stands: everything is a category.
  const bool has_pages = !pres.pages.empty();
  if (selected_category_.empty() && has_pages) {
    selected_category_ = std::string(kPagePrefix) + pres.pages.front().title;
  }
  const bool page_selected = selected_category_.rfind(kPagePrefix, 0) == 0;

  // Collect sorted unique category paths of the flags that go to Advanced.
  std::set<std::string> category_set;
  for (auto& entry : registry) {
    if (!has_pages || in_advanced(entry)) {
      category_set.insert(entry.category);
    }
  }

  // Build tree: for each category path like "Input/Keybinds/Controller",
  // also register the parent paths "Input" and "Input/Keybinds" as nodes.
  struct CatNode {
    std::string full_path;
    std::string label;  // leaf segment (e.g. "Controller")
    std::map<std::string, CatNode> children;
    bool has_direct_entries = false;
  };
  std::map<std::string, CatNode> tree;

  for (auto& cat : category_set) {
    std::map<std::string, CatNode>* level = &tree;
    std::string path_so_far;
    size_t start = 0;
    while (start < cat.size()) {
      size_t slash = cat.find('/', start);
      std::string segment =
          (slash == std::string::npos) ? cat.substr(start) : cat.substr(start, slash - start);
      if (!path_so_far.empty())
        path_so_far += "/";
      path_so_far += segment;
      auto& node = (*level)[segment];
      node.full_path = path_so_far;
      node.label = segment;
      if (path_so_far == cat)
        node.has_direct_entries = true;
      level = &node.children;
      start = (slash == std::string::npos) ? cat.size() : slash + 1;
    }
  }

  const std::string search(search_buf_);
  const bool searching = !search.empty();

  // Wide enough that a sentence-length label and a text field with buttons
  // sit side by side without the widgets landing on top of the words.
  ImGui::SetNextWindowSize(ImVec2(960.0f * FontScale(), 600.0f * FontScale()),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Settings##rex", nullptr, ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }

  // Search bar at the top (full width).
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##search", "Search settings...", search_buf_, sizeof(search_buf_));

  ImGui::Separator();

  const float panel_width = 190.0f * FontScale();
  ImGui::BeginChild("##cats", ImVec2(panel_width, -30.0f), true);

  // Player-facing pages first, as a plain list.
  for (const auto& page : pres.pages) {
    const std::string key = std::string(kPagePrefix) + page.title;
    if (ImGui::Selectable(page.title.c_str(), selected_category_ == key)) {
      selected_category_ = key;
    }
  }

  // Recursive lambda to draw the category tree.
  std::function<void(const std::map<std::string, CatNode>&, int)> draw_tree;
  draw_tree = [&](const std::map<std::string, CatNode>& nodes, int depth) {
    for (auto& [key, node] : nodes) {
      if (node.children.empty()) {
        // Leaf node - selectable
        bool selected = (selected_category_ == node.full_path);
        if (depth > 0)
          ImGui::Indent(8.0f);
        if (ImGui::Selectable(node.label.c_str(), selected)) {
          selected_category_ = node.full_path;
        }
        if (depth > 0)
          ImGui::Unindent(8.0f);
      } else {
        // Parent node with children - use tree node
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_OpenOnArrow;
        if (node.has_direct_entries) {
          // Can be selected as well as expanded
          if (selected_category_ == node.full_path)
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        bool open = ImGui::TreeNodeEx(node.label.c_str(), flags);
        if (ImGui::IsItemClicked() && node.has_direct_entries) {
          selected_category_ = node.full_path;
        }
        if (open) {
          draw_tree(node.children, depth + 1);
          ImGui::TreePop();
        }
      }
    }
  };
  if (has_pages) {
    ImGui::Separator();
    // Collapsed by default: this is the developer's end of the window.
    ImGuiTreeNodeFlags adv_flags = ImGuiTreeNodeFlags_OpenOnArrow;
    if (selected_category_ == "advanced:")
      adv_flags |= ImGuiTreeNodeFlags_Selected;
    bool adv_open = ImGui::TreeNodeEx(pres.advanced_title.c_str(), adv_flags);
    if (ImGui::IsItemClicked()) {
      selected_category_ = "advanced:";
    }
    if (adv_open) {
      draw_tree(tree, 1);
      ImGui::TreePop();
    }
  } else {
    // Root node named after the config file
    std::string root_label = config_path_.stem().string();
    ImGuiTreeNodeFlags root_flags = ImGuiTreeNodeFlags_DefaultOpen;
    if (selected_category_.empty())
      root_flags |= ImGuiTreeNodeFlags_Selected;
    bool root_open = ImGui::TreeNodeEx(root_label.c_str(), root_flags);
    if (ImGui::IsItemClicked()) {
      selected_category_.clear();
    }
    if (root_open) {
      draw_tree(tree, 1);
      ImGui::TreePop();
    }
  }

  ImGui::EndChild();

  ImGui::SameLine();

  // Helper: check if a CVAR's category matches the selected category.
  // Exact match or prefix match (e.g. selecting "Input" shows all "Input/*").
  auto category_matches = [&](const std::string& cat) -> bool {
    if (selected_category_.empty() || selected_category_ == "advanced:")
      return true;  // Root selected - show all
    if (cat == selected_category_)
      return true;
    if (cat.size() > selected_category_.size() &&
        cat.compare(0, selected_category_.size(), selected_category_) == 0 &&
        cat[selected_category_.size()] == '/') {
      return true;
    }
    return false;
  };

  // Helper: check if a category is a keybind category.
  auto is_keybind_category = [](const std::string& cat) -> bool {
    return cat == "Input/Keybinds" ||
           (cat.size() > 15 && cat.compare(0, 15, "Input/Keybinds/") == 0);
  };

  // A keybind category may also hold a setting that is not itself a bind, such
  // as which player the keyboard is. Those are told apart by name, so they get
  // an ordinary widget instead of a Rebind button.
  auto is_bind_entry = [](const std::string& name) -> bool {
    return name.rfind("keybind_", 0) == 0 || name.rfind("bind_", 0) == 0;
  };

  auto to_lower = [](std::string s) {
    for (auto& c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  const std::string search_lower = to_lower(search);

  // Draws one settings row. `label` is what the row is called (a page's label,
  // or the raw flag name under Advanced), `help` what the tooltip says, and
  // `choices` an optional drop-down replacing the flag's own widget.
  auto draw_entry = [&](const rex::cvar::FlagEntry& entry, const std::string& label,
                        const std::string& help,
                        const std::vector<std::pair<std::string, std::string>>& choices) {
    bool read_only = (entry.lifecycle == rex::cvar::Lifecycle::kInitOnly);

    ImGui::PushID(entry.name.c_str());

    if (read_only)
      ImGui::BeginDisabled();

    std::string current_val = entry.getter();

    const char* lifecycle_label = "";
    switch (entry.lifecycle) {
      case rex::cvar::Lifecycle::kHotReload:
        lifecycle_label = "Applies immediately";
        break;
      case rex::cvar::Lifecycle::kRequiresRestart:
        lifecycle_label = "Takes effect after a restart";
        break;
      case rex::cvar::Lifecycle::kInitOnly:
        lifecycle_label = "Read-only - set at initialization only";
        break;
    }
    auto show_tooltip = [&] {
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (!help.empty()) {
          ImGui::SetTooltip("%s\n[%s]", help.c_str(), lifecycle_label);
        } else {
          ImGui::SetTooltip("[%s]", lifecycle_label);
        }
      }
    };

    if (is_keybind_category(entry.category) && is_bind_entry(entry.name)) {
      // Controller binds stay editable with MnK mode off, so the layout can be
      // set up before switching it on. Only the row is dimmed as a hint.
      bool mnk_off =
          (entry.category == "Input/Keybinds/Controller" && !REXCVAR_QUERY(bool, mnk_mode));
      if (mnk_off)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.6f);

      ImGui::TextUnformatted(label.c_str());
      show_tooltip();
      ImGui::SameLine(ControlColumnX());

      bool is_capturing = (capturing_bind_name_ == entry.name);

      if (is_capturing) {
        ImGui::Button("Press any key...##v", ImVec2(140.0f * FontScale(), 0));

        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
          capturing_bind_name_.clear();
        } else {
          for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            auto imgui_key = static_cast<ImGuiKey>(k);
            if (imgui_key == ImGuiKey_Escape)
              continue;
            if (ImGui::IsKeyPressed(imgui_key)) {
              auto vk = ImGuiKeyToVirtualKey(imgui_key);
              std::string name = rex::ui::VirtualKeyToString(vk);
              if (!name.empty()) {
                rex::cvar::SetFlagByName(entry.name, name);
              }
              capturing_bind_name_.clear();
              break;
            }
          }
          for (int mb = 0; mb < 3; ++mb) {
            if (ImGui::IsMouseClicked(mb)) {
              const char* names[] = {"LMB", "RMB", "MMB"};
              rex::cvar::SetFlagByName(entry.name, names[mb]);
              capturing_bind_name_.clear();
              break;
            }
          }
        }
      } else {
        ImGui::SetNextItemWidth(80.0f * FontScale());
        ImGui::Text("%-12s", current_val.empty() ? "(none)" : current_val.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Rebind##v")) {
          capturing_bind_name_ = entry.name;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset##v")) {
          rex::cvar::SetFlagByName(entry.name, entry.default_value);
        }
        ImGui::SameLine();
        // An empty bind never matches, which is how an action is turned off.
        if (ImGui::SmallButton("Clear##v")) {
          rex::cvar::SetFlagByName(entry.name, "");
        }
      }

      // Conflict detection
      if (!current_val.empty()) {
        int conflict_count = 0;
        // The D-pad deliberately shares the left stick's keys by default, so
        // that pair is not reported as a clash.
        auto direction_twins = [](const std::string& a, const std::string& b) {
          auto dir = [](const std::string& n) -> std::string {
            for (const char* p : {"keybind_dpad_", "keybind_lstick_"}) {
              size_t len = std::strlen(p);
              if (n.compare(0, len, p) == 0)
                return n.substr(len);
            }
            return std::string();
          };
          std::string da = dir(a);
          return !da.empty() && da == dir(b);
        };
        for (auto& other : registry) {
          if (is_keybind_category(other.category) && is_bind_entry(other.name) &&
              other.name != entry.name && other.getter() == current_val &&
              !direction_twins(entry.name, other.name)) {
            conflict_count++;
          }
        }
        if (conflict_count > 0) {
          ImGui::SameLine();
          ImGui::TextColored(style.warning, "(!)");
          if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Key '%s' is also bound to %d other action(s)", current_val.c_str(),
                              conflict_count);
          }
        }
      }

      if (mnk_off)
        ImGui::PopStyleVar();
      if (read_only)
        ImGui::EndDisabled();
      ImGui::PopID();
      return;
    }

    // Non-keybind CVARs: colored label on left, value widget on right
    ImGui::TextColored(LifecycleColor(entry.lifecycle, style), "%s", label.c_str());
    show_tooltip();
    ImGui::SameLine(ControlColumnX());

    ImGui::SetNextItemWidth(220.0f * FontScale());
    if (!choices.empty()) {
      // A page-supplied drop-down: the flag holds the value, the player sees
      // the label. An unlisted current value is shown raw rather than lost.
      const char* shown = current_val.c_str();
      for (const auto& [value, choice_label] : choices) {
        if (value == current_val) {
          shown = choice_label.c_str();
          break;
        }
      }
      if (ImGui::BeginCombo("##v", shown)) {
        for (const auto& [value, choice_label] : choices) {
          bool sel = (value == current_val);
          if (ImGui::Selectable(choice_label.c_str(), sel)) {
            rex::cvar::SetFlagByName(entry.name, value);
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else if (entry.type == rex::cvar::FlagType::Boolean) {
      bool v = rex::string::from_string<bool>(current_val, false);
      if (ImGui::Checkbox("##v", &v)) {
        rex::cvar::SetFlagByName(entry.name, v ? "true" : "false");
      }
    } else if (entry.type == rex::cvar::FlagType::String &&
               !entry.constraints.allowed_values.empty()) {
      const auto& opts = entry.constraints.allowed_values;
      int cur_idx = 0;
      for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
        if (opts[i] == current_val) {
          cur_idx = i;
          break;
        }
      }
      if (ImGui::BeginCombo("##v", opts[cur_idx].c_str())) {
        for (int i = 0; i < static_cast<int>(opts.size()); ++i) {
          bool sel = (i == cur_idx);
          if (ImGui::Selectable(opts[i].c_str(), sel)) {
            rex::cvar::SetFlagByName(entry.name, opts[i]);
          }
          if (sel)
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
    } else if (entry.type == rex::cvar::FlagType::Int32 ||
               entry.type == rex::cvar::FlagType::Int64 ||
               entry.type == rex::cvar::FlagType::Uint32 ||
               entry.type == rex::cvar::FlagType::Uint64) {
      int v = std::atoi(current_val.c_str());
      int vmin =
          entry.constraints.min.has_value() ? static_cast<int>(*entry.constraints.min) : INT_MIN;
      int vmax =
          entry.constraints.max.has_value() ? static_cast<int>(*entry.constraints.max) : INT_MAX;
      if (ImGui::InputInt("##v", &v)) {
        v = std::clamp(v, vmin, vmax);
        rex::cvar::SetFlagByName(entry.name, std::to_string(v));
      }
    } else if (entry.type == rex::cvar::FlagType::Double) {
      double v = std::atof(current_val.c_str());
      if (ImGui::InputDouble("##v", &v, 0.0, 0.0, "%.4f")) {
        if (entry.constraints.min)
          v = std::max(v, *entry.constraints.min);
        if (entry.constraints.max)
          v = std::min(v, *entry.constraints.max);
        rex::cvar::SetFlagByName(entry.name, std::to_string(v));
      }
    } else if (entry.type == rex::cvar::FlagType::Command) {
      if (ImGui::Button(std::string(entry.name + "##v").c_str())) {
        entry.command_callback("");
      }
    } else {
      // Refilling the buffer from the cvar every frame undid each keystroke,
      // so a path could never be typed over: ImGui reloads its edit state
      // when the caller's buffer changes underneath it. The field being
      // edited keeps its own buffer instead, and the cvar is written when the
      // field is left or Enter is pressed - not on every character, which
      // would apply half-typed paths.
      const bool editing = (editing_text_name_ == entry.name);
      char local_buf[sizeof(text_buf_)];
      char* buf = text_buf_;
      if (!editing) {
        buf = local_buf;
        rex::string::copy_truncating(local_buf, current_val, sizeof(local_buf));
      }
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::InputText("##v", buf, sizeof(text_buf_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rex::cvar::SetFlagByName(entry.name, buf);
      }
      if (ImGui::IsItemActivated()) {
        rex::string::copy_truncating(text_buf_, current_val, sizeof(text_buf_));
        editing_text_name_ = entry.name;
      } else if (editing && ImGui::IsItemDeactivated()) {
        // Escape reverts the text before deactivating, so this writes back
        // what was already there and the setting is unchanged, which is what
        // Escape should do.
        rex::cvar::SetFlagByName(entry.name, text_buf_);
        editing_text_name_.clear();
      }
    }

    if (read_only)
      ImGui::EndDisabled();

    ImGui::PopID();
  };

  auto find_entry = [&](const std::string& name) -> const rex::cvar::FlagEntry* {
    for (auto& entry : registry) {
      if (entry.name == name) {
        return &entry;
      }
    }
    return nullptr;
  };

  ImGui::BeginChild("##cvars", ImVec2(0, -30.0f), false);
  if (searching) {
    // Search covers everything a person could be looking for: the page label,
    // the flag name and the description, on the pages and under Advanced.
    for (const auto& page : pres.pages) {
      bool titled = false;
      for (const auto& item : page.items) {
        const rex::cvar::FlagEntry* entry = find_entry(item.cvar);
        if (!entry) {
          continue;
        }
        const std::string& help = item.help.empty() ? entry->description : item.help;
        if (to_lower(item.label).find(search_lower) == std::string::npos &&
            to_lower(entry->name).find(search_lower) == std::string::npos &&
            to_lower(help).find(search_lower) == std::string::npos) {
          continue;
        }
        if (!titled) {
          ImGui::SeparatorText(page.title.c_str());
          titled = true;
        }
        draw_entry(*entry, item.label, help, item.choices);
      }
    }
    bool titled = false;
    for (auto& entry : registry) {
      if (has_pages && !in_advanced(entry)) {
        continue;
      }
      if (to_lower(entry.name).find(search_lower) == std::string::npos &&
          to_lower(entry.description).find(search_lower) == std::string::npos) {
        continue;
      }
      if (has_pages && !titled) {
        ImGui::SeparatorText(pres.advanced_title.c_str());
        titled = true;
      }
      draw_entry(entry, entry.name, entry.description, {});
    }
  } else if (page_selected) {
    const std::string title = selected_category_.substr(std::strlen(kPagePrefix));
    for (const auto& page : pres.pages) {
      if (page.title != title) {
        continue;
      }
      for (const auto& item : page.items) {
        const rex::cvar::FlagEntry* entry = find_entry(item.cvar);
        if (!entry) {
          // A page naming a flag this build does not have is a layout bug,
          // not a player's problem; say so quietly instead of crashing.
          ImGui::TextDisabled("%s (not available in this build)", item.label.c_str());
          continue;
        }
        draw_entry(*entry, item.label, item.help.empty() ? entry->description : item.help,
                   item.choices);
      }
      break;
    }
  } else {
    for (auto& entry : registry) {
      if (has_pages && !in_advanced(entry)) {
        continue;
      }
      if (!category_matches(entry.category)) {
        continue;
      }
      // Under Advanced the flag name is the label: that is what a person will
      // be asked to change in a bug report, and what the config file says.
      const bool bind = is_keybind_category(entry.category) && is_bind_entry(entry.name);
      draw_entry(entry, bind && !entry.description.empty() ? entry.description : entry.name,
                 entry.description, {});
    }
  }
  ImGui::EndChild();

  // Bottom bar: Save and reset buttons.
  ImGui::Separator();
  if (ImGui::Button("Save to config")) {
    rex::cvar::SaveConfig(config_path_);
  }
  ImGui::SameLine();
  // A way back for anyone who changed settings until the game stopped working.
  // Behind a confirmation because it also throws away keybinds, and written to
  // the config right away - a setting that only takes effect on the next start
  // is exactly the kind that gets a run stuck, so the reset has to survive a
  // restart too.
  if (ImGui::Button("Reset all to defaults")) {
    ImGui::OpenPopup("Reset all settings?");
  }
  if (ImGui::BeginPopupModal("Reset all settings?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Every setting and keybind goes back to its default value,");
    ImGui::TextUnformatted("and the config file is overwritten with them.");
    ImGui::TextUnformatted("Settings marked as restart-only apply on the next start.");
    ImGui::Separator();
    if (ImGui::Button("Reset")) {
      rex::cvar::ResetAllToDefaults();
      rex::cvar::SaveConfig(config_path_);
      // Anything half-typed or half-rebound refers to a value that just
      // changed underneath it.
      capturing_bind_name_.clear();
      editing_text_name_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(%s)", config_path_.filename().string().c_str());
  ImGui::SameLine();
  ImGui::TextColored(style.lifecycle_restart, "This colour");
  ImGui::SameLine();
  ImGui::TextDisabled("= takes effect after a restart");

  ImGui::End();
}

}  // namespace rex::ui
