/*
 * PROJECT:     LiberNT RDP console server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Console desktop RDP server for an explicitly configured test LAN
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>
 */

/*
 * FreeRDP implements the wire protocol and TLS. This backend captures the
 * existing interactive desktop with GDI and injects authenticated input.
 * It does not create a terminal session or replace the display driver.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <freerdp/freerdp.h>
#include <freerdp/peer.h>
#include <freerdp/listener.h>
#include <freerdp/settings.h>
#include <freerdp/constants.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <freerdp/codec/planar.h>
#include <winpr/ssl.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include "transfer.h"
#include "clipboard.h"

/* OpenSSL names its x86_64 MinGW DLLs with an -x64 suffix. */
#ifdef _WIN64
#define LIBCRYPTO_DLL "libcrypto-3-x64.dll"
#else
#define LIBCRYPTO_DLL "libcrypto-3.dll"
#endif

typedef ULONG (__cdecl *DEBUG_PRINT)(const char *, ...);
static DEBUG_PRINT DebugPrint;
static HANDLE StopEvent;
/* One session drives the console at a time; a new connection replaces it */
typedef struct CLIENT_SESSION
{
    freerdp_peer *Peer;
    BOOL Loopback;      /* the peer is the Hyper-V socket bridge */
    HANDLE Replaced;    /* set when a newer connection takes over */
} CLIENT_SESSION;
#define MAX_CLIENT_THREADS 8
static CRITICAL_SECTION SessionLock;
static CLIENT_SESSION *CurrentSession;
static HANDLE SessionSlot;
static volatile LONG ClientThreads;
static char BindAddress[64], AllowedPeer[64], Username[128], Password[256];
static char Certificate[MAX_PATH], PrivateKey[MAX_PATH];
#define MAX_INTERFACE_MACS 8
static BYTE InterfaceMac[MAX_INTERFACE_MACS][6];
static unsigned InterfaceMacCount;
static BOOL ConfigureAddress;
static IPMask InterfaceMask;
static ULONG AddressContext;
static BOOL AddressAdded;
static WCHAR TransferRoot[MAX_PATH];
static BOOL ClipboardEnabled;
static BOOL Loopback, HvSocketTrust;

typedef struct DESKTOP_CONTEXT
{
    rdpContext rdp;
    BOOL Authorized, Active, Suppressed, Refresh;
    BOOL Loopback;      /* the peer is the Hyper-V socket bridge */
    int Width, Height;
    HDC ScreenDC, MemoryDC;
    HBITMAP Bitmap;
    HGDIOBJ OldBitmap;
    BYTE *Pixels, *Previous;
    BYTE Tile[64 * 32 * 4];
    BITMAP_PLANAR_CONTEXT *Planar;
    BYTE PlanarTile[2 * 64 * 32 * 4];
    BOOL Keys[512];
    DWORD MouseButtons;
    /* Another desktop has the input or the screen (Ctrl+Alt+Del, a locked console) */
    BOOL InputBlocked, CaptureBlocked;
    ULONGLONG Frames, Updates, Inputs;
    HANDLE Vcm;
    RDP_TRANSFER *Transfer;
    RDP_CLIPBOARD *Clipboard;
} DESKTOP_CONTEXT;

static void Log(const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;
    /* The debugger line first: it does not depend on the console window. */
    if (DebugPrint) DebugPrint("RDPTEST: %s\n", buffer);
    fprintf(stdout, "RDPTEST: %s\n", buffer);
    fflush(stdout);
}

/* Clipboard events go to the kernel debugger only. Writing to the console
 * from the clipboard window while it renders data for another application
 * (WM_RENDERFORMAT) blocks in LiberNT and stalls that application. */
static void DebugLog(const char *fmt, ...)
{
    char buffer[1024];
    va_list args;
    if (!DebugPrint) return;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = 0;
    DebugPrint("RDPTEST: %s\n", buffer);
}

static BOOL InputReady(rdpInput *input)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)input->context;
    return ctx->Authorized && ctx->Active;
}

static BOOL ConfiguredInterface(const IP_ADAPTER_INFO *adapter)
{
    unsigned i;
    if (adapter->AddressLength != sizeof(InterfaceMac[0])) return FALSE;
    for (i = 0; i < InterfaceMacCount; ++i)
        if (!memcmp(adapter->Address, InterfaceMac[i], sizeof(InterfaceMac[i]))) return TRUE;
    return FALSE;
}

/* Optional, explicit test-LAN setup. Select by MAC, never by adapter order,
 * and retain existing addresses. The temporary alias is removed on exit. */
