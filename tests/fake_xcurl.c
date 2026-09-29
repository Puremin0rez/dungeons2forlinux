/* Stand-in for the game's XCurl.dll: wrappers around the WinHTTP imports xgameruntime.dll hooks. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

__declspec(dllexport) void *fx_open(void)
{
    return WinHttpOpen(L"fake", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
}
__declspec(dllexport) BOOL fx_close(void *h) { return WinHttpCloseHandle(h); }
__declspec(dllexport) BOOL fx_set_option(void *h, DWORD opt, void *buf, DWORD len)
{
    return WinHttpSetOption(h, opt, buf, len);
}
__declspec(dllexport) void *fx_connect(void *s, const WCHAR *host, unsigned short port)
{
    return WinHttpConnect(s, host, port, 0);
}
__declspec(dllexport) void *fx_open_request(void *c, const WCHAR *path)
{
    return WinHttpOpenRequest(c, L"GET", path, NULL, NULL, NULL, 0);
}
__declspec(dllexport) BOOL fx_send(void *r)
{
    return WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0);
}
__declspec(dllexport) BOOL fx_receive(void *r) { return WinHttpReceiveResponse(r, NULL); }
__declspec(dllexport) BOOL fx_read(void *r, void *buf, DWORD n, DWORD *got)
{
    return WinHttpReadData(r, buf, n, got);
}
__declspec(dllexport) BOOL fx_query(void *r, DWORD level, void *buf, DWORD *len)
{
    return WinHttpQueryHeaders(r, level, NULL, buf, len, NULL);
}
