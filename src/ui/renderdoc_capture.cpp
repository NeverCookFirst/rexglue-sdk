/**
 * @see include/rex/ui/renderdoc_capture.h
 */

#include <rex/ui/renderdoc_capture.h>

#include <memory>

#include <rex/logging.h>
#include <rex/ui/renderdoc_api.h>

namespace rex {
namespace ui {

bool TriggerRenderDocCapture() {
  static std::unique_ptr<RenderDocAPI> api = RenderDocAPI::CreateIfConnected();
  if (!api || !api->api_1_0_0()) {
    REXLOG_WARN(
        "RenderDoc is not attached to this process - launch the game through "
        "RenderDoc to capture a frame");
    return false;
  }
  api->api_1_0_0()->TriggerCapture();
  REXLOG_INFO("RenderDoc capture of the next frame triggered");
  return true;
}

}  // namespace ui
}  // namespace rex
