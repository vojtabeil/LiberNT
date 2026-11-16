/*
 * PROJECT:     LiberNT Hyper-V VMBus Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hyper-V socket (hv_sock) interface for user mode
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

/*
 * \\.\HvSocket accepts Hyper-V socket connections that the host opens to a
 * service GUID in this VM. Only LocalSystem may open it.
 *
 * 1. One handle registers a listener: IOCTL_HVSOCK_LISTEN with the service
 *    GUID. Connections are accepted while that handle stays open.
 * 2. Each new handle takes one connection: IOCTL_HVSOCK_ACCEPT with the
 *    service GUID waits for it and returns an HVSOCK_ACCEPT_INFO.
 * 3. ReadFile and WriteFile then carry the byte stream. A read that returns
 *    0 bytes means the host closed its side. Closing the handle closes the
 *    connection. Open the handles for overlapped I/O to read and write at
 *    the same time.
 */

#pragma once

#define HVSOCK_DEVICE_NAME      L"\\Device\\HvSocket"
#define HVSOCK_DOS_DEVICE_NAME  L"\\DosDevices\\HvSocket"
#define HVSOCK_WIN32_NAME       L"\\\\.\\HvSocket"

#define IOCTL_HVSOCK_LISTEN \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_HVSOCK_ACCEPT \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _HVSOCK_ACCEPT_INFO
{
    GUID ServiceId;
    GUID InstanceId;
} HVSOCK_ACCEPT_INFO, *PHVSOCK_ACCEPT_INFO;