static BOOL EnsureAddress(void)
{
    IP_ADAPTER_INFO *adapters, *adapter;
    IP_ADDR_STRING *ip;
    ULONG size = 0, instance;
    DWORD status;
    if (!ConfigureAddress || AddressAdded) return TRUE;
    if (GetAdaptersInfo(NULL, &size) != ERROR_BUFFER_OVERFLOW) return FALSE;
    adapters = malloc(size);
    if (!adapters) return FALSE;
    status = GetAdaptersInfo(adapters, &size);
    if (status != NO_ERROR) { free(adapters); return FALSE; }
    for (adapter = adapters; adapter; adapter = adapter->Next)
    {
        if (!ConfiguredInterface(adapter)) continue;
        for (ip = &adapter->IpAddressList; ip; ip = ip->Next)
            if (!strcmp(ip->IpAddress.String, BindAddress))
            { free(adapters); return TRUE; }
        status = AddIPAddress(inet_addr(BindAddress), InterfaceMask,
                              adapter->Index, &AddressContext, &instance);
        Log("Test Ethernet interface %lu: address %s, AddIPAddress=%lu",
            adapter->Index, BindAddress, status);
        AddressAdded = status == NO_ERROR;
        free(adapters);
        return AddressAdded;
    }
    free(adapters);
    return FALSE;
}

/* A relative certificate or key path names a file next to the configuration,
 * so one configuration works from the live image and from an installed disk. */
static BOOL ConfigRelativePath(const char *config, char *path, size_t size)
{
    const char *separator = strrchr(config, '\\');
    size_t directory, length = strlen(path);
    if (!path[0] || path[0] == '\\' || path[0] == '/' || path[1] == ':') return TRUE;
    if (!separator) return FALSE;
    directory = (size_t)(separator - config) + 1;
    if (directory + length >= size) return FALSE;
    memmove(path + directory, path, length + 1);
    memcpy(path, config, directory);
    return TRUE;
}

/* Release the keys and mouse buttons the client left pressed */
static void ReleaseHeldInput(DESKTOP_CONTEXT *ctx)
{
    unsigned i;
    INPUT event = {0};
    for (i = 0; i < 512; ++i)
        if (ctx->Keys[i])
        {
            event.type = INPUT_KEYBOARD;
            event.ki.wScan = i & 255;
            event.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
            if (i & 256) event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            if (SendInput(1, &event, sizeof(event)) == 1) ctx->Keys[i] = FALSE;
        }
    memset(&event, 0, sizeof(event));
    event.type = INPUT_MOUSE;
    if (ctx->MouseButtons & PTR_FLAGS_BUTTON1) event.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
    if (ctx->MouseButtons & PTR_FLAGS_BUTTON2) event.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
    if (ctx->MouseButtons & PTR_FLAGS_BUTTON3) event.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP;
    if (event.mi.dwFlags && SendInput(1, &event, sizeof(event)) == 1) ctx->MouseButtons = 0;
}

/* SendInput fails while another desktop has the input: the Winlogon desktop after
 * Ctrl+Alt+Del (also sent by the client, which win32k turns into the secure attention
 * sequence) or a locked console. That must not end the session, or every reconnect
 * fails the same way until someone clears that desktop on the basic console: drop the
 * event and carry on. Key releases lost meanwhile are sent once input works again. */
static BOOL Inject(DESKTOP_CONTEXT *ctx, INPUT *event)
{
    if (ctx->InputBlocked) ReleaseHeldInput(ctx);
    if (SendInput(1, event, sizeof(*event)) != 1)
    {
        if (!ctx->InputBlocked)
            Log("Input injection failed error=%lu: another desktop has the input; dropping input until it is back",
                GetLastError());
        ctx->InputBlocked = TRUE;
        return FALSE;
    }
    if (ctx->InputBlocked)
    {
        ctx->InputBlocked = FALSE;
        Log("Input injection works again");
    }
    ++ctx->Inputs;
    return TRUE;
}

static BOOL Keyboard(rdpInput *input, UINT16 flags, UINT8 code)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)input->context;
    INPUT event = {0};
    unsigned index = code | ((flags & KBD_FLAGS_EXTENDED) ? 256 : 0);
    if (!InputReady(input)) return FALSE;
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = code;
    event.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (flags & KBD_FLAGS_EXTENDED) event.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (flags & KBD_FLAGS_RELEASE) event.ki.dwFlags |= KEYEVENTF_KEYUP;
    /* A press that did not get through is not held; a release that did not stays due */
    if (Inject(ctx, &event)) ctx->Keys[index] = !(flags & KBD_FLAGS_RELEASE);
    return TRUE;
}

static BOOL UnicodeKeyboard(rdpInput *input, UINT16 flags, UINT16 code)
{
    INPUT event = {0};
    if (!InputReady(input)) return FALSE;
    event.type = INPUT_KEYBOARD;
    event.ki.wScan = code;
    event.ki.dwFlags = KEYEVENTF_UNICODE;
    if (flags & KBD_FLAGS_RELEASE) event.ki.dwFlags |= KEYEVENTF_KEYUP;
    Inject((DESKTOP_CONTEXT *)input->context, &event);
    return TRUE;
}

