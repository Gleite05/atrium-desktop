#include "shell/vpn.h"

#include "localization.h"
#include "log.h"

#include <windows.h>
#include <ras.h>
#include <raserror.h>
#include <shellapi.h>

#include <algorithm>
#include <cwctype>

namespace atrium {

namespace {

bool SameName(const std::wstring& a, const std::wstring& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
}

// rasdial with its arguments, hidden; its exit code, or -1 when it does not finish in time.
int RunRasdial(const std::wstring& args, DWORD timeoutMs)
{
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\rasdial.exe\" " + args;
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"vpn: rasdial did not start (%lu)", GetLastError());
        return -2;
    }
    int code = -1;
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) {
        DWORD exit = 0;
        GetExitCodeProcess(pi.hProcess, &exit);
        code = int(exit);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring Quoted(const std::wstring& name) { return L"\"" + name + L"\""; }

} // namespace

std::vector<std::wstring> ConnectedVpn()
{
    std::vector<std::wstring> out;
    std::vector<RASCONNW> conns(8);
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (auto& c : conns)
            c.dwSize = sizeof(RASCONNW);
        DWORD bytes = DWORD(conns.size() * sizeof(RASCONNW)), count = 0;
        DWORD r = RasEnumConnectionsW(conns.data(), &bytes, &count);
        if (r == ERROR_BUFFER_TOO_SMALL) {
            conns.resize(bytes / sizeof(RASCONNW) + 1);
            continue;
        }
        if (r == ERROR_SUCCESS)
            for (DWORD i = 0; i < count; ++i)
                out.emplace_back(conns[i].szEntryName);
        break;
    }
    return out;
}

std::vector<VpnEntry> ListVpn()
{
    std::vector<VpnEntry> out;
    std::vector<RASENTRYNAMEW> names(16);
    for (int attempt = 0; attempt < 3; ++attempt) {
        for (auto& n : names)
            n.dwSize = sizeof(RASENTRYNAMEW);
        DWORD bytes = DWORD(names.size() * sizeof(RASENTRYNAMEW)), count = 0;
        DWORD r = RasEnumEntriesW(nullptr, nullptr, names.data(), &bytes, &count);
        if (r == ERROR_BUFFER_TOO_SMALL) {
            names.resize(bytes / sizeof(RASENTRYNAMEW) + 1);
            continue;
        }
        if (r == ERROR_SUCCESS)
            for (DWORD i = 0; i < count; ++i)
                if (std::ranges::none_of(out, [&](const VpnEntry& e) { return SameName(e.name, names[i].szEntryName); }))
                    out.push_back({names[i].szEntryName, false});
        break;
    }
    const auto connected = ConnectedVpn();
    for (auto& e : out)
        e.connected = std::ranges::any_of(connected, [&](const std::wstring& c) { return SameName(c, e.name); });
    return out;
}

int ConnectVpn(const std::wstring& name)
{
    int code = RunRasdial(Quoted(name), 90000);
    Log(L"vpn: connect -> %d", code);
    return code;
}

int DisconnectVpn(const std::wstring& name)
{
    int code = RunRasdial(Quoted(name) + L" /disconnect", 20000);
    Log(L"vpn: disconnect -> %d", code);
    return code;
}

void PromptVpn(const std::wstring& name) { ShellExecuteW(nullptr, L"open", L"rasphone.exe", (L"-d " + Quoted(name)).c_str(), nullptr, SW_SHOWNORMAL); }

bool VpnNeedsInput(int code) { return code == ERROR_AUTHENTICATION_FAILURE || code == ERROR_INTERACTIVE_MODE || code == ERROR_PPP_TIMEOUT; }

std::wstring VpnError(int code)
{
    switch (code) {
    case -1: return Tr(L"No answer");
    case 809: return Tr(L"The server did not answer");
    case 868: return Tr(L"Server name not found");
    case 13801:
    case 13806: return Tr(L"Certificate problem");
    default: return TrFormat(L"Error %d", code);
    }
}

} // namespace atrium
