#include "shell/now_playing.h"

#include "log.h"

#include <shcore.h>
#include <wincodec.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cwctype>
#include <utility>

namespace atrium {

namespace {

using namespace winrt::Windows::Media::Control;
namespace streams = winrt::Windows::Storage::Streams;

constexpr auto kEvery = std::chrono::milliseconds(1000);
constexpr UINT kArtPx = 320; // decoded once per track, drawn at up to 196 DIP

bool Contains(const std::wstring& s, const wchar_t* part)
{
    std::wstring a = s, b = part;
    std::ranges::transform(a, a.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    std::ranges::transform(b, b.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
    return a.find(b) != std::wstring::npos;
}

double Seconds(winrt::Windows::Foundation::TimeSpan t) { return std::chrono::duration<double>(t).count(); }

// The session to show: Spotify's when it has one, else the one Windows calls current.
GlobalSystemMediaTransportControlsSession Pick(const GlobalSystemMediaTransportControlsSessionManager& manager)
{
    for (const auto& s : manager.GetSessions())
        if (Contains(std::wstring(s.SourceAppUserModelId()), L"spotify"))
            return s;
    return manager.GetCurrentSession();
}

// The thumbnail's picture, cropped square and scaled to kArtPx, premultiplied BGRA.
IconImage DecodeArt(const streams::IRandomAccessStreamReference& ref)
{
    IconImage out;
    if (!ref)
        return out;
    try {
        streams::IRandomAccessStreamWithContentType stream = ref.OpenReadAsync().get();
        winrt::com_ptr<IStream> com;
        if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(com.put()))))
            return out;
        winrt::com_ptr<IWICImagingFactory> f;
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))) ||
            FAILED(f->CreateDecoderFromStream(com.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
            FAILED(decoder->GetFrame(0, frame.put())))
            return out;
        UINT sw = 0, sh = 0;
        if (FAILED(frame->GetSize(&sw, &sh)) || !sw || !sh)
            return out;
        // Spotify's thumbnails are square; others (a video's) are cropped to their middle.
        const UINT side = std::min(sw, sh);
        WICRect crop{INT((sw - side) / 2), INT((sh - side) / 2), INT(side), INT(side)};
        winrt::com_ptr<IWICBitmapClipper> clip;
        winrt::com_ptr<IWICBitmapScaler> scale;
        winrt::com_ptr<IWICFormatConverter> conv;
        if (FAILED(f->CreateBitmapClipper(clip.put())) || FAILED(clip->Initialize(frame.get(), &crop)) || FAILED(f->CreateBitmapScaler(scale.put())) ||
            FAILED(scale->Initialize(clip.get(), kArtPx, kArtPx, WICBitmapInterpolationModeHighQualityCubic)) || FAILED(f->CreateFormatConverter(conv.put())) ||
            FAILED(conv->Initialize(scale.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            return out;
        out.bgra.resize(size_t(kArtPx) * kArtPx * 4);
        if (FAILED(conv->CopyPixels(nullptr, kArtPx * 4, UINT(out.bgra.size()), out.bgra.data()))) {
            out.bgra.clear();
            return out;
        }
        out.width = out.height = int(kArtPx);
    } catch (...) {
        out = {};
    }
    return out;
}

} // namespace

double NowPlaying::PositionNow() const
{
    double p = position;
    if (playing && at)
        p += double(GetTickCount64() - at) / 1000.0;
    return duration > 0 ? std::clamp(p, 0.0, duration) : std::max(0.0, p);
}

bool NowPlaying::IsSpotify() const { return Contains(app, L"spotify"); }

MediaSource::MediaSource(HWND notify, UINT message) : notify_(notify), message_(message), thread_([this] { Run(); }) {}

MediaSource::~MediaSource()
{
    {
        std::lock_guard g(lock_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void MediaSource::SetActive(bool on)
{
    {
        std::lock_guard g(lock_);
        if (active_ == on)
            return;
        active_ = on;
    }
    wake_.notify_all();
}

NowPlaying MediaSource::Latest()
{
    std::lock_guard g(lock_);
    return latest_;
}

void MediaSource::Do(MediaAction action)
{
    {
        std::lock_guard g(lock_);
        action_ = action;
    }
    wake_.notify_all();
}

void MediaSource::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Atrium now playing");
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
    std::wstring artFor; // the track whose art was decoded
    std::shared_ptr<const IconImage> art;
    unsigned artId = 0;
    for (;;) {
        std::optional<MediaAction> action;
        {
            std::unique_lock g(lock_);
            wake_.wait(g, [&] { return quit_ || active_ || action_; });
            if (quit_)
                break;
            action = std::exchange(action_, std::nullopt);
        }
        NowPlaying now;
        try {
            if (!manager)
                manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            GlobalSystemMediaTransportControlsSession s{nullptr};
            if (manager)
                s = Pick(manager);
            if (s) {
                if (action) {
                    if (*action == MediaAction::PlayPause)
                        s.TryTogglePlayPauseAsync().get();
                    else if (*action == MediaAction::Next)
                        s.TrySkipNextAsync().get();
                    else
                        s.TrySkipPreviousAsync().get();
                    Sleep(350); // the app updates its session a moment later
                }
                now.present = true;
                now.app = s.SourceAppUserModelId();
                auto props = s.TryGetMediaPropertiesAsync().get();
                now.title = props.Title();
                now.artist = props.Artist();
                now.album = props.AlbumTitle();
                now.playing = s.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                auto tl = s.GetTimelineProperties();
                now.duration = std::max(0.0, Seconds(tl.EndTime() - tl.StartTime()));
                now.position = Seconds(tl.Position());
                // The position is as of the app's last update: carried on to now while it plays.
                const double since = std::chrono::duration<double>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                if (now.playing && since > 0 && (now.duration <= 0 || since < now.duration))
                    now.position += since;
                now.at = GetTickCount64();
                const std::wstring key = now.app + L"\n" + now.title + L"\n" + now.artist + L"\n" + now.album;
                if (key != artFor) {
                    IconImage img = DecodeArt(props.Thumbnail());
                    art = img.Empty() ? nullptr : std::make_shared<const IconImage>(std::move(img));
                    ++artId;
                    artFor = key;
                }
                now.art = art;
                now.artId = artId;
            }
        } catch (...) {
            manager = nullptr; // asked again next time (the service restarted)
        }
        bool changed;
        {
            std::lock_guard g(lock_);
            const NowPlaying& was = latest_;
            changed = now.present != was.present || now.app != was.app || now.title != was.title || now.artist != was.artist || now.album != was.album ||
                      now.playing != was.playing || std::abs(now.duration - was.duration) > 0.5 || now.artId != was.artId ||
                      std::abs(now.PositionNow() - was.PositionNow()) > 2.0;
            latest_ = std::move(now);
        }
        if (changed)
            PostMessageW(notify_, message_, 0, 0);
        std::unique_lock g(lock_);
        wake_.wait_for(g, kEvery, [&] { return quit_ || action_.has_value(); });
        if (quit_)
            break;
    }
    manager = nullptr;
    winrt::uninit_apartment();
}

} // namespace atrium