static BOOL Mouse(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)input->context;
    INPUT event = {0};
    BOOL down = (flags & PTR_FLAGS_DOWN) != 0;
    if (!InputReady(input)) return FALSE;
    event.type = INPUT_MOUSE;
    if (x >= ctx->Width) x = ctx->Width - 1;
    if (y >= ctx->Height) y = ctx->Height - 1;
    event.mi.dx = MulDiv(x, 65535, ctx->Width - 1);
    event.mi.dy = MulDiv(y, 65535, ctx->Height - 1);
    event.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
    if (flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL))
    {
        LONG delta = flags & 0x1ff;
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) delta -= 0x200;
        event.mi.mouseData = (DWORD)delta;
        event.mi.dwFlags |= (flags & PTR_FLAGS_HWHEEL) ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    }
    else
    {
        if (flags & PTR_FLAGS_BUTTON1) event.mi.dwFlags |= down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        if (flags & PTR_FLAGS_BUTTON2) event.mi.dwFlags |= down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        if (flags & PTR_FLAGS_BUTTON3) event.mi.dwFlags |= down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
    }
    if (Inject(ctx, &event) && !(flags & (PTR_FLAGS_WHEEL | PTR_FLAGS_HWHEEL)))
    {
        if (down) ctx->MouseButtons |= flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3);
        else ctx->MouseButtons &= ~(flags & (PTR_FLAGS_BUTTON1 | PTR_FLAGS_BUTTON2 | PTR_FLAGS_BUTTON3));
    }
    return TRUE;
}

static BOOL Synchronize(rdpInput *input, UINT32 flags)
{
    (void)flags;
    return InputReady(input);
}

static void FreeDesktopCapture(DESKTOP_CONTEXT *ctx)
{
    if (ctx->OldBitmap) SelectObject(ctx->MemoryDC, ctx->OldBitmap);
    if (ctx->Bitmap) DeleteObject(ctx->Bitmap);
    if (ctx->MemoryDC) DeleteDC(ctx->MemoryDC);
    if (ctx->ScreenDC) ReleaseDC(NULL, ctx->ScreenDC);
    free(ctx->Previous);
    ctx->OldBitmap = NULL; ctx->Bitmap = NULL;
    ctx->MemoryDC = ctx->ScreenDC = NULL;
    ctx->Previous = ctx->Pixels = NULL;
}

static BOOL AllocateCapture(DESKTOP_CONTEXT *ctx)
{
    BITMAPINFO info = {0};
    FreeDesktopCapture(ctx);
    ctx->Width = GetSystemMetrics(SM_CXSCREEN);
    ctx->Height = GetSystemMetrics(SM_CYSCREEN);
    if (ctx->Width < 2 || ctx->Height < 2 || ctx->Width > 8192 || ctx->Height > 8192) return FALSE;
    ctx->ScreenDC = GetDC(NULL);
    if (!ctx->ScreenDC) return FALSE;
    ctx->MemoryDC = CreateCompatibleDC(ctx->ScreenDC);
    if (!ctx->MemoryDC) return FALSE;
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = ctx->Width;
    info.bmiHeader.biHeight = -ctx->Height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    ctx->Bitmap = CreateDIBSection(ctx->ScreenDC, &info, DIB_RGB_COLORS,
                                   (void **)&ctx->Pixels, NULL, 0);
    if (!ctx->Bitmap) return FALSE;
    ctx->OldBitmap = SelectObject(ctx->MemoryDC, ctx->Bitmap);
    if (!ctx->OldBitmap || ctx->OldBitmap == HGDI_ERROR) { ctx->OldBitmap = NULL; return FALSE; }
    ctx->Previous = calloc((size_t)ctx->Width * ctx->Height, 4);
    if (!ctx->Planar)
        ctx->Planar = freerdp_bitmap_planar_context_new(PLANAR_FORMAT_HEADER_RLE, 64, 32);
    ctx->Refresh = TRUE;
    return ctx->Previous != NULL;
}

static BOOL Capabilities(freerdp_peer *peer)
{
    return freerdp_settings_set_uint32(peer->context->settings, FreeRDP_DesktopWidth,
                                       GetSystemMetrics(SM_CXSCREEN)) &&
           freerdp_settings_set_uint32(peer->context->settings, FreeRDP_DesktopHeight,
                                       GetSystemMetrics(SM_CYSCREEN));
}

