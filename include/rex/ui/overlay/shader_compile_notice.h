/**
 * @file        rex/ui/overlay/shader_compile_notice.h
 *
 * @brief       "Compiling shaders" notice at the bottom of the screen.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once
#include <rex/ui/imgui_dialog.h>

#include <cstdint>
#include <functional>

namespace rex::ui {

// Always registered; draws nothing unless shaders are compiling and the
// show_shader_compile_notice setting is on. Players took the hitching while a
// new area's shaders compile for the game being broken - this says it is not.
class ShaderCompileNoticeDialog : public ImGuiDialog {
 public:
  // Fills done / total and returns true while a burst is being compiled.
  using ProgressProvider = std::function<bool(uint32_t& done, uint32_t& total)>;

  ShaderCompileNoticeDialog(ImGuiDrawer* imgui_drawer, ProgressProvider provider);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  ProgressProvider provider_;
  // Short bursts (one or two pipelines mid-level) would only flicker.
  uint64_t visible_since_ticks_ = 0;
  uint64_t last_active_ticks_ = 0;
  uint32_t last_done_ = 0;
  uint32_t last_total_ = 0;
};

}  // namespace rex::ui
