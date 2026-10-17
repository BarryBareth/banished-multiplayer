#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Minimal Steamworks access through the game's own steam_api64.dll (2017 SDK, C++ interfaces only).
// Interfaces are fetched by version string via ISteamClient::GetISteamGenericInterface; the vtable layouts are
// declared by hand after the public Steamworks SDK interfaces (SteamMatchMaking009, SteamNetworking006,
// SteamFriends017) and Valve's GameNetworkingSockets (SteamNetworkingMessages002 / Utils004). No SDK files are needed.
namespace steam {
bool Init();                                   // lazy; false if Steam isn't running / the API isn't loaded
void RunCallbacks();                           // main thread, every frame
uint64_t MySteamId();
std::string PersonaName();

// Lobby (friends-only). Results arrive through OnEvent.
void CreateLobby(int maxMembers);
void JoinLobby(uint64_t lobby);
void LeaveLobby();
void OpenInviteOverlay();
uint64_t Lobby();                              // current lobby, 0 if none
uint64_t LobbyOwner();
int LobbyMembers();
void SetLobbyData(const char* key, const char* value);

enum Event { kLobbyCreated, kLobbyEntered, kLobbyFailed, kJoinRequested, kP2PRequest, kP2PFailed, kMemberChange };
// kJoinRequested: a = lobby the user accepted an invite to; kP2PRequest/kP2PFailed: a = remote user
void SetEventHandler(std::function<void(Event, uint64_t a)> fn);

// P2P (reliable, channel 0). Packets are delivered whole.
bool Send(uint64_t to, const void* data, uint32_t size);
bool Receive(std::string& out, uint64_t& from);  // poll; true while packets are waiting
void Accept(uint64_t remote);
void Close(uint64_t remote);
bool RelayOnly();                              // Valve relays only (SteamNetworkingMessages: no peer sees an IP)
int RelayStatus();                             // Valve relay network: 100 = available, 1-3 = starting, <0 = trouble
// Rich presence (status = friends list "View game info"; group = players shown together; connect = "Join Game").
// Empty status clears it. Repeated identical calls are free.
void SetPresence(const std::string& status, const std::string& group, int groupSize, const std::string& connect);
}