static BOOL PostConnect(freerdp_peer *peer)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)peer->context;
    rdpSettings *settings = ctx->rdp.settings;
    const char *user = freerdp_settings_get_string(settings, FreeRDP_Username);
    const char *password = freerdp_settings_get_string(settings, FreeRDP_Password);
    /* The enhanced session sends no guest credentials. With hvsock_trust=1 the verified
     * bridge is trusted: only the Hyper-V host can open its channel, and like the basic
     * console of Virtual Machine Connection the session is open to whoever may connect
     * to the VM there. */
    if (!(ctx->Loopback && HvSocketTrust) &&
        (!user || !password || strcmp(user, Username) || strcmp(password, Password)))
    {
        Log("Authentication rejected for peer %s", peer->hostname);
        return FALSE;
    }
    ctx->Authorized = TRUE;
    if (!AllocateCapture(ctx)) return FALSE;
    if (*TransferRoot && WTSVirtualChannelManagerIsChannelJoined(ctx->Vcm, "rdpdr"))
    {
        ctx->Transfer = TransferStart(ctx->Vcm, &ctx->rdp, TransferRoot, Log);
        if (!ctx->Transfer) { Log("File transfer channel failed to start"); return FALSE; }
    }
    /* Clipboard redirection is optional: the session goes on without it. */
    if (ClipboardEnabled && WTSVirtualChannelManagerIsChannelJoined(ctx->Vcm, "cliprdr"))
        ctx->Clipboard = ClipboardStart(ctx->Vcm, &ctx->rdp, DebugLog);
    Log("Authenticated peer %s; console desktop %dx%d", peer->hostname, ctx->Width, ctx->Height);
    return TRUE;
}

static BOOL Activate(freerdp_peer *peer)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)peer->context;
    POINTER_SYSTEM_UPDATE pointer = {0};
    if (!ctx->Authorized) return FALSE;
    ctx->Active = TRUE;
    ctx->Refresh = TRUE;
    pointer.type = SYSPTR_NULL;
    return ctx->rdp.update->pointer->PointerSystem(&ctx->rdp, &pointer);
}

static BOOL Refresh(rdpContext *rdp, BYTE count, const RECTANGLE_16 *rects)
{
    (void)count; (void)rects;
    ((DESKTOP_CONTEXT *)rdp)->Refresh = TRUE;
    return TRUE;
}

static BOOL Suppress(rdpContext *rdp, BYTE allow, const RECTANGLE_16 *area)
{
    (void)area;
    ((DESKTOP_CONTEXT *)rdp)->Suppressed = !allow;
    ((DESKTOP_CONTEXT *)rdp)->Refresh = TRUE;
    return TRUE;
}

static BOOL Capture(DESKTOP_CONTEXT *ctx)
{
    int x, y, row;
    CURSORINFO cursor = {0};
    BITMAP_DATA rectangle = {0};
    BITMAP_UPDATE update = {0};
    if (!ctx->Authorized || !ctx->Active || ctx->Suppressed) return TRUE;
    if (ctx->Width != GetSystemMetrics(SM_CXSCREEN) || ctx->Height != GetSystemMetrics(SM_CYSCREEN))
    {
        if (!AllocateCapture(ctx) ||
            !freerdp_settings_set_uint32(ctx->rdp.settings, FreeRDP_DesktopWidth, ctx->Width) ||
            !freerdp_settings_set_uint32(ctx->rdp.settings, FreeRDP_DesktopHeight, ctx->Height)) return FALSE;
        ctx->Active = FALSE;
        return ctx->rdp.update->DesktopResize(&ctx->rdp);
    }
    /* Like input, the capture can fail while another desktop is shown: skip the frame */
    if (!BitBlt(ctx->MemoryDC, 0, 0, ctx->Width, ctx->Height, ctx->ScreenDC, 0, 0, SRCCOPY | CAPTUREBLT))
    {
        if (!ctx->CaptureBlocked) Log("Desktop capture failed error=%lu; retrying", GetLastError());
        ctx->CaptureBlocked = TRUE;
        return TRUE;
    }
    if (ctx->CaptureBlocked)
    {
        ctx->CaptureBlocked = FALSE;
        ctx->Refresh = TRUE;
        Log("Desktop capture works again");
    }
    cursor.cbSize = sizeof(cursor);
    if (GetCursorInfo(&cursor) && (cursor.flags & CURSOR_SHOWING))
    {
        ICONINFO icon = {0};
        if (GetIconInfo(cursor.hCursor, &icon))
        {
            DrawIconEx(ctx->MemoryDC, cursor.ptScreenPos.x - icon.xHotspot,
                       cursor.ptScreenPos.y - icon.yHotspot, cursor.hCursor, 0, 0, 0, NULL, DI_NORMAL);
            if (icon.hbmColor) DeleteObject(icon.hbmColor);
            if (icon.hbmMask) DeleteObject(icon.hbmMask);
        }
    }
    GdiFlush();
    update.number = 1; update.rectangles = &rectangle; update.skipCompression = TRUE;
    rectangle.bitsPerPixel = 32; rectangle.bitmapDataStream = ctx->Tile;
    for (y = 0; y < ctx->Height; y += 32)
        for (x = 0; x < ctx->Width; x += 64)
        {
            int width = min(64, ctx->Width - x), height = min(32, ctx->Height - y);
            BOOL changed = ctx->Refresh;
            for (row = 0; !changed && row < height; ++row)
            {
                size_t offset = ((size_t)(y + row) * ctx->Width + x) * 4;
                changed = memcmp(ctx->Pixels + offset, ctx->Previous + offset, width * 4) != 0;
            }
            if (!changed) continue;
            rectangle.destLeft = x; rectangle.destTop = y;
            rectangle.destRight = x + width - 1; rectangle.destBottom = y + height - 1;
            rectangle.width = width; rectangle.height = height;
            /* Planar (RDP 6.0) compression: Microsoft clients end the session with a
             * protocol error on uncompressed 32 bpp bitmap updates. */
            rectangle.compressed = FALSE;
            if (ctx->Planar && width >= 4 && height >= 4)
            {
                UINT32 size = 0;
                BYTE *planar = freerdp_bitmap_compress_planar(
                    ctx->Planar, ctx->Pixels + ((size_t)y * ctx->Width + x) * 4, PIXEL_FORMAT_BGRX32,
                    width, height, ctx->Width * 4, ctx->PlanarTile, &size);
                if (planar == ctx->PlanarTile && size && size <= sizeof(ctx->PlanarTile))
                {
                    rectangle.compressed = TRUE;
                    rectangle.bitmapDataStream = ctx->PlanarTile;
                    rectangle.bitmapLength = size;
                    rectangle.cbCompMainBodySize = size;
                    rectangle.cbScanWidth = width * 4;
                    rectangle.cbUncompressedSize = width * height * 4;
                }
            }
            if (!rectangle.compressed)
            {
                for (row = 0; row < height; ++row)
                    memcpy(ctx->Tile + (size_t)row * width * 4,
                           ctx->Pixels + ((size_t)(y + height - 1 - row) * ctx->Width + x) * 4, width * 4);
                rectangle.bitmapDataStream = ctx->Tile;
                rectangle.bitmapLength = width * height * 4;
            }
            if (!ctx->rdp.update->BitmapUpdate(&ctx->rdp, &update)) return FALSE;
            ++ctx->Updates;
        }
    memcpy(ctx->Previous, ctx->Pixels, (size_t)ctx->Width * ctx->Height * 4);
    ctx->Refresh = FALSE;
    if (++ctx->Frames == 1) Log("First desktop frame sent, bitmap updates=%llu", ctx->Updates);
    return TRUE;
}

