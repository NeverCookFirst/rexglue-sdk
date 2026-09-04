#pragma once
/**
 * Trigger a RenderDoc frame capture from inside the process.
 *
 * Deliberately separate from renderdoc_api.h: that header pulls in the
 * third-party renderdoc_app.h, which is not part of the installed include
 * tree, so anything an application compiles (rex_app.cpp included) cannot see
 * it. This one is safe to include anywhere.
 */

namespace rex {
namespace ui {

// Captures the NEXT presented frame. Returns false, and logs why, when the
// process was not launched under RenderDoc. Cheap to call repeatedly: the API
// handle is probed once and cached.
bool TriggerRenderDocCapture();

}  // namespace ui
}  // namespace rex
