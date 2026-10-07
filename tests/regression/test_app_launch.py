"""Execute the production LaunchModule body with deferred UI and guest-thread fixtures."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
parser.add_argument('--compiler', default='clang++')
parser.add_argument('--source', type=Path)
args = parser.parse_args()
sdk = Path(__file__).resolve().parents[2]
source = (args.source or sdk / 'src/ui/rex_app.cpp').read_text()
start = source.index('void ReXApp::LaunchModule() {')
opening = source.index('{', start)
depth, end = 1, opening + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
body = source[start:end]

code = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#define REXLOG_INFO(...) ((void)0)
#define REXLOG_ERROR(...) ((void)0)
namespace rex::system { struct AchievementEvent {}; }
namespace ui {
struct AchievementNotificationDialog {
  void Push(const rex::system::AchievementEvent&) {}
};
struct ShaderCompileNoticeDialog {
  template<class Callback> ShaderCompileNoticeDialog(void*, Callback) {}
};
}
struct UIContext {
  std::thread::id ui_thread = std::this_thread::get_id();
  std::function<void()> pending;
  bool quit = false;
  bool CallInUIThreadDeferred(std::function<void()> callback) {
    assert(!pending); pending = std::move(callback); return true;
  }
  void RunPending() {
    assert(std::this_thread::get_id() == ui_thread && pending);
    auto callback = std::move(pending); pending = {}; callback();
  }
  bool HasQuitFromUIThread() const {
    assert(std::this_thread::get_id() == ui_thread); return quit;
  }
  void QuitFromUIThread() {
    assert(std::this_thread::get_id() == ui_thread); quit = true;
  }
  bool CallInUIThread(std::function<void()> callback) {
    assert(std::this_thread::get_id() == ui_thread); callback(); return true;
  }
};
struct GuestThread {
  unsigned resumed = 0, waited = 0;
  void Resume() { ++resumed; }
  void Wait(int, int, int, void*) { ++waited; }
};
struct KernelState { uint32_t title_id() { return 0x5752084B; } };
struct Graphics {
  unsigned storage_initialized = 0;
  bool GetCompileProgress(uint32_t&, uint32_t&) { return false; }
  void InitializeShaderStorage(const std::string& root, uint32_t title, bool blocking) {
    assert(root == "cache" && title == 0x5752084B && blocking);
    ++storage_initialized;
  }
};
struct Runtime {
  unsigned prepared = 0;
  bool preparation_fails = false;
  KernelState kernel;
  Graphics graphics;
  std::shared_ptr<GuestThread> thread = std::make_shared<GuestThread>();
  std::shared_ptr<GuestThread> PrepareModuleLaunch() {
    ++prepared; return preparation_fails ? nullptr : thread;
  }
  KernelState* kernel_state() { return &kernel; }
  Graphics* graphics_system() { return &graphics; }
  std::string cache_root() { return "cache"; }
};
struct Achievements {
  unsigned registered = 0;
  unsigned RegisterNotificationCallback(
      std::function<void(const rex::system::AchievementEvent&)>) {
    return ++registered;
  }
};
struct ReXApp {
  UIContext context;
  Achievements achievement_manager;
  std::shared_ptr<ui::AchievementNotificationDialog> achievement_notification_;
  unsigned achievement_notification_listener_ = 0;
  std::unique_ptr<ui::ShaderCompileNoticeDialog> shader_compile_notice_;
  std::unique_ptr<int> imgui_drawer_ = std::make_unique<int>(1);
  std::unique_ptr<Runtime> runtime_ = std::make_unique<Runtime>();
  std::atomic<bool> shutting_down_{true};
  std::thread module_thread_;
  bool hook_quits = false;
  unsigned pre_launch = 0, post_launch = 0, guest_exits = 0;
  ~ReXApp() { Join(); }
  void Join() { if (module_thread_.joinable()) module_thread_.join(); }
  UIContext& app_context() { return context; }
  Achievements& achievements() { return achievement_manager; }
  ui::AchievementNotificationDialog* CreateAchievementNotificationDialog() {
    return new ui::AchievementNotificationDialog;
  }
  void OnPreLaunchModule() {
    ++pre_launch; if (hook_quits) context.QuitFromUIThread();
  }
  void OnPostLaunchModule(GuestThread* thread) {
    assert(thread == runtime_->thread.get()); ++post_launch;
  }
  void OnGuestThreadExit(GuestThread* thread) {
    assert(thread == runtime_->thread.get()); ++guest_exits;
  }
  void LaunchModule();
};
''' + body + r'''
int main() {
  for (unsigned scenario = 0; scenario < 3; ++scenario) {
    ReXApp app;
    app.hook_quits = scenario == 1;
    app.runtime_->preparation_fails = scenario == 2;
    app.LaunchModule();
    assert(app.context.pending && app.pre_launch == 0 && app.runtime_->prepared == 0);
    app.context.RunPending();
    app.Join();
    assert(app.pre_launch == 1 && app.achievement_manager.registered == 1);
    assert(app.achievement_notification_ && app.shader_compile_notice_);
    if (scenario == 0) {
      assert(!app.context.quit && app.runtime_->prepared == 1);
      assert(app.runtime_->graphics.storage_initialized == 1);
      assert(app.post_launch == 1 && app.guest_exits == 1);
      assert(app.runtime_->thread->resumed == 1 && app.runtime_->thread->waited == 1);
    } else {
      assert(app.context.quit && app.runtime_->prepared == (scenario == 2 ? 1u : 0u));
      assert(app.runtime_->graphics.storage_initialized == 0);
      assert(app.post_launch == 0 && app.guest_exits == 0);
      assert(app.runtime_->thread->resumed == 0 && app.runtime_->thread->waited == 0);
    }
  }
}
'''
args.output.mkdir(parents=True, exist_ok=True)
cpp = args.output.resolve() / 'app-launch.cpp'
binary = args.output.resolve() / 'app-launch.exe'
cpp.write_text(code)
subprocess.run([args.compiler, '-std=c++20', '-UNDEBUG', '-pthread', str(cpp),
                '-o', str(binary)], check=True, timeout=45)
subprocess.run([str(binary)], check=True, timeout=10)
print('PASS: production LaunchModule preserves successful deferred launch and '
      'preparation failure; prelaunch UI quit skips preparation, storage, hooks and resume')