static void ContextFree(freerdp_peer *peer, rdpContext *rdp)
{
    DESKTOP_CONTEXT *ctx = (DESKTOP_CONTEXT *)rdp;
    (void)peer;
    ClipboardStop(ctx->Clipboard);
    TransferStop(ctx->Transfer);
    if (ctx->Vcm) WTSCloseServer(ctx->Vcm);
    /* A disconnected controller must not leave Ctrl/Alt or mouse buttons held. */
    ReleaseHeldInput(ctx);
    Log("Session closed: frames=%llu updates=%llu input events=%llu", ctx->Frames, ctx->Updates, ctx->Inputs);
    FreeDesktopCapture(ctx);
    freerdp_bitmap_planar_context_free(ctx->Planar);
    ctx->Planar = NULL;
}

static DWORD WINAPI ClientThread(void *arg)
{
    CLIENT_SESSION *session = arg;
    freerdp_peer *peer = session->Peer;
    rdpSettings *settings;
    rdpPrivateKey *key;
    rdpCertificate *cert;
    DWORD count, status, started, lastFrame = 0;
    HANDLE events[32];
    DESKTOP_CONTEXT *ctx;
    BOOL slot = FALSE;
    /* A new connection replaces the current session, as on a Windows console: a client
     * that reconnects, or one whose previous connection has not been torn down yet,
     * must not be turned away. Virtual Machine Connection opens its enhanced session
     * with a negotiation that fails on purpose and reconnects right away; refusing
     * that reconnect while the first connection was still closing failed the whole
     * enhanced session. */
    EnterCriticalSection(&SessionLock);
    if (CurrentSession) SetEvent(CurrentSession->Replaced);
    CurrentSession = session;
    LeaveCriticalSection(&SessionLock);
    events[0] = SessionSlot; events[1] = session->Replaced; events[2] = StopEvent;
    status = WaitForMultipleObjects(3, events, FALSE, 10000);
    slot = status == WAIT_OBJECT_0 || status == WAIT_ABANDONED_0;
    if (!slot)
    {
        if (status == WAIT_TIMEOUT) Log("Previous session did not end in time; connection dropped");
        goto done;
    }
    started = GetTickCount();
    peer->ContextSize = sizeof(DESKTOP_CONTEXT);
    peer->ContextFree = ContextFree;
    if (!freerdp_peer_context_new(peer)) goto done;
    ctx = (DESKTOP_CONTEXT *)peer->context;
    ctx->Loopback = session->Loopback;
    ctx->Vcm = WTSOpenServerA((LPSTR)peer->context);
    if (!ctx->Vcm) goto cleanup;
    settings = ctx->rdp.settings;
    key = freerdp_key_new_from_file_enc(PrivateKey, NULL);
    cert = freerdp_certificate_new_from_file(Certificate);
    if (!key || !cert) { freerdp_key_free(key); freerdp_certificate_free(cert); goto cleanup; }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1))
    { freerdp_key_free(key); freerdp_certificate_free(cert); goto cleanup; }
    if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1))
    { freerdp_certificate_free(cert); goto cleanup; }
    /* Virtual Machine Connection's enhanced session, through hvsockbridge, closes the
     * connection when the server picks TLS; it accepts standard RDP security, as with
     * xrdp. The Hyper-V socket never leaves the host, so the loopback leg is unencrypted. */
    if (ctx->Loopback &&
        (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, TRUE) ||
         !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE) ||
         !freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, TRUE) ||
         !freerdp_settings_set_uint32(settings, FreeRDP_EncryptionLevel, ENCRYPTION_LEVEL_NONE) ||
         !freerdp_settings_set_uint32(settings, FreeRDP_EncryptionMethods, ENCRYPTION_METHOD_NONE)))
        goto cleanup;
    if (!ctx->Loopback &&
        (!freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE) ||
         !freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE)))
        goto cleanup;
    if (!freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_NSCodec, FALSE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE) ||
        !freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, TRUE) ||
        !freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32)) goto cleanup;
    peer->Capabilities = Capabilities;
    peer->PostConnect = PostConnect; peer->Activate = Activate;
    ctx->rdp.input->SynchronizeEvent = Synchronize;
    ctx->rdp.input->KeyboardEvent = Keyboard;
    ctx->rdp.input->UnicodeKeyboardEvent = UnicodeKeyboard;
    ctx->rdp.input->MouseEvent = Mouse;
    ctx->rdp.update->RefreshRect = Refresh;
    ctx->rdp.update->SuppressOutput = Suppress;
    if (!peer->Initialize(peer)) goto cleanup;
    for (;;)
    {
        events[0] = StopEvent;
        events[1] = session->Replaced;
        count = peer->GetEventHandles(peer, events + 2, 29);
        if (!count) break;
        events[count + 2] = WTSVirtualChannelManagerGetEventHandle(ctx->Vcm);
        status = WaitForMultipleObjects(count + 3, events, FALSE, 100);
        if (status == WAIT_OBJECT_0 || status == WAIT_FAILED) break;
        if (status == WAIT_OBJECT_0 + 1)
        {
            /* Tell the client why, as Windows does: a client that only sees the connection
             * drop reconnects automatically and takes the console back. */
            Log("Session replaced by a new connection");
            if (peer->activated)
            {
                freerdp_set_error_info(peer->context->rdp, ERRINFO_DISCONNECTED_BY_OTHER_CONNECTION);
                peer->Close(peer);
            }
            break;
        }
        if (!peer->CheckFileDescriptor(peer)) { Log("Session ended by the RDP transport"); break; }
        if (!WTSVirtualChannelManagerCheckFileDescriptor(ctx->Vcm)) { Log("Session ended by a virtual channel error"); break; }
        if (!ctx->Authorized && GetTickCount() - started > 30000) break;
        if (ctx->Active && peer->activated && GetTickCount() - lastFrame >= 100)
        {
            if (!Capture(ctx)) { Log("Session ended: desktop update failed"); break; }
            lastFrame = GetTickCount();
        }
    }
    peer->Disconnect(peer);
