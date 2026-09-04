/**
 * @file        ui/overlay/achievement_toast.cpp
 * @brief       Achievement toast implementation. See achievement_toast.h for details.
 *
 * Styled after the Xbox 360 dashboard notification: a dark charcoal
 * stadium-shaped slab that slides down from the top edge, centred, glossier
 * across its top half, with a quartered badge carrying a quadrant arc and a
 * white trophy on the left, two lines of white text, and a single specular
 * sheen that sweeps across shortly after it appears. Drawn through the ImGui
 * draw list rather than widgets, since none of that is expressible with stock
 * widgets. The arc is LEGO Dimensions blue rather than the dashboard's green,
 * which is this project's one accent colour.
 *
 * The achievement's own icon is deliberately not drawn: the 360 popup showed
 * the same trophy for every unlock. AchievementIconCache is still held so that
 * an icon variant of the badge stays a one-line change.
 *
 * @copyright   Copyright (c) 2026 Rien Gupta <rgupta9@scu.edu>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include <rex/ui/overlay/achievement_toast.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/ui_sound.h>

REXCVAR_DEFINE_STRING(ui_sound_achievement, "achievement_unlocked.wav", "UI",
                      "WAV played when an achievement unlocks. Relative paths resolve against "
                      "the executable folder. Empty disables the sound.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace rex::ui {

namespace {

// Panel geometry in unscaled pixels; everything is multiplied by a scale
// derived from the window height so the toast keeps its proportions at 4K.
// The slab is a stadium - both ends are full half-circles - so the corner
// radius is always half the height and is derived rather than tuned.
// Width is measured from the text rather than fixed: the dashboard's popup hugs
// its content, and a constant width leaves a long empty tail after a short
// achievement name. This is only the floor.
constexpr float kPanelMinWidth = 320.0f;
constexpr float kPanelHeight = 78.0f;
constexpr float kMargin = 24.0f;

// The overlay font is sized for dense settings lists; the toast is glanced at
// from across the room, so it draws its text larger than the UI default rather
// than forcing every other overlay to grow with it.
constexpr float kTextScale = 1.4f;

// imgui.h does not export a pi; IM_PI lives in imgui_internal.h, which this
// file deliberately does not pull in.
constexpr float kPi = 3.14159265358979323846f;

// Animation timing in seconds, within kDisplaySeconds.
constexpr float kSlideIn = 0.42f;
constexpr float kSheenStart = 0.30f;
constexpr float kSheenDuration = 0.75f;
constexpr float kFadeOut = 0.45f;

// The 360 notification palette: a dark charcoal slab whose top half is a little
// lighter, the hard gloss split of the era's dashboard, with white text
// throughout and a pale hairline border.
constexpr ImU32 kPanelTop = IM_COL32(70, 72, 74, 248);
constexpr ImU32 kPanelBottom = IM_COL32(48, 50, 52, 248);
constexpr ImU32 kBorder = IM_COL32(188, 192, 196, 190);
// Xbox green, as on the dashboard. The project's blue accent recolours the
// interface chrome, but the arc here reads as achievement status, so it keeps
// the original colour the same way the achievements list does.
constexpr ImU32 kAccent = IM_COL32(140, 198, 63, 255);
constexpr ImU32 kBadgeFill = IM_COL32(24, 26, 28, 255);
constexpr ImU32 kBadgeCross = IM_COL32(110, 114, 116, 225);

// Ease-out cubic: quick start, soft landing, the way the slab settled on the
// dashboard.
float EaseOut(float t) {
  t = std::clamp(t, 0.0f, 1.0f);
  const float inv = 1.0f - t;
  return 1.0f - inv * inv * inv;
}

ImU32 WithAlpha(ImU32 color, float alpha) {
  const float a = ((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
  return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

// The trophy at the centre of the badge, built from primitives rather than a
// font glyph: the UI font is whatever the app registered, so no cup codepoint
// can be assumed to exist. `s` is the glyph's half-extent; the shape is drawn
// centred on `c` and scales with it.
void DrawTrophy(ImDrawList* dl, ImVec2 c, float s, ImU32 color) {
  // Bowl: a cup that narrows toward the bottom, with the lower corners pulled
  // in so it reads as rounded rather than as a plain trapezoid.
  const ImVec2 bowl[6] = {
      ImVec2(c.x - 0.60f * s, c.y - 0.62f * s), ImVec2(c.x + 0.60f * s, c.y - 0.62f * s),
      ImVec2(c.x + 0.46f * s, c.y + 0.02f * s), ImVec2(c.x + 0.22f * s, c.y + 0.26f * s),
      ImVec2(c.x - 0.22f * s, c.y + 0.26f * s), ImVec2(c.x - 0.46f * s, c.y + 0.02f * s),
  };
  dl->AddConvexPolyFilled(bowl, 6, color);

  // Handles: an open arc on each side, stroked rather than filled.
  const float handle_r = 0.30f * s;
  const float thickness = std::max(1.0f, 0.13f * s);
  dl->PathArcTo(ImVec2(c.x - 0.58f * s, c.y - 0.30f * s), handle_r, 0.55f * kPi, 1.55f * kPi,
                0);
  dl->PathStroke(color, 0, thickness);
  dl->PathArcTo(ImVec2(c.x + 0.58f * s, c.y - 0.30f * s), handle_r, -0.55f * kPi, 0.45f * kPi,
                0);
  dl->PathStroke(color, 0, thickness);

  // Stem and base.
  dl->AddRectFilled(ImVec2(c.x - 0.11f * s, c.y + 0.24f * s),
                    ImVec2(c.x + 0.11f * s, c.y + 0.55f * s), color);
  dl->AddRectFilled(ImVec2(c.x - 0.40f * s, c.y + 0.55f * s),
                    ImVec2(c.x + 0.40f * s, c.y + 0.76f * s), color, 0.06f * s);
}

}  // namespace

AchievementToastDialog::AchievementToastDialog(ImGuiDrawer* drawer,
                                               ImmediateDrawer* immediate_drawer,
                                               rex::Runtime* runtime)
    : AchievementNotificationDialog(drawer), icon_cache_(immediate_drawer, runtime) {}

AchievementToastDialog::~AchievementToastDialog() {}

void AchievementToastDialog::Push(const rex::system::AchievementEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  queue_.push_back({event, std::chrono::steady_clock::now()});
}

void AchievementToastDialog::PlayUnlockSound() {
  const std::string& configured = REXCVAR_GET(ui_sound_achievement);
  if (configured.empty()) {
    return;
  }
  std::filesystem::path path(configured);
  if (path.is_relative()) {
    path = rex::filesystem::GetExecutableFolder() / path;
  }
  PlayUiSound(path);
}

void AchievementToastDialog::OnDraw(ImGuiIO& io) {
  auto now = std::chrono::steady_clock::now();

  PendingToast toast;
  float age = 0.0f;
  bool just_became_visible = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!queue_.empty()) {
      if (!queue_.front().visible_since) {
        queue_.front().visible_since = now;
        just_became_visible = true;
      }
      age = std::chrono::duration<float>(now - *queue_.front().visible_since).count();
      if (age >= kDisplaySeconds) {
        queue_.pop_front();
        just_became_visible = false;
      } else {
        break;
      }
    }
    if (queue_.empty()) {
      return;
    }
    toast = queue_.front();
  }

  // The sound rides the start of the slide, not the unlock event: an
  // achievement that arrives while another toast is on screen is queued, and
  // its sound waits for its own turn.
  if (just_became_visible) {
    PlayUnlockSound();
  }

  // Fade covers the tail only - the entrance is carried by the slide.
  float alpha = 1.0f;
  if (age > kDisplaySeconds - kFadeOut) {
    alpha = (kDisplaySeconds - age) / kFadeOut;
  }
  alpha = std::clamp(alpha, 0.0f, 1.0f);

  const float scale = std::max(1.0f, io.DisplaySize.y / 1080.0f);
  const float height = kPanelHeight * scale;
  const float margin = kMargin * scale;
  const float rounding = height * 0.5f;

  // The badge sits just inside the left cap, and the text column begins where
  // the badge ends. Both are expressed as fractions of the height so the whole
  // toast stays proportional at any scale.
  const float badge_inset = height * 0.085f;
  const float badge_radius = height * 0.5f - badge_inset;
  const float text_x_offset = badge_inset * 2.0f + badge_radius * 2.0f + height * 0.15f;

  static constexpr const char* kHeading = "Achievement unlocked";
  char second_line[192];
  std::snprintf(second_line, sizeof(second_line), "%uG - %s",
                static_cast<unsigned>(toast.event.achievement.gamerscore),
                toast.event.achievement.label.c_str());

  // Measured and drawn through the explicit-size overloads, so the enlarged
  // text never has to touch the global font stack that every other overlay
  // shares.
  ImFont* font = ImGui::GetFont();
  const float font_size = ImGui::GetFontSize() * kTextScale;
  const float line_h = font_size;
  const float text_width =
      std::max(font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, kHeading).x,
               font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, second_line).x);
  const float width =
      std::max(kPanelMinWidth * scale, text_x_offset + text_width + rounding * 0.7f);

  // Rests near the top edge, centred on the display, and slides down into place
  // from above it. Centring is computed from the measured width, so it stays
  // centred as the panel grows with a longer achievement name.
  const float rest_y = margin;
  const float slide = EaseOut(age / kSlideIn);
  const ImVec2 pos((io.DisplaySize.x - width) * 0.5f,
                   rest_y - (height + margin) * (1.0f - slide));

  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoFocusOnAppearing;

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  if (ImGui::Begin("##ach_toast", nullptr, flags)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = pos;
    const ImVec2 p1(pos.x + width, pos.y + height);

    // Curve quality. ImGui picks the segment count for arcs and rounded corners
    // from CircleTessellationMaxError, whose 0.30 px default is tuned for small
    // widget corners; on a 32 px stadium cap and the badge ring it is coarse
    // enough to read as stair-stepping. Tightened for this toast only, and the
    // edge-antialiasing flags are asserted rather than assumed - nothing in the
    // drawer sets them either way, so this pins the result.
    ImGuiStyle& style = ImGui::GetStyle();
    const float saved_circle_error = style.CircleTessellationMaxError;
    const ImDrawListFlags saved_flags = dl->Flags;
    style.CircleTessellationMaxError = 0.10f;
    dl->Flags |= ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines |
                 ImDrawListFlags_AntiAliasedLinesUseTex;

    // Drop shadow, then the slab, then a hairline border. The gloss is a
    // lighter copy of the same rounded shape clipped to the upper half - the
    // era's hard 50% split. Clipping a second rounded rect rather than a
    // gradient keeps the round caps intact; AddRectFilledMultiColor has no
    // rounding of its own and would square them off.
    dl->AddRectFilled(ImVec2(p0.x + 2.0f * scale, p0.y + 3.0f * scale),
                      ImVec2(p1.x + 2.0f * scale, p1.y + 3.0f * scale),
                      WithAlpha(IM_COL32(0, 0, 0, 110), alpha), rounding);
    dl->AddRectFilled(p0, p1, WithAlpha(kPanelBottom, alpha), rounding);
    dl->PushClipRect(p0, ImVec2(p1.x, p0.y + height * 0.5f), true);
    dl->AddRectFilled(p0, p1, WithAlpha(kPanelTop, alpha), rounding);
    dl->PopClipRect();
    dl->AddRect(p0, p1, WithAlpha(kBorder, alpha), rounding, 0, 1.5f * scale);

    // Badge: a dark disc quartered by a thin cross, a bright green arc over the
    // upper-left quadrant, and a white trophy in the middle.
    const ImVec2 badge_center(p0.x + badge_inset + badge_radius, p0.y + height * 0.5f);
    dl->AddCircleFilled(badge_center, badge_radius, WithAlpha(kBadgeFill, alpha), 0);

    // The cross stops short of the rim so it reads as engraved rather than as
    // lines laid over the disc.
    const float cross_reach = badge_radius * 0.93f;
    const float hairline = std::max(1.0f, 1.0f * scale);
    dl->AddLine(ImVec2(badge_center.x - cross_reach, badge_center.y),
                ImVec2(badge_center.x + cross_reach, badge_center.y),
                WithAlpha(kBadgeCross, alpha), hairline);
    dl->AddLine(ImVec2(badge_center.x, badge_center.y - cross_reach),
                ImVec2(badge_center.x, badge_center.y + cross_reach),
                WithAlpha(kBadgeCross, alpha), hairline);

    // ImGui angles run clockwise from +X with Y pointing down, so the upper-left
    // quadrant is PI to 1.5*PI.
    const float arc_radius = badge_radius * 0.88f;
    dl->PathArcTo(badge_center, arc_radius, kPi, 1.5f * kPi, 0);
    dl->PathStroke(WithAlpha(kAccent, alpha), 0, badge_radius * 0.20f);

    DrawTrophy(dl, badge_center, badge_radius * 0.52f, WithAlpha(IM_COL32_WHITE, alpha));

    // Two lines of white text: the heading, then the score and the achievement
    // name together on one line.
    // The pair is centred on the slab as one block rather than each line being
    // placed from the middle outward.
    const float text_x = p0.x + text_x_offset;
    const float line_gap = line_h * 1.14f;
    const float text_y = p0.y + height * 0.5f - (line_h + line_gap) * 0.5f;

    // The panel is sized to the text now, but a name long enough to be clamped
    // by the maximum width still needs the clip as a backstop.
    dl->PushClipRect(ImVec2(text_x, p0.y), ImVec2(p1.x - rounding * 0.35f, p1.y), true);
    dl->AddText(font, font_size, ImVec2(text_x, text_y), WithAlpha(IM_COL32_WHITE, alpha),
                kHeading);
    dl->AddText(font, font_size, ImVec2(text_x, text_y + line_gap),
                WithAlpha(IM_COL32_WHITE, alpha), second_line);
    dl->PopClipRect();

    // Specular sweep: one slanted bright band crossing the slab, brightest at
    // mid-travel. Clipped to the panel so it cannot bleed onto the game.
    const float sheen_t = (age - kSheenStart) / kSheenDuration;
    if (sheen_t > 0.0f && sheen_t < 1.0f) {
      dl->PushClipRect(p0, p1, true);
      const float band = width * 0.18f;
      const float lean = height * 0.35f;
      const float x = p0.x - band - lean + (width + 2.0f * band + lean) * sheen_t;
      const float peak = std::sin(sheen_t * kPi);
      const ImU32 bright = WithAlpha(IM_COL32(255, 255, 255, 42), alpha * peak);
      dl->AddQuadFilled(ImVec2(x, p1.y), ImVec2(x + lean, p0.y), ImVec2(x + lean + band, p0.y),
                        ImVec2(x + band, p1.y), bright);
      dl->PopClipRect();
    }

    // The style is global and the draw list is reused by every other overlay,
    // so both go back exactly as they were found.
    style.CircleTessellationMaxError = saved_circle_error;
    dl->Flags = saved_flags;
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

}  // namespace rex::ui
