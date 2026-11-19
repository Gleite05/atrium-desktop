// Windows' own VPN connections (Settings > Network & Internet > VPN): the
// phonebook entries of the user and of all users, which of them are connected,
// and connecting or disconnecting one. Connecting goes through rasdial with the
// credentials Windows keeps for the entry; when it has none, Windows' own
// dialog asks for them (rasphone -d).
#pragma once

#include <string>
#include <vector>

namespace atrium {

struct VpnEntry {
    std::wstring name;
    bool connected = false;
};

// Every VPN entry, in the phonebook's order, with its state. Fast (no network).
std::vector<VpnEntry> ListVpn();
// The names of the connected VPNs.
std::vector<std::wstring> ConnectedVpn();
// Blocking: rasdial's exit code (0 connected; 691, 703, 718: credentials wanted;
// -1: no answer in time). Call off the UI thread.
int ConnectVpn(const std::wstring& name);
int DisconnectVpn(const std::wstring& name);
// The connection needs the user's input (credentials): Windows' dialog for it.
void PromptVpn(const std::wstring& name);
bool VpnNeedsInput(int code);
// A short, user-facing reason for a failed attempt.
std::wstring VpnError(int code);

} // namespace atrium
