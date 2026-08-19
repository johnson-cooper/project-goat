#pragma once

// Owns every loaded/streamed audio resource for goat-client-rl, plus the
// current music/SFX volume, so screen-painting and duel code never touch
// raylib's PlaySound/PlayMusicStream directly or need to know which file on
// disk backs which event — that one mapping lives in AudioManager.cpp
// (music_path/sound_path) so replacing a sound file later never touches
// gameplay code. Scoped to goat-client-rl only; the Win32 client and the
// rules engine own no audio at all.
//
// Deliberately not copyable: it owns raylib Music/Sound handles, which must
// be unloaded exactly once, and AppState only ever holds one by reference
// anyway (see main.cpp's single `AppState state;`).

#include <raylib.h>

#include <unordered_map>

namespace goat::audio {

enum class MusicTrack { None, Menu, Duel };

// One entry per SFX this project actually has a file for — see
// AudioManager.cpp's sound_path(). Deliberately doesn't include every verb
// the rules engine can report (Activate, Attack, Change Position, ...):
// external/sound/fx has no asset for those yet, and this project's own
// policy is "map what exists, don't invent what doesn't."
enum class SoundEffect {
    SummonMonster,   // also used for Special Summon — no dedicated asset for that verb
    SetCard,         // monster set and spell/trap set share this one file
    Destroy,
    LifePointDamage,
    PackOpen,
    CardReveal,
    UiClick,
};

class AudioManager {
public:
    AudioManager() = default;
    ~AudioManager();
    AudioManager(const AudioManager&) = delete;
    AudioManager& operator=(const AudioManager&) = delete;

    // Initializes the audio device, loads whichever of external/sound's
    // ost/fx files actually exist on this machine (a missing file just
    // leaves that track/effect silently unavailable — see loadMusic/
    // loadSound), and restores the persisted music/SFX volume from
    // saves/audio.cfg (sensible defaults if that file is missing or
    // malformed). Always returns true today (matches raylib's own
    // InitAudioDevice, which has no failure signal to check) — kept
    // bool-returning so a future real failure path has somewhere to report.
    bool initialize();

    // Unloads every loaded Music/Sound and closes the audio device. Must
    // run before CloseWindow(). Safe to call more than once (e.g. an
    // explicit shutdown at the end of main() followed by AppState's own
    // destructor running ~AudioManager) and safe even if initialize() was
    // never called.
    void shutdown();

    // Advances music stream buffering and any in-progress fade. Call
    // exactly once per frame regardless of screen — a cheap no-op when no
    // music is currently loaded/playing.
    void update();

    // Cross-fades to `track` over a short fade. A no-op if `track` is
    // already playing or already the fade target, so callers (see
    // main.cpp's desired_music_for) can call this unconditionally every
    // frame without ever restarting the current track.
    void requestMusic(MusicTrack track);
    void stopMusic();

    // Fires-and-forgets one instance of `effect`. Silently does nothing if
    // that effect's file wasn't found at startup — missing audio must never
    // block or crash gameplay. Overlaps freely with music and with other
    // SFX; does not stop whatever's already playing.
    void playSound(SoundEffect effect);

    void setMusicVolume(float volume); // 0..1, clamped; persists immediately
    void setSfxVolume(float volume);   // 0..1, clamped; persists immediately
    float musicVolume() const { return musicVolume_; }
    float sfxVolume() const { return sfxVolume_; }

private:
    void loadMusic(MusicTrack track, const char* path);
    void loadSound(SoundEffect effect, const char* path);
    Music* activeMusic();
    void applyMusicVolume();

    bool initialized_ = false;
    std::unordered_map<MusicTrack, Music> musicTracks_;
    std::unordered_map<SoundEffect, Sound> soundEffects_;

    float musicVolume_ = 0.7f;
    float sfxVolume_ = 0.8f;

    MusicTrack currentTrack_ = MusicTrack::None; // actually loaded/playing right now
    MusicTrack targetTrack_ = MusicTrack::None;  // what requestMusic most recently asked for

    enum class FadeState { Idle, FadingOut, FadingIn };
    FadeState fadeState_ = FadeState::Idle;
    float fadeElapsed_ = 0.0f;
};

// The shared button() widget (main.cpp) plays a UI click through this free
// function instead of taking an AudioManager parameter itself, which would
// otherwise have to thread through every one of its ~36 call sites across
// every screen for one optional sound. `set_ui_click_source` is called once
// at startup, right after AudioManager::initialize(); before that (or if
// it's never called) play_ui_click() is just a no-op.
void set_ui_click_source(AudioManager* manager);
void play_ui_click();

} // namespace goat::audio
