/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <atomic>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "xenia/app/discord/discord_presence.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/config.h"
#include "xenia/debug/ui/debug_window.h"
#include "xenia/emulator.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context.h"
#include "xenia/vfs/devices/host_path_device.h"

// Available audio systems:
#include "xenia/apu/nop/nop_audio_system.h"
#include "xenia/apu/sdl/sdl_audio_system.h"
#if XE_PLATFORM_WIN32
#include "xenia/apu/xaudio2/xaudio2_audio_system.h"
#endif  // XE_PLATFORM_WIN32

// Available graphics systems:
#include "xenia/gpu/null/null_graphics_system.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#if XE_PLATFORM_WIN32
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#endif  // XE_PLATFORM_WIN32

// Available input drivers:
#include "xenia/hid/nop/nop_hid.h"
#include "xenia/hid/sdl/sdl_hid.h"
#if XE_PLATFORM_WIN32
#include "xenia/hid/winkey/winkey_hid.h"
#include "xenia/hid/xinput/xinput_hid.h"
#endif  // XE_PLATFORM_WIN32

#include "third_party/fmt/include/fmt/format.h"

DEFINE_string(apu, "any", "Audio system. Use: [any, nop, sdl, xaudio2]", "APU");
DEFINE_string(gpu, "any", "Graphics system. Use: [any, d3d12, vulkan, null]",
              "GPU");
DEFINE_string(hid, "any", "Input system. Use: [any, nop, sdl, winkey, xinput]",
              "HID");

DEFINE_bool(fullscreen, false, "Toggles fullscreen", "GPU");

DEFINE_path(
    storage_root, "",
    "Root path for persistent internal data storage (config, etc.), or empty "
    "to use the path preferred for the OS, such as the documents folder, or "
    "the emulator executable directory if portable.txt is present in it.",
    "Storage");
DEFINE_path(
    content_root, "",
    "Root path for guest content storage (saves, etc.), or empty to use the "
    "content folder under the storage root.",
    "Storage");
DEFINE_path(
    cache_root, "",
    "Root path for files used to speed up certain parts of the emulator or the "
    "game. These files may be persistent, but they can be deleted without "
    "major side effects such as progress loss. If empty, the cache folder "
    "under the storage root, or, if available, the cache directory preferred "
    "for the OS, will be used.",
    "Storage");

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, false, "Enable cache mount", "Storage");

DEFINE_transient_path(target, "",
                      "Specifies the target .xex or .iso to execute.",
                      "General");
DEFINE_transient_bool(portable, false,
                      "Specifies if Xenia should run in portable mode.",
                      "General");

DECLARE_bool(debug);

DEFINE_bool(discord, true, "Enable Discord rich presence", "General");

namespace xe {
namespace app {

class EmulatorApp final : public xe::ui::WindowedApp {
 public:
  static std::unique_ptr<xe::ui::WindowedApp> Create(
      xe::ui::WindowedAppContext& app_context) {
    return std::unique_ptr<xe::ui::WindowedApp>(new EmulatorApp(app_context));
  }

  ~EmulatorApp();

  bool OnInitialize() override;

 protected:
  void OnDestroy() override;

 private:
  template <typename T, typename... Args>
  class Factory {
   private:
    struct Creator {
      std::string name;
      std::function<bool()> is_available;
      std::function<std::unique_ptr<T>(Args...)> instantiate;
    };

    std::vector<Creator> creators_;

   public:
    void Add(const std::string_view name, std::function<bool()> is_available,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      creators_.push_back({std::string(name), is_available, instantiate});
    }

    void Add(const std::string_view name,
             std::function<std::unique_ptr<T>(Args...)> instantiate) {
      auto always_available = []() { return true; };
      Add(name, always_available, instantiate);
    }

    template <typename DT>
    void Add(const std::string_view name) {
      Add(name, DT::IsAvailable, [](Args... args) {
        return std::make_unique<DT>(std::forward<Args>(args)...);
      });
    }

    std::unique_ptr<T> Create(const std::string_view name, Args... args) {
      if (!name.empty() && name != "any") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });
        if (it != creators_.cend() && (*it).is_available()) {
          return (*it).instantiate(std::forward<Args>(args)...);
        }
        return nullptr;
      } else {
        for (const auto& creator : creators_) {
          if (!creator.is_available()) continue;
          auto instance = creator.instantiate(std::forward<Args>(args)...);
          if (!instance) continue;
          return instance;
        }
        return nullptr;
      }
    }

