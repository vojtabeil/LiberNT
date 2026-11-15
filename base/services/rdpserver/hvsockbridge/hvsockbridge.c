/*
 * PROJECT:     LiberNT RDP console server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Bridge Hyper-V socket RDP connections to the local RDP server
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>
 */

/*
 * The enhanced session of Virtual Machine Connection reaches a guest through
 * a Hyper-V socket on the RDP service GUID. This service accepts those
 * connections from \\.\HvSocket and forwards each one to the RDP server on
 * 127.0.0.1:3389.
 *
 * It listens only while the RDP server accepts TCP connections: the host
 * offers the enhanced session as soon as a listener answers its probe, and
 * that must not happen with no server behind the bridge.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdarg.h>
#include "hvsockioctl.h"

#define SERVER_PORT     3389
#define PROBE_INTERVAL  2000
#define BUFFER_SIZE     16384

static const GUID RdpServiceId =
    { 0x00000d3d, 0xfacb, 0x11e6, { 0xbd, 0x58, 0x64, 0x00, 0x6a, 0x79, 0x86, 0xd3 } };

static WCHAR ServiceName[] = L"hvsockbridge";
static SERVICE_STATUS_HANDLE ServiceHandle;
static HANDLE StopEvent;
static volatile LONG Connections;

typedef struct _BRIDGE
{
    HANDLE Socket;      /* Hyper-V socket connection */
    SOCKET Tcp;
} BRIDGE, *PBRIDGE;

static void Log(const char *Format, ...)
{
    char Buffer[256];
    va_list Args;

    va_start(Args, Format);
    _vsnprintf(Buffer, sizeof(Buffer) - 2, Format, Args);
    va_end(Args);
    Buffer[sizeof(Buffer) - 2] = 0;
    strcat(Buffer, "\n");
    OutputDebugStringA(Buffer);
}

static HANDLE OpenHvSocket(void)
{
    return CreateFileW(HVSOCK_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, NULL);
}

/* One overlapped request on a Hyper-V socket handle; FALSE when it failed or the service stops */
static BOOL HvRequest(HANDLE Handle, DWORD Code, BOOL Write, PVOID Buffer, DWORD Length,
                      PVOID Output, DWORD OutputLength, PDWORD Done, BOOL StopOnEvent)
{
    OVERLAPPED Overlapped = {0};
    HANDLE Events[2];
    BOOL Ok;

    Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!Overlapped.hEvent)
        return FALSE;

    if (Code != 0)
        Ok = DeviceIoControl(Handle, Code, Buffer, Length, Output, OutputLength, NULL, &Overlapped);
    else if (Write)
        Ok = WriteFile(Handle, Buffer, Length, NULL, &Overlapped);
    else
        Ok = ReadFile(Handle, Buffer, Length, NULL, &Overlapped);

    if (!Ok && GetLastError() == ERROR_IO_PENDING)
    {
        Events[0] = Overlapped.hEvent;
        Events[1] = StopEvent;
        if (WaitForMultipleObjects(StopOnEvent ? 2 : 1, Events, FALSE, INFINITE) != WAIT_OBJECT_0)
            CancelIo(Handle);
        Ok = TRUE;
    }

    if (Ok)
        Ok = GetOverlappedResult(Handle, &Overlapped, Done, TRUE);

    CloseHandle(Overlapped.hEvent);
    return Ok;
}

static SOCKET ConnectServer(void)
{
    struct sockaddr_in Address = {0};
    SOCKET Tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (Tcp == INVALID_SOCKET)
        return INVALID_SOCKET;

    Address.sin_family = AF_INET;
    Address.sin_port = htons(SERVER_PORT);
    Address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(Tcp, (struct sockaddr *)&Address, sizeof(Address)) != 0)
    {
        closesocket(Tcp);
        return INVALID_SOCKET;
    }

    return Tcp;
}

static BOOL ServerReachable(void)
{
    SOCKET Tcp = ConnectServer();

    if (Tcp == INVALID_SOCKET)
        return FALSE;
    closesocket(Tcp);
    return TRUE;
}

/* Hyper-V socket to TCP; the reverse direction runs in the bridge thread */
static DWORD WINAPI HostToServer(PVOID Context)
{
    PBRIDGE Bridge = Context;
    char Buffer[BUFFER_SIZE];
    DWORD Length;
    int Sent;

    while (HvRequest(Bridge->Socket, 0, FALSE, Buffer, sizeof(Buffer), NULL, 0, &Length, FALSE) && Length != 0)
    {
        for (Sent = 0; Sent < (int)Length; )
        {
            int Count = send(Bridge->Tcp, Buffer + Sent, (int)Length - Sent, 0);
            if (Count <= 0)
                goto Done;
            Sent += Count;
        }
    }

Done:
    shutdown(Bridge->Tcp, SD_SEND);
    return 0;
}

