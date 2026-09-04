/**
 * @file        rex/ui/ui_sound.h
 * @brief       Playback of short host-side UI sounds (achievement toast, ...).
 *
 * Separate from rex::audio, which exists solely to service the guest's XMA
 * buffers: its entry point takes a guest memory address and its mixer is fed by
 * the emulated APU. Host UI sounds have no guest side at all, so they get their
 * own small SDL playback stream here.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */
#pragma once

#include <filesystem>

namespace rex::ui {

// Plays a WAV file, mixing over anything already playing. Returns false if the
// file is missing or unreadable. The first call opens the playback device; the
// decoded samples are cached, so repeat plays of the same file cost nothing but
// a memcpy. Safe to call from any thread. Never blocks on the audio device: a
// failure to play is logged once per path and ignored - a missing UI sound must
// never take the game down.
bool PlayUiSound(const std::filesystem::path& wav_path);

// Releases the playback device and the sample cache. Called during UI shutdown.
void ShutdownUiSound();

}  // namespace rex::ui