    std::vector<std::unique_ptr<T>> CreateAll(const std::string_view name,
                                              Args... args) {
      std::vector<std::unique_ptr<T>> instances;
      if (!name.empty() && name != "any") {
        auto it = std::find_if(
            creators_.cbegin(), creators_.cend(),
            [&name](const auto& f) { return name.compare(f.name) == 0; });
        if (it != creators_.cend() && (*it).is_available()) {
          auto instance = (*it).instantiate(std::forward<Args>(args)...);
          if (instance) {
            instances.emplace_back(std::move(instance));
          }
        }
      } else {
        for (const auto& creator : creators_) {
          if (!creator.is_available()) continue;
          auto instance = creator.instantiate(std::forward<Args>(args)...);
          if (instance) {
            instances.emplace_back(std::move(instance));
          }
        }
      }
      return instances;
    }
  };

  explicit EmulatorApp(xe::ui::WindowedAppContext& app_context);

  static std::unique_ptr<apu::AudioSystem> CreateAudioSystem(
      cpu::Processor* processor);
  static std::unique_ptr<gpu::GraphicsSystem> CreateGraphicsSystem();
  static std::vector<std::unique_ptr<hid::InputDriver>> CreateInputDrivers(
      ui::Window* window);

  void EmulatorThread();
  void ShutdownEmulatorThreadFromUIThread();

  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<EmulatorWindow> emulator_window_;

  // Created on demand, used by the emulator.
  std::unique_ptr<xe::debug::ui::DebugWindow> debug_window_;

  // Refreshing the emulator - placed after its dependencies.
  std::atomic<bool> emulator_thread_quit_requested_;
  std::unique_ptr<xe::threading::Event> emulator_thread_event_;
  std::thread emulator_thread_;
};

EmulatorApp::EmulatorApp(xe::ui::WindowedAppContext& app_context)
    : xe::ui::WindowedApp(app_context, "xenia", "[Path to .iso/.xex]") {
  AddPositionalOption("target");
}

EmulatorApp::~EmulatorApp() {
  // Should be shut down from OnDestroy if OnInitialize has ever been done, but
  // for the most safety as a running thread may be destroyed only after
  // joining.
  ShutdownEmulatorThreadFromUIThread();
}

std::unique_ptr<apu::AudioSystem> EmulatorApp::CreateAudioSystem(
    cpu::Processor* processor) {
  Factory<apu::AudioSystem, cpu::Processor*> factory;
#if XE_PLATFORM_WIN32
  factory.Add<apu::xaudio2::XAudio2AudioSystem>("xaudio2");
#endif  // XE_PLATFORM_WIN32
  factory.Add<apu::sdl::SDLAudioSystem>("sdl");
  factory.Add<apu::nop::NopAudioSystem>("nop");
  return factory.Create(cvars::apu, processor);
}

std::unique_ptr<gpu::GraphicsSystem> EmulatorApp::CreateGraphicsSystem() {
  Factory<gpu::GraphicsSystem> factory;
#if XE_PLATFORM_WIN32
  factory.Add<gpu::d3d12::D3D12GraphicsSystem>("d3d12");
#endif  // XE_PLATFORM_WIN32
  factory.Add<gpu::vulkan::VulkanGraphicsSystem>("vulkan");
  factory.Add<gpu::null::NullGraphicsSystem>("null");
  return factory.Create(cvars::gpu);
}

std::vector<std::unique_ptr<hid::InputDriver>> EmulatorApp::CreateInputDrivers(
    ui::Window* window) {
  std::vector<std::unique_ptr<hid::InputDriver>> drivers;
  if (cvars::hid.compare("nop") == 0) {
    drivers.emplace_back(xe::hid::nop::Create(window));
  } else {
    Factory<hid::InputDriver, ui::Window*> factory;
    factory.Add("sdl", xe::hid::sdl::Create);
#if XE_PLATFORM_WIN32
    factory.Add("xinput", xe::hid::xinput::Create);
    // WinKey input driver should always be the last input driver added!
    factory.Add("winkey", xe::hid::winkey::Create);
#endif  // XE_PLATFORM_WIN32
    for (auto& driver : factory.CreateAll(cvars::hid, window)) {
      if (XSUCCEEDED(driver->Setup(drivers))) {
        drivers.emplace_back(std::move(driver));
      }
    }
    if (drivers.empty()) {
      // Fallback to nop if none created.
      drivers.emplace_back(xe::hid::nop::Create(window));
    }
  }
  return drivers;
}

