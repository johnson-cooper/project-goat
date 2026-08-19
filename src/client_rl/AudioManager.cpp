#include "AudioManager.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

namespace goat::audio {

namespace {
namespace fs = std::filesystem;

// Cross-fade duration for menu<->duel music transitions — see the top-level
// task's "0.25-0.75 seconds is enough" guidance.
constexpr float kFadeSeconds = 0.5f;
constexpr const char* kSettingsPath = "saves/audio.cfg";

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// The one place mapping each music/SFX event to the actual file under
// external/sound — everything else in this project refers to events by
// enum, never by filename, so replacing a track later never touches
// gameplay/UI code. Filenames match what's actually present in this
// project's local external/sound/ checkout (inspected before writing this),
// not an assumed convention.
const char* music_path(MusicTrack track) {
    switch (track) {
        case MusicTrack::Menu: return "external/sound/ost/menu.mp3";
        case MusicTrack::Duel: return "external/sound/ost/duel.mp3";
        case MusicTrack::None: return "";
    }
    return "";
}

const char* sound_path(SoundEffect effect) {
    switch (effect) {
        case SoundEffect::SummonMonster:   return "external/sound/fx/summon_monster.wav";
        case SoundEffect::SetCard:         return "external/sound/fx/set_card.wav";
        case SoundEffect::Destroy:         return "external/sound/fx/card_destroyed.wav";
        case SoundEffect::LifePointDamage: return "external/sound/fx/lifepoints.wav";
        case SoundEffect::PackOpen:        return "external/sound/fx/pack_open.wav";
        case SoundEffect::CardReveal:      return "external/sound/fx/card_reveal.wav";
        case SoundEffect::UiClick:         return "external/sound/fx/menu_button_select.wav";
    }
    return "";
}

struct VolumeSettings { float music = 0.7f; float sfx = 0.8f; };

// Never throws and never leaves a volume out of range: a missing file (first
// run), a hand-edited garbage value, or a future format change all just fall
// back to defaults for that one field instead of taking the client down
// before it even opens a window.
VolumeSettings load_settings() {
    VolumeSettings settings;
    std::ifstream input(kSettingsPath);
    if (!input) return settings;
    std::string line;
    while (std::getline(input, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals);
        const std::string value = line.substr(equals + 1);
        try {
            if (key == "music_volume") settings.music = clamp01(std::stof(value));
            else if (key == "sfx_volume") settings.sfx = clamp01(value.empty() ? 0.0f : std::stof(value));
        } catch (const std::exception&) {
            // Malformed number on this one line — keep the default (or
            // whatever was already parsed) and keep reading the rest.
        }
    }
    return settings;
}

// Best-effort save: a read-only/missing saves/ directory degrades to
// "settings just don't persist this run" rather than crashing the client.
void save_settings(const VolumeSettings& settings) {
    std::error_code ignored;
    fs::create_directories("saves", ignored);
    std::ofstream output(kSettingsPath, std::ios::trunc);
    if (!output) return;
    output << "music_volume=" << settings.music << '\n';
    output << "sfx_volume=" << settings.sfx << '\n';
}

AudioManager* g_ui_click_source = nullptr;

} // namespace

AudioManager::~AudioManager() { shutdown(); }

bool AudioManager::initialize() {
    if (initialized_) return true;
    InitAudioDevice();
    initialized_ = true;

    loadMusic(MusicTrack::Menu, music_path(MusicTrack::Menu));
    loadMusic(MusicTrack::Duel, music_path(MusicTrack::Duel));
    loadSound(SoundEffect::SummonMonster, sound_path(SoundEffect::SummonMonster));
    loadSound(SoundEffect::SetCard, sound_path(SoundEffect::SetCard));
    loadSound(SoundEffect::Destroy, sound_path(SoundEffect::Destroy));
    loadSound(SoundEffect::LifePointDamage, sound_path(SoundEffect::LifePointDamage));
    loadSound(SoundEffect::PackOpen, sound_path(SoundEffect::PackOpen));
    loadSound(SoundEffect::CardReveal, sound_path(SoundEffect::CardReveal));
    loadSound(SoundEffect::UiClick, sound_path(SoundEffect::UiClick));

    const VolumeSettings settings = load_settings();
    musicVolume_ = settings.music;
    sfxVolume_ = settings.sfx;
    return true;
}

void AudioManager::loadMusic(MusicTrack track, const char* path) {
    Music music{};
    if (FileExists(path)) {
        music = LoadMusicStream(path);
        if (IsMusicValid(music)) music.looping = true;
    }
    if (!IsMusicValid(music)) TraceLog(LOG_WARNING, "AudioManager: music not found or failed to load: %s", path);
    musicTracks_[track] = music;
}

void AudioManager::loadSound(SoundEffect effect, const char* path) {
    Sound sound{};
    if (FileExists(path)) sound = LoadSound(path);
    if (!IsSoundValid(sound)) TraceLog(LOG_WARNING, "AudioManager: sound effect not found or failed to load: %s", path);
    soundEffects_[effect] = sound;
}

void AudioManager::shutdown() {
    if (!initialized_) return;
    if (g_ui_click_source == this) g_ui_click_source = nullptr;
    for (auto& [track, music] : musicTracks_) if (IsMusicValid(music)) UnloadMusicStream(music);
    musicTracks_.clear();
    for (auto& [effect, sound] : soundEffects_) if (IsSoundValid(sound)) UnloadSound(sound);
    soundEffects_.clear();
    CloseAudioDevice();
    initialized_ = false;
    currentTrack_ = MusicTrack::None;
    targetTrack_ = MusicTrack::None;
    fadeState_ = FadeState::Idle;
}

Music* AudioManager::activeMusic() {
    const auto it = musicTracks_.find(currentTrack_);
    if (it == musicTracks_.end() || !IsMusicValid(it->second)) return nullptr;
    return &it->second;
}

void AudioManager::applyMusicVolume() {
    if (Music* music = activeMusic()) SetMusicVolume(*music, musicVolume_);
}

void AudioManager::requestMusic(MusicTrack track) {
    if (!initialized_ || track == targetTrack_) return; // already playing it, or already fading toward it
    targetTrack_ = track;
    if (currentTrack_ == MusicTrack::None) {
        // Nothing playing yet (fresh startup) — jump straight in instead of
        // fading out of silence.
        currentTrack_ = track;
        fadeState_ = FadeState::FadingIn;
        fadeElapsed_ = 0.0f;
        if (Music* music = activeMusic()) { SetMusicVolume(*music, 0.0f); PlayMusicStream(*music); }
    } else {
        fadeState_ = FadeState::FadingOut;
        fadeElapsed_ = 0.0f;
    }
}

void AudioManager::stopMusic() {
    if (Music* music = activeMusic()) StopMusicStream(*music);
    currentTrack_ = MusicTrack::None;
    targetTrack_ = MusicTrack::None;
    fadeState_ = FadeState::Idle;
}

void AudioManager::update() {
    if (!initialized_) return;
    if (Music* music = activeMusic()) UpdateMusicStream(*music);

    const float dt = GetFrameTime();
    if (fadeState_ == FadeState::FadingOut) {
        fadeElapsed_ += dt;
        const float t = clamp01(fadeElapsed_ / kFadeSeconds);
        if (Music* music = activeMusic()) SetMusicVolume(*music, musicVolume_ * (1.0f - t));
        if (t >= 1.0f) {
            if (Music* music = activeMusic()) StopMusicStream(*music);
            currentTrack_ = targetTrack_;
            fadeElapsed_ = 0.0f;
            fadeState_ = FadeState::FadingIn;
            if (Music* music = activeMusic()) { SetMusicVolume(*music, 0.0f); PlayMusicStream(*music); }
        }
    } else if (fadeState_ == FadeState::FadingIn) {
        fadeElapsed_ += dt;
        const float t = clamp01(fadeElapsed_ / kFadeSeconds);
        if (Music* music = activeMusic()) SetMusicVolume(*music, musicVolume_ * t);
        if (t >= 1.0f) fadeState_ = FadeState::Idle;
    }
}

void AudioManager::playSound(SoundEffect effect) {
    if (!initialized_) return;
    const auto it = soundEffects_.find(effect);
    if (it == soundEffects_.end() || !IsSoundValid(it->second)) return;
    SetSoundVolume(it->second, sfxVolume_);
    PlaySound(it->second);
}

void AudioManager::setMusicVolume(float volume) {
    musicVolume_ = clamp01(volume);
    // Only touch the active track directly outside of an in-progress fade —
    // update()'s own SetMusicVolume calls already scale by musicVolume_
    // every frame during a fade, so touching it here too would just fight
    // that and cause an audible flicker.
    if (fadeState_ == FadeState::Idle) applyMusicVolume();
    save_settings({musicVolume_, sfxVolume_});
}

void AudioManager::setSfxVolume(float volume) {
    sfxVolume_ = clamp01(volume);
    save_settings({musicVolume_, sfxVolume_});
}

void set_ui_click_source(AudioManager* manager) { g_ui_click_source = manager; }
void play_ui_click() { if (g_ui_click_source) g_ui_click_source->playSound(SoundEffect::UiClick); }

} // namespace goat::audio
