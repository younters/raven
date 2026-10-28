#include <winsock2.h>
#include <ws2spi.h>
#include <windows.h>

void entry(void)
{
    DWORD bytes = 0;
    int error = 123;
    WSASetLastError(4321);
    if (WSCEnumProtocols(NULL, NULL, &bytes, &error) != SOCKET_ERROR || error != WSAENOBUFS ||
        bytes != 4 * sizeof(WSAPROTOCOL_INFOW) || WSAGetLastError() != 4321) ExitProcess(10);
    WSAPROTOCOL_INFOW info[4];
    error = 123;
    if (WSCEnumProtocols(NULL, info, &bytes, &error) != 4 || error != 123) ExitProcess(11);
    for (int i = 0; i < 4; ++i)
        if (info[i].ProtocolChain.ChainLen != 1 || (info[i].dwProviderFlags & PFL_HIDDEN)) ExitProcess(12);
    int protocols[] = {IPPROTO_TCP, 0};
    bytes = sizeof(info);
    if (WSCEnumProtocols(protocols, info, &bytes, &error) != 2 ||
        info[0].iProtocol != IPPROTO_TCP || info[1].iProtocol != IPPROTO_TCP) ExitProcess(13);
    if (WSCEnumProtocols(NULL, info, NULL, &error) != SOCKET_ERROR || error != WSAEFAULT) ExitProcess(14);
    HANDLE file = CreateFileA("C:\\socket_catalog.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD written;
    const char text[] = "OK: SPI sizing, catalog, filtering and error output";
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, text, sizeof(text)-1, &written, NULL)) ExitProcess(15);
    CloseHandle(file); ExitProcess(0);
}