static DWORD WINAPI BridgeThread(PVOID Context)
{
    PBRIDGE Bridge = Context;
    char Buffer[BUFFER_SIZE];
    HANDLE Reader;
    DWORD Done;
    int Length;

    Reader = CreateThread(NULL, 0, HostToServer, Bridge, 0, NULL);
    if (Reader != NULL)
    {
        while ((Length = recv(Bridge->Tcp, Buffer, sizeof(Buffer), 0)) > 0)
        {
            if (!HvRequest(Bridge->Socket, 0, TRUE, Buffer, (DWORD)Length, NULL, 0, &Done, FALSE))
                break;
        }

        /* Closing the Hyper-V socket ends the stream for the host and fails the pending read */
        CloseHandle(Bridge->Socket);
        closesocket(Bridge->Tcp);
        WaitForSingleObject(Reader, INFINITE);
        CloseHandle(Reader);
    }
    else
    {
        CloseHandle(Bridge->Socket);
        closesocket(Bridge->Tcp);
    }

    Log("hvsockbridge: connection closed");
    HeapFree(GetProcessHeap(), 0, Bridge);
    InterlockedDecrement(&Connections);
    return 0;
}

/* Accept connections until the service stops or the server goes away */
static void Serve(HANDLE Listener)
{
    HVSOCK_ACCEPT_INFO Info;
    PBRIDGE Bridge;
    HANDLE Socket;
    HANDLE Thread;
    SOCKET Tcp;
    DWORD Done;

    while (WaitForSingleObject(StopEvent, 0) != WAIT_OBJECT_0)
    {
        Socket = OpenHvSocket();
        if (Socket == INVALID_HANDLE_VALUE)
            return;

        if (!HvRequest(Socket, IOCTL_HVSOCK_ACCEPT, FALSE, (PVOID)&RdpServiceId, sizeof(RdpServiceId),
                       &Info, sizeof(Info), &Done, TRUE))
        {
            CloseHandle(Socket);
            return;
        }

        Tcp = ConnectServer();
        if (Tcp == INVALID_SOCKET)
        {
            Log("hvsockbridge: the RDP server does not answer, no longer listening");
            CloseHandle(Socket);
            return;
        }

        Bridge = HeapAlloc(GetProcessHeap(), 0, sizeof(*Bridge));
        if (Bridge == NULL)
        {
            closesocket(Tcp);
            CloseHandle(Socket);
            continue;
        }
        Bridge->Socket = Socket;
        Bridge->Tcp = Tcp;

        InterlockedIncrement(&Connections);
        Thread = CreateThread(NULL, 0, BridgeThread, Bridge, 0, NULL);
        if (Thread == NULL)
        {
            InterlockedDecrement(&Connections);
            closesocket(Tcp);
            CloseHandle(Socket);
            HeapFree(GetProcessHeap(), 0, Bridge);
            continue;
        }
        CloseHandle(Thread);
        Log("hvsockbridge: connection %08lx forwarded", Info.InstanceId.Data1);
    }
}

static DWORD WINAPI MainThread(PVOID Context)
{
    HANDLE Listener;
    DWORD Done;

    UNREFERENCED_PARAMETER(Context);

    while (WaitForSingleObject(StopEvent, PROBE_INTERVAL) == WAIT_TIMEOUT)
    {
        if (!ServerReachable())
            continue;

        Listener = OpenHvSocket();
        if (Listener == INVALID_HANDLE_VALUE)
        {
            Log("hvsockbridge: \\\\.\\HvSocket unavailable, error %lu", GetLastError());
            continue;
        }

        if (HvRequest(Listener, IOCTL_HVSOCK_LISTEN, FALSE, (PVOID)&RdpServiceId, sizeof(RdpServiceId),
                      NULL, 0, &Done, FALSE))
        {
            Log("hvsockbridge: listening for enhanced session connections");
            Serve(Listener);
        }
        CloseHandle(Listener);
    }

    return 0;
}

static DWORD WINAPI ServiceHandler(DWORD Control, DWORD EventType, LPVOID EventData, LPVOID Context)
{
    SERVICE_STATUS Status = {0};

    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(Context);

    if (Control != SERVICE_CONTROL_STOP && Control != SERVICE_CONTROL_SHUTDOWN)
        return Control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;

    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Status.dwCurrentState = SERVICE_STOP_PENDING;
    Status.dwWaitHint = 5000;
    SetServiceStatus(ServiceHandle, &Status);
    SetEvent(StopEvent);
    return NO_ERROR;
}

static void WINAPI ServiceMain(DWORD Argc, LPWSTR *Argv)
{
    SERVICE_STATUS Status = {0};
    WSADATA Winsock;
    HANDLE Thread;

    UNREFERENCED_PARAMETER(Argc);
    UNREFERENCED_PARAMETER(Argv);

    StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    ServiceHandle = RegisterServiceCtrlHandlerExW(ServiceName, ServiceHandler, NULL);
    if (!ServiceHandle || !StopEvent)
        return;

    Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    Status.dwCurrentState = SERVICE_RUNNING;
    Status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(ServiceHandle, &Status);

    if (WSAStartup(MAKEWORD(2, 2), &Winsock) == 0)
    {
        Thread = CreateThread(NULL, 0, MainThread, NULL, 0, NULL);
        if (Thread != NULL)
        {
            WaitForSingleObject(Thread, INFINITE);
            CloseHandle(Thread);
        }
        WSACleanup();
    }

    Status.dwCurrentState = SERVICE_STOPPED;
    Status.dwControlsAccepted = 0;
    SetServiceStatus(ServiceHandle, &Status);
}

int wmain(int argc, WCHAR **argv)
{
    static const SERVICE_TABLE_ENTRYW Table[] =
    {
        { ServiceName, ServiceMain },
        { NULL, NULL }
    };

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    StartServiceCtrlDispatcherW(Table);
    return 0;
}