cleanup:
    freerdp_peer_context_free(peer);
done:
    freerdp_peer_free(peer);
    EnterCriticalSection(&SessionLock);
    if (CurrentSession == session) CurrentSession = NULL;
    LeaveCriticalSection(&SessionLock);
    /* The next session starts once this one has released its keys and capture */
    if (slot) ReleaseMutex(SessionSlot);
    CloseHandle(session->Replaced);
    free(session);
    InterlockedDecrement(&ClientThreads);
    return 0;
}

/* A loopback peer must be the Hyper-V socket bridge service: find the process that owns
 * the client end of the TCP connection and compare it with the service's process. */
static BOOL PeerIsBridge(const struct sockaddr_in *peer)
{
    MIB_TCPTABLE_OWNER_PID *table = NULL;
    SERVICE_STATUS_PROCESS status;
    SC_HANDLE manager, service;
    DWORD size = 0, owner = 0, i;
    BOOL bridge = FALSE;
    if (GetExtendedTcpTable(NULL, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) != ERROR_INSUFFICIENT_BUFFER)
        return FALSE;
    table = malloc(size);
    if (!table) return FALSE;
    if (GetExtendedTcpTable(table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_CONNECTIONS, 0) == NO_ERROR)
        for (i = 0; i < table->dwNumEntries; ++i)
            if (table->table[i].dwLocalAddr == peer->sin_addr.s_addr &&
                (USHORT)table->table[i].dwLocalPort == peer->sin_port &&
                (USHORT)table->table[i].dwRemotePort == htons(3389))
            { owner = table->table[i].dwOwningPid; break; }
    free(table);
    if (!owner) return FALSE;
    manager = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!manager) return FALSE;
    service = OpenServiceA(manager, "hvsockbridge", SERVICE_QUERY_STATUS);
    if (service)
    {
        if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status, sizeof(status), &size))
            bridge = status.dwProcessId == owner;
        CloseServiceHandle(service);
    }
    CloseServiceHandle(manager);
    return bridge;
}