bool EmulatorApp::OnInitialize() {
  Profiler::Initialize();
  Profiler::ThreadEnter("Main");

  // Figure out where internal files and content should go.
  std::filesystem::path storage_root = cvars::storage_root;
  if (storage_root.empty()) {
    storage_root = xe::filesystem::GetExecutableFolder();
    if (!cvars::portable &&
        !std::filesystem::exists(storage_root / "portable.txt")) {
      storage_root = xe::filesystem::GetUserFolder();
#if defined(XE_PLATFORM_WIN32) || defined(XE_PLATFORM_GNU_LINUX)
      storage_root = storage_root / "Xenia";
#else
      // TODO(Triang3l): Point to the app's external storage "files" directory
      // on Android.
#warning Unhandled platform for the data root.
      storage_root = storage_root / "Xenia";
#endif
    }
  }
  storage_root = std::filesystem::absolute(storage_root);
  XELOGI("Storage root: {}", xe::path_to_utf8(storage_root));

  config::SetupConfig(storage_root);

  std::filesystem::path content_root = cvars::content_root;
  if (content_root.empty()) {
    content_root = storage_root / "content";
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!content_root.is_absolute()) {
      content_root = storage_root / content_root;
    }
  }
  content_root = std::filesystem::absolute(content_root);
  XELOGI("Content root: {}", xe::path_to_utf8(content_root));

  std::filesystem::path cache_root = cvars::cache_root;
  if (cache_root.empty()) {
    cache_root = storage_root / "cache";
    // TODO(Triang3l): Point to the app's external storage "cache" directory on
    // Android.
  } else {
    // If content root isn't an absolute path, then it should be relative to the
    // storage root.
    if (!cache_root.is_absolute()) {
      cache_root = storage_root / cache_root;
    }
  }
  cache_root = std::filesystem::absolute(cache_root);
  XELOGI("Cache root: {}", xe::path_to_utf8(cache_root));

  if (cvars::discord) {
    discord::DiscordPresence::Initialize();
    discord::DiscordPresence::NotPlaying();
  }

  // Create the emulator but don't initialize so we can setup the window.
  emulator_ =
      std::make_unique<Emulator>("", storage_root, content_root, cache_root);

  // Main emulator display window.
  emulator_window_ = EmulatorWindow::Create(emulator_.get(), app_context());
  if (!emulator_window_) {
    XELOGE("Failed to create the main emulator window");
    return false;
  }

  // Setup the emulator and run its loop in a separate thread.
  emulator_thread_quit_requested_.store(false, std::memory_order_relaxed);
  emulator_thread_event_ = xe::threading::Event::CreateAutoResetEvent(false);
  emulator_thread_ = std::thread(&EmulatorApp::EmulatorThread, this);

  return true;
}

void EmulatorApp::OnDestroy() {
  ShutdownEmulatorThreadFromUIThread();

  if (cvars::discord) {
    discord::DiscordPresence::Shutdown();
  }

  Profiler::Dump();
  // The profiler needs to shut down before the graphics context.
  Profiler::Shutdown();

  // TODO(DrChat): Remove this code and do a proper exit.
  XELOGI("Cheap-skate exit!");
  std::quick_exit(EXIT_SUCCESS);
}

