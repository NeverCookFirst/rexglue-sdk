/**
 * @file        ui/ui_sound.cpp
 * @brief       UI sound playback. See rex/ui/ui_sound.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#include <rex/ui/ui_sound.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include <rex/logging.h>

namespace rex::ui {

namespace {

struct CachedSound {
  SDL_AudioSpec spec{};
  std::vector<Uint8> samples;
  bool valid = false;
};

std::mutex g_mutex;
SDL_AudioStream* g_stream = nullptr;
SDL_AudioSpec g_stream_spec{};
std::unordered_map<std::string, CachedSound> g_cache;

// Loads and caches, or returns the cached entry (including a cached failure, so
// a missing file is reported once rather than every time an achievement fires).
const CachedSound& GetSound(const std::string& key, const std::filesystem::path& path) {
  auto it = g_cache.find(key);
  if (it != g_cache.end()) {
    return it->second;
  }

  CachedSound sound;
  SDL_AudioSpec spec{};
  Uint8* buffer = nullptr;
  Uint32 length = 0;
  if (SDL_LoadWAV(path.string().c_str(), &spec, &buffer, &length)) {
    sound.spec = spec;
    sound.samples.assign(buffer, buffer + length);
    sound.valid = true;
    SDL_free(buffer);
    REXLOG_INFO("UI sound loaded: {} ({} Hz, {} ch, {} bytes)", path.string(), spec.freq,
                spec.channels, length);
  } else {
    REXLOG_WARN("UI sound unavailable: {} ({})", path.string(), SDL_GetError());
  }
  return g_cache.emplace(key, std::move(sound)).first->second;
}

}  // namespace

bool PlayUiSound(const std::filesystem::path& wav_path) {
  std::lock_guard<std::mutex> lock(g_mutex);

  const std::string key = wav_path.string();
  const CachedSound& sound = GetSound(key, wav_path);
  if (!sound.valid) {
    return false;
  }

  // One device stream, opened for the first sound's format. A later sound in a
  // different format would need its own stream; every sound the UI ships is
  // converted to one format at build time, so this stays a single stream.
  if (g_stream && (g_stream_spec.freq != sound.spec.freq ||
                   g_stream_spec.format != sound.spec.format ||
                   g_stream_spec.channels != sound.spec.channels)) {
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
  }
  if (!g_stream) {
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sound.spec, nullptr,
                                         nullptr);
    if (!g_stream) {
      REXLOG_WARN("UI sound: SDL_OpenAudioDeviceStream failed ({})", SDL_GetError());
      return false;
    }
    g_stream_spec = sound.spec;
    SDL_ResumeAudioStreamDevice(g_stream);
  }

  if (!SDL_PutAudioStreamData(g_stream, sound.samples.data(),
                              static_cast<int>(sound.samples.size()))) {
    REXLOG_WARN("UI sound: SDL_PutAudioStreamData failed ({})", SDL_GetError());
    return false;
  }
  return true;
}

void ShutdownUiSound() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_stream) {
    SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
  }
  g_cache.clear();
}

}  // namespace rex::ui