static BOOL Accepted(freerdp_listener *listener, freerdp_peer *peer)
{
    CLIENT_SESSION *session;
    HANDLE thread;
    struct sockaddr_in address;
    int length = sizeof(address);
    (void)listener;
    if (getpeername((SOCKET)peer->sockfd, (struct sockaddr *)&address, &length) ||
        address.sin_family != AF_INET) return FALSE;
    /* The controller, or with loopback=1 the Hyper-V socket bridge on this machine */
    if (address.sin_addr.s_addr != inet_addr(AllowedPeer))
    {
        if (!Loopback || address.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return FALSE;
        if (!PeerIsBridge(&address))
        {
            Log("Loopback connection from a process other than hvsockbridge refused");
            return FALSE;
        }
    }
    if (InterlockedIncrement(&ClientThreads) > MAX_CLIENT_THREADS)
    {
        InterlockedDecrement(&ClientThreads);
        Log("Too many connections at once; connection refused");
        return FALSE;
    }
    session = calloc(1, sizeof(*session));
    if (session) session->Replaced = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!session || !session->Replaced)
    {
        free(session);
        InterlockedDecrement(&ClientThreads);
        return FALSE;
    }
    session->Peer = peer;
    session->Loopback = address.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
    /* The transport owns the socket once the client thread runs */
    thread = CreateThread(NULL, 0, ClientThread, session, 0, NULL);
    if (!thread)
    {
        CloseHandle(session->Replaced);
        free(session);
        InterlockedDecrement(&ClientThreads);
        return FALSE;
    }
    CloseHandle(thread);
    return TRUE;
}

static BOOL WINAPI Stop(DWORD event)
{
    (void)event;
    SetEvent(StopEvent);
    return TRUE;
}