void EmulatorApp::EmulatorThread() {
  assert_not_null(emulator_thread_event_);

  xe::threading::set_name("Emulator");
  Profiler::ThreadEnter("Emulator");

  // Setup and initialize all subsystems. If we can't do something
  // (unsupported system, memory issues, etc) this will fail early.
  X_STATUS result =
      emulator_->Setup(emulator_window_->window(), CreateAudioSystem,
                       CreateGraphicsSystem, CreateInputDrivers);
  if (XFAILED(result)) {
    XELOGE("Failed to setup emulator: {:08X}", result);
    app_context().RequestDeferredQuit();
    return;
  }

  if (cvars::mount_scratch) {
    auto scratch_device = std::make_unique<xe::vfs::HostPathDevice>(
        "\\SCRATCH", "scratch", false);
    if (!scratch_device->Initialize()) {
      XELOGE("Unable to scan scratch path");
    } else {
      if (!emulator_->file_system()->RegisterDevice(
              std::move(scratch_device))) {
        XELOGE("Unable to register scratch path");
      } else {
        emulator_->file_system()->RegisterSymbolicLink("scratch:", "\\SCRATCH");
      }
    }
  }

  if (cvars::mount_cache) {
    auto cache0_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\CACHE0", "cache0", false);
    if (!cache0_device->Initialize()) {
      XELOGE("Unable to scan cache0 path");
    } else {
      if (!emulator_->file_system()->RegisterDevice(std::move(cache0_device))) {
        XELOGE("Unable to register cache0 path");
      } else {
        emulator_->file_system()->RegisterSymbolicLink("cache0:", "\\CACHE0");
      }
    }

    auto cache1_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\CACHE1", "cache1", false);
    if (!cache1_device->Initialize()) {
      XELOGE("Unable to scan cache1 path");
    } else {
      if (!emulator_->file_system()->RegisterDevice(std::move(cache1_device))) {
        XELOGE("Unable to register cache1 path");
      } else {
        emulator_->file_system()->RegisterSymbolicLink("cache1:", "\\CACHE1");
      }
    }

    // Some (older?) games try accessing cache:\ too
    // NOTE: this must be registered _after_ the cache0/cache1 devices, due to
    // substring/start_with logic inside VirtualFileSystem::ResolvePath, else
    // accesses to those devices will go here instead
    auto cache_device =
        std::make_unique<xe::vfs::HostPathDevice>("\\CACHE", "cache", false);
    if (!cache_device->Initialize()) {
      XELOGE("Unable to scan cache path");
    } else {
      if (!emulator_->file_system()->RegisterDevice(std::move(cache_device))) {
        XELOGE("Unable to register cache path");
      } else {
        emulator_->file_system()->RegisterSymbolicLink("cache:", "\\CACHE");
      }
    }
  }

  // Set a debug handler.
  // This will respond to debugging requests so we can open the debug UI.
  if (cvars::debug) {
    emulator_->processor()->set_debug_listener_request_handler(
        [this](xe::cpu::Processor* processor) {
          if (debug_window_) {
            return debug_window_.get();
          }
          app_context().CallInUIThreadSynchronous([this]() {
            debug_window_ = xe::debug::ui::DebugWindow::Create(emulator_.get(),
                                                               app_context());
            debug_window_->window()->on_closed.AddListener(
                [this](xe::ui::UIEvent* e) {
                  emulator_->processor()->set_debug_listener(nullptr);
                  app_context().CallInUIThread(
                      [this]() { debug_window_.reset(); });
                });
          });
          // If failed to enqueue the UI thread call, this will just be null.
          return debug_window_.get();
        });
  }

  emulator_->on_launch.AddListener([&](auto title_id, const auto& game_title) {
    if (cvars::discord) {
      discord::DiscordPresence::PlayingTitle(
          game_title.empty() ? "Unknown Title" : std::string(game_title));
    }
    app_context().CallInUIThread([this]() { emulator_window_->UpdateTitle(); });
    emulator_thread_event_->Set();
  });

  emulator_->on_shader_storage_initialization.AddListener(
      [this](bool initializing) {
        app_context().CallInUIThread([this, initializing]() {
          emulator_window_->SetInitializingShaderStorage(initializing);
        });
      });

  emulator_->on_terminate.AddListener([]() {
    if (cvars::discord) {
      discord::DiscordPresence::NotPlaying();
    }
  });

  // Enable the main menu now that the emulator is properly loaded
  app_context().CallInUIThread(
      [this]() { emulator_window_->window()->EnableMainMenu(); });

  // Grab path from the flag or unnamed argument.
  std::filesystem::path path;
  if (!cvars::target.empty()) {
    path = cvars::target;
  }

  // Toggles fullscreen
  if (cvars::fullscreen) {
    app_context().CallInUIThread(
        [this]() { emulator_window_->ToggleFullscreen(); });
  }

  if (!path.empty()) {
    // Normalize the path and make absolute.
    auto abs_path = std::filesystem::absolute(path);
    result = emulator_->LaunchPath(abs_path);
    if (XFAILED(result)) {
      xe::FatalError(fmt::format("Failed to launch target: {:08X}", result));
      app_context().RequestDeferredQuit();
      return;
    }
  }

  // Now, we're going to use this thread to drive events related to emulation.
  while (!emulator_thread_quit_requested_.load(std::memory_order_relaxed)) {
    xe::threading::Wait(emulator_thread_event_.get(), false);
    while (true) {
      emulator_->WaitUntilExit();
      if (emulator_->TitleRequested()) {
        emulator_->LaunchNextTitle();
      } else {
        break;
      }
    }
  }
}

void EmulatorApp::ShutdownEmulatorThreadFromUIThread() {
  // TODO(Triang3l): Proper shutdown of the emulator (relying on std::quick_exit
  // for now) - currently WaitUntilExit loops forever otherwise (plus possibly
  // lots of other things not shutting down correctly now). Some parts of the
  // code call the regular std::exit, which seems to be calling destructors (at
  // least on Linux), so the entire join is currently commented out.
#if 0
  // Same thread as the one created it, to make sure there's zero possibility of
  // a race with the creation of the emulator thread.
  assert_true(app_context().IsInUIThread());
  emulator_thread_quit_requested_.store(true, std::memory_order_relaxed);
  if (!emulator_thread_.joinable()) {
    return;
  }
  emulator_thread_event_->Set();
  emulator_thread_.join();
#endif
}

}  // namespace app
}  // namespace xe

XE_DEFINE_WINDOWED_APP(xenia, xe::app::EmulatorApp::Create);
