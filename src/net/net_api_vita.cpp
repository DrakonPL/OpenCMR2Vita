// Network play on the PS Vita (port/net.h). Stubs: no provider and no session.
// ENet's unix.c does not build on the vitasdk newlib yet.

#include "port/net.h"

HRESULT Net_EnumProviders(NetProviderCallback, void *) { return NET_OK; }
HRESULT Net_InitializeConnection(int) { return NET_ERR_UNAVAILABLE; }
HRESULT Net_EnumSessions(const NetGuid *, DWORD, NetSessionCallback, void *) { return NET_ERR_UNAVAILABLE; }
HRESULT Net_Open(const NetSessionDesc *, DWORD) { return NET_ERR_UNAVAILABLE; }
HRESULT Net_Close(void) { return NET_OK; }
HRESULT Net_GetSessionDesc(NetSessionDesc *) { return NET_ERR_NOCONNECTION; }
HRESULT Net_SetSessionDesc(const NetSessionDesc *) { return NET_ERR_NOCONNECTION; }
HRESULT Net_CreatePlayer(NetPlayerID *, const NetName *, const void *, DWORD) { return NET_ERR_NOCONNECTION; }
HRESULT Net_DestroyPlayer(NetPlayerID) { return NET_ERR_NOCONNECTION; }
HRESULT Net_EnumPlayers(NetPlayerCallback, void *) { return NET_ERR_NOCONNECTION; }
HRESULT Net_SetPlayerData(NetPlayerID, const void *, DWORD) { return NET_ERR_NOCONNECTION; }
HRESULT Net_Send(NetPlayerID, NetPlayerID, BOOL, const void *, DWORD) { return NET_ERR_NOCONNECTION; }
HRESULT Net_Receive(NetPlayerID *, NetPlayerID *, void *, DWORD *) { return NET_ERR_NOMESSAGES; }