int main(int argc, char **argv)
{
    char config[MAX_PATH], modules[MAX_PATH], *separator;
    char mac[MAX_INTERFACE_MACS * 18 + 16], mask[64], tail;
    unsigned octets[6], i;
    typedef int (__cdecl *SET_PROVIDER_PATH)(void *, const char *);
    SET_PROVIDER_PATH setProviderPath;
    freerdp_listener *listener;
    HANDLE events[32], singleton;
    BOOL LanListening = FALSE;
    DWORD count, status;
    WSADATA winsock;
    DebugPrint = (DEBUG_PRINT)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "DbgPrint");
    if (argc != 2 || !GetFullPathNameA(argv[1], sizeof(config), config, NULL))
    { Log("Usage: rdpserver.exe path-to-test-config.ini"); return 2; }
    if (!GetPrivateProfileIntA("server", "enabled", 0, config)) return 0;
    /* On the interactive desktop the console window can take the focus from
     * the application a client types into, and a Ctrl+C typed into it stops
     * the server: keep it hidden unless console=1. */
    if (!GetPrivateProfileIntA("server", "console", 0, config) && GetConsoleWindow())
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    GetPrivateProfileStringA("server", "bind", "", BindAddress, sizeof(BindAddress), config);
    GetPrivateProfileStringA("server", "allow", "", AllowedPeer, sizeof(AllowedPeer), config);
    GetPrivateProfileStringA("server", "username", "", Username, sizeof(Username), config);
    GetPrivateProfileStringA("server", "password", "", Password, sizeof(Password), config);
    GetPrivateProfileStringA("server", "certificate", "", Certificate, sizeof(Certificate), config);
    GetPrivateProfileStringA("server", "privatekey", "", PrivateKey, sizeof(PrivateKey), config);
    GetPrivateProfileStringA("server", "interface_mac", "", mac, sizeof(mac), config);
    GetPrivateProfileStringA("server", "netmask", "", mask, sizeof(mask), config);
    ClipboardEnabled = GetPrivateProfileIntA("server", "clipboard", 1, config) != 0;
    Loopback = GetPrivateProfileIntA("server", "loopback", 0, config) != 0;
    HvSocketTrust = Loopback && GetPrivateProfileIntA("server", "hvsock_trust", 0, config) != 0;
    {
        char root[MAX_PATH];
        GetPrivateProfileStringA("server", "transfer_root", "", root, sizeof(root), config);
        if (*root && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, root, -1,
                                         TransferRoot, MAX_PATH)) return 2;
    }
    if (mac[0])
    {
        char *entry, *next;
        if ((InterfaceMask = inet_addr(mask)) == INADDR_NONE || !InterfaceMask)
        { Log("Invalid test Ethernet MAC or netmask"); return 2; }
        for (entry = mac; entry; entry = next)
        {
            next = strchr(entry, ',');
            if (next) *next++ = 0;
            if (InterfaceMacCount == MAX_INTERFACE_MACS ||
                sscanf(entry, " %x:%x:%x:%x:%x:%x %c", &octets[0], &octets[1], &octets[2],
                       &octets[3], &octets[4], &octets[5], &tail) != 6)
            { Log("Invalid test Ethernet MAC or netmask"); return 2; }
            for (i = 0; i < 6; ++i)
            {
                if (octets[i] > 255) return 2;
                InterfaceMac[InterfaceMacCount][i] = (BYTE)octets[i];
            }
            ++InterfaceMacCount;
        }
        ConfigureAddress = TRUE;
    }
    if (!ConfigRelativePath(config, Certificate, sizeof(Certificate)) ||
        !ConfigRelativePath(config, PrivateKey, sizeof(PrivateKey)))
    { Log("Certificate or private key path is too long"); return 2; }
    if (!Username[0] || strlen(Password) < 16 || inet_addr(BindAddress) == INADDR_NONE ||
        inet_addr(BindAddress) == INADDR_ANY || inet_addr(AllowedPeer) == INADDR_NONE ||
        inet_addr(AllowedPeer) == INADDR_ANY || !Certificate[0] || !PrivateKey[0])
    { Log("Invalid configuration: explicit bind/peer IPv4, credentials and TLS files required"); return 2; }
    singleton = CreateMutexA(NULL, FALSE, "Local\\LiberNT-RDPServer-Console");
    if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS) { if (singleton) CloseHandle(singleton); return 1; }
    if (WSAStartup(MAKEWORD(2, 2), &winsock)) return 1;
    if (!GetModuleFileNameA(NULL, modules, sizeof(modules))) return 1;
    separator = strrchr(modules, '\\');
    if (!separator || (size_t)(separator - modules) + sizeof("\\ossl-modules") > sizeof(modules)) return 1;
    strcpy(separator, "\\ossl-modules");
    SetEnvironmentVariableA("OPENSSL_MODULES", modules);
    /* OpenSSL's CRT may cache its environment before main. Set its provider
     * path directly so ReactOS does not fall back to the build-machine path. */
    setProviderPath = (SET_PROVIDER_PATH)(void *)GetProcAddress(
        GetModuleHandleA(LIBCRYPTO_DLL), "OSSL_PROVIDER_set_default_search_path");
    if (!setProviderPath || !setProviderPath(NULL, modules))
    { Log("Cannot configure the bundled OpenSSL provider directory"); return 1; }
    if (!winpr_InitializeSSL(WINPR_SSL_INIT_DEFAULT)) return 1;
    StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!StopEvent) return 1;
    SessionSlot = CreateMutexA(NULL, FALSE, NULL);
    if (!SessionSlot) return 1;
    InitializeCriticalSection(&SessionLock);
    SetConsoleCtrlHandler(Stop, TRUE);
    listener = freerdp_listener_new();
    if (!listener) return 1;
    listener->PeerAccepted = Accepted;
    if (!WTSRegisterWtsApiFunctionTable(FreeRDP_InitWtsApi())) return 1;
    Log("Starting console server; waiting for %s:3389", BindAddress);
    /* hvsockbridge forwards the enhanced session of Virtual Machine Connection to
     * 127.0.0.1. It does not depend on the test LAN, so serve it right away. */
    if (Loopback)
    {
        if (listener->Open(listener, "127.0.0.1", 3389))
            Log("Listening on 127.0.0.1:3389 for the Hyper-V socket bridge");
        else
            Log("Cannot listen on 127.0.0.1:3389");
    }
    for (;;)
    {
        /* Autostart may run before PnP has finished configuring Ethernet, and the
         * configured interface may not exist at all: keep trying every 2 s. */
        if (!LanListening && EnsureAddress() && listener->Open(listener, BindAddress, 3389))
        {
            LanListening = TRUE;
            Log("Listening on %s:3389; TLS; allowed controller %s", BindAddress, AllowedPeer);
        }
        events[0] = StopEvent;
        count = listener->GetEventHandles(listener, events + 1, 31);
        if (!count && LanListening) break;
        status = WaitForMultipleObjects(count + 1, events, FALSE, LanListening ? INFINITE : 2000);
        if (status == WAIT_OBJECT_0 || status == WAIT_FAILED) break;
        if (status == WAIT_TIMEOUT) continue;
        if (!listener->CheckFileDescriptor(listener)) break;
    }
    listener->Close(listener);
    SetEvent(StopEvent);
    while (InterlockedCompareExchange(&ClientThreads, 0, 0)) Sleep(50);
    freerdp_listener_free(listener);
    SetConsoleCtrlHandler(Stop, FALSE);
    CloseHandle(StopEvent); CloseHandle(singleton);
    SecureZeroMemory(Password, sizeof(Password));
    if (AddressAdded) DeleteIPAddress(AddressContext);
    WSACleanup();
    return 0;
}
