// Windows notifications as Atrium cards, through UserNotificationListener
// (on by default since v0.6, D24). Unpackaged processes cannot subscribe to its
// change event, so a worker thread reads the toast list every second and posts
// new toasts, and toasts whose text changed, to the UI thread. The first read
// only records what is already there. While access is unavailable, it checks
// for access changes every ten seconds instead of ending the reader.
// It never dismisses or changes anything
// in Windows, so the system banners still appear unless the user turns an
// app's banners off in Windows (keeping it in the notification centre).
#pragma once

#include "notice.h"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

// A toast in Windows' notification centre now: who sent it and when (for the bar's counts).
struct ToastEntry {
    std::wstring appId; // the sender's AUMID (empty: unpackaged, unknown)
    std::wstring app;   // its display name
    ULONGLONG created = 0; // GetTickCount64() when it arrived
};

class NotificationListener {
public:
    // Posts `msg` to `notify` with a heap-allocated Notice* in lParam (the receiver deletes it).
    // With `toastsMsg`, also posts a std::vector<ToastEntry>* (every toast there) when the set changes.
    NotificationListener(HWND notify, UINT msg, UINT toastsMsg = 0);
    ~NotificationListener();
    NotificationListener(const NotificationListener&) = delete;
    NotificationListener& operator=(const NotificationListener&) = delete;

private:
    void Run();
    HWND notify_;
    UINT msg_;
    UINT toastsMsg_ = 0;
    std::atomic<bool> stop_{false};
    HANDLE wake_ = nullptr;
    std::thread thread_;
};

} // namespace atrium
