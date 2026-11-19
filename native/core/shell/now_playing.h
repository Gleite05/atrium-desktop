// What is playing, for the media widget: Windows' media sessions (the ones the
// volume flyout and the lock screen show), Spotify's first when it has one.
// Read on a worker thread once a second while such a widget is on the desktop;
// the album art is decoded when the track changes.
#pragma once

#include "icon_image.h"

#include <windows.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace atrium {

struct NowPlaying {
    bool present = false;    // some app has a media session
    std::wstring app;        // its AppUserModelID (Spotify.exe, SpotifyAB.SpotifyMusic_...)
    std::wstring title, artist, album;
    bool playing = false;
    double position = 0;     // seconds, at `at`
    double duration = 0;     // seconds; 0: unknown
    ULONGLONG at = 0;        // GetTickCount64() when `position` was true
    std::shared_ptr<const IconImage> art; // the album art, square; null: none
    unsigned artId = 0;      // changes with `art`
    // Where the track is now (moves on while it plays).
    double PositionNow() const;
    bool IsSpotify() const;
};

enum class MediaAction { PlayPause, Next, Previous };

class MediaSource {
public:
    // `message` is posted to `notify` when Latest() changes.
    MediaSource(HWND notify, UINT message);
    ~MediaSource();
    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;

    void SetActive(bool on);
    NowPlaying Latest();
    void Do(MediaAction action);

private:
    void Run();

    HWND notify_;
    UINT message_;
    std::mutex lock_;
    std::condition_variable wake_;
    bool active_ = false, quit_ = false;
    std::optional<MediaAction> action_;
    NowPlaying latest_;
    std::thread thread_;
};

} // namespace atrium
