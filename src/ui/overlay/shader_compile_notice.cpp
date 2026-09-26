/**
 * @file        ui/overlay/shader_compile_notice.cpp
 *
 * @brief       See shader_compile_notice.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/shader_compile_notice.h>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>

#include <imgui.h>

REXCVAR_DEFINE_BOOL(show_shader_compile_notice, true, "UI",
                    "Show \"Compiling shaders\" with a count at the bottom of the screen while "
                    "shaders compile in the background")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {
// Bursts shorter than this never show: a pipeline or two mid-level would only
// blink the text on and off.
constexpr uint64_t kShowDelayMs = 250;
// Keep the last count up briefly after a burst ends, so it does not vanish
// the instant a second burst is about to start.
constexpr uint64_t kLingerMs = 500;
}  // namespace

ShaderCompileNoticeDialog::ShaderCompileNoticeDialog(ImGuiDrawer* imgui_drawer,
                                                     ProgressProvider provider)
    : ImGuiDialog(imgui_drawer), provider_(std::move(provider)) {}

void ShaderCompileNoticeDialog::OnDraw(ImGuiIO& io) {
  if (!provider_ || !REXCVAR_GET(show_shader_compile_notice)) {
    visible_since_ticks_ = 0;
    return;
  }
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
  uint32_t done = 0, total = 0;
  if (provider_(done, total)) {
    if (!visible_since_ticks_) {
      visible_since_ticks_ = now;
    }
    last_active_ticks_ = now;
    last_done_ = done;
    last_total_ = total;
  } else if (!visible_since_ticks_ || (now - last_active_ticks_) * 1000 / freq > kLingerMs) {
    visible_since_ticks_ = 0;
    return;
  } else {
    // Lingering after the burst: show it as finished.
    last_done_ = last_total_;
  }
  if ((now - visible_since_ticks_) * 1000 / freq < kShowDelayMs) {
    return;
  }

  char text[96];
  snprintf(text, sizeof(text), "Compiling shaders, please wait... %u / %u", last_done_,
           last_total_);
  const ImVec2 size = ImGui::CalcTextSize(text);
  const ImVec2 padding(14.0f, 8.0f);
  ImGui::SetNextWindowPos(
      ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - io.DisplaySize.y * 0.06f),
      ImGuiCond_Always, ImVec2(0.5f, 1.0f));
  ImGui::SetNextWindowSize(ImVec2(size.x + padding.x * 2, size.y + padding.y * 2));
  ImGui::SetNextWindowBgAlpha(0.6f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##shader_compile_notice", nullptr, kFlags)) {
    ImGui::TextUnformatted(text);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

}  // namespace rex::ui
