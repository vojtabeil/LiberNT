/*
 * PROJECT:     LiberNT RDP console server
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Clipboard redirection over the RDP cliprdr channel
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif193@gmail.com>
 */

/*
 * A hidden window on the interactive desktop listens for clipboard changes.
 * A local change is announced to the client as a format list; the client's
 * data requests are served with GetClipboardData. A client format list makes
 * the window the clipboard owner with delayed rendering, and WM_RENDERFORMAT
 * fetches the data from the client. Only memory (HGLOBAL) formats travel;
 * GDI handle formats and file streams are not redirected.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <freerdp/server/cliprdr.h>
#include "clipboard.h"

#define MAX_FORMATS 64
#define DATA_LIMIT (32 * 1024 * 1024)
#define RENDER_TIMEOUT 10000
#define WM_CLIP_CLIENT_LIST (WM_APP + 1)
#define WM_CLIP_DATA_REQUEST (WM_APP + 2)
#define WM_CLIP_STOP (WM_APP + 3)

typedef BOOL (WINAPI *ADD_LISTENER)(HWND);
typedef BOOL (WINAPI *REMOVE_LISTENER)(HWND);

typedef struct
{
    UINT32 Remote;
    UINT Local;
} FORMAT_MAP;

struct RDP_CLIPBOARD
{
    CliprdrServerContext *Channel;
    CLIPBOARD_LOG Log;
    HANDLE Thread, Created, DataReady;
    HWND Window, NextViewer;
    BOOL Listener;
    volatile LONG Stopping, Started;
    CRITICAL_SECTION Lock;
    FORMAT_MAP Formats[MAX_FORMATS];
    UINT FormatCount;
    BYTE *Data;
    UINT32 DataLength;
    BOOL DataOk;
    ULONGLONG ToClient, FromClient;
};

static const UINT StandardFormats[] =
{
    CF_UNICODETEXT, CF_TEXT, CF_OEMTEXT, CF_LOCALE, CF_DIB, CF_DIBV5
};

/* Registered formats that carry file streams or handles are left out. */
static BOOL Redirectable(UINT format, const char *name)
{
    unsigned i;
    if (format >= 0xC000 && format <= 0xFFFF)
        return name && *name && strncmp(name, "FileGroupDescriptor", 19) &&
               strcmp(name, "FileContents") && strcmp(name, "FileName") &&
               strcmp(name, "FileNameW") && strcmp(name, "Shell IDList Array");
    for (i = 0; i < ARRAYSIZE(StandardFormats); ++i)
        if (format == StandardFormats[i]) return TRUE;
    return FALSE;
}

static UINT SendDataResponse(RDP_CLIPBOARD *c, const BYTE *data, UINT32 length)
{
    CLIPRDR_FORMAT_DATA_RESPONSE response = {0};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    response.common.dataLen = data ? length : 0;
    response.requestedFormatData = data;
    return c->Channel->ServerFormatDataResponse(c->Channel, &response);
}

static BOOL OpenClipboardRetry(HWND window)
{
    unsigned i;
    for (i = 0; i < 20; ++i)
    {
        if (OpenClipboard(window)) return TRUE;
        Sleep(25);
    }
    return FALSE;
}

/* Local clipboard changed: announce its redirectable formats to the client. */
static void AnnounceLocal(RDP_CLIPBOARD *c)
{
    CLIPRDR_FORMAT formats[MAX_FORMATS];
    char names[MAX_FORMATS][128];
    CLIPRDR_FORMAT_LIST list = {0};
    UINT format = 0, count = 0;
    UINT rc;
    if (!InterlockedCompareExchange(&c->Started, 0, 0) || GetClipboardOwner() == c->Window) return;
    if (!OpenClipboardRetry(c->Window)) { c->Log("Clipboard: cannot open the local clipboard"); return; }
    while ((format = EnumClipboardFormats(format)) && count < MAX_FORMATS)
    {
        names[count][0] = 0;
        if (format >= 0xC000 &&
            !GetClipboardFormatNameA(format, names[count], sizeof(names[count])))
            continue;
        if (!Redirectable(format, names[count])) continue;
        formats[count].formatId = format;
        formats[count].formatName = names[count][0] ? names[count] : NULL;
        ++count;
    }
    /* CF_DIB is synthesized from CF_BITMAP; offer it even when the
     * enumeration lists only the GDI handle format. */
    if (count < MAX_FORMATS && IsClipboardFormatAvailable(CF_DIB))
    {
        UINT i;
        for (i = 0; i < count && formats[i].formatId != CF_DIB; ++i) ;
        if (i == count) { formats[count].formatId = CF_DIB; formats[count].formatName = NULL; ++count; }
    }
    CloseClipboard();
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = count;
    list.formats = formats;
    rc = c->Channel->ServerFormatList(c->Channel, &list);
    c->Log("Clipboard: local change, %u formats announced to the client (rc=%u)", count, rc);
}

/* The client asked for one of the formats the VM announced. */
static void ServeRequest(RDP_CLIPBOARD *c, UINT format)
{
    HANDLE handle;
    BYTE *data;
    SIZE_T size;
    if (InterlockedCompareExchange(&c->Stopping, 0, 0)) return;
    if (!format || !OpenClipboardRetry(c->Window))
    {
        SendDataResponse(c, NULL, 0);
        return;
    }
    handle = GetClipboardData(format);
    data = handle ? GlobalLock(handle) : NULL;
    size = data ? GlobalSize(handle) : 0;
    if (data && size && size <= DATA_LIMIT)
    {
        /* Text is sent up to and including its terminator. */
        if (format == CF_UNICODETEXT)
        {
            SIZE_T chars = 0;
            while (chars < size / 2 && ((WCHAR *)data)[chars]) ++chars;
            if (chars < size / 2) size = (chars + 1) * 2;
        }
        else if (format == CF_TEXT || format == CF_OEMTEXT)
        {
            SIZE_T chars = 0;
            while (chars < size && data[chars]) ++chars;
            if (chars < size) size = chars + 1;
        }
        SendDataResponse(c, data, (UINT32)size);
        ++c->ToClient;
        c->Log("Clipboard: sent format %u to the client, %lu bytes", format, (ULONG)size);
    }
    else
    {
        SendDataResponse(c, NULL, 0);
        c->Log("Clipboard: format %u not available", format);
    }
    if (data) GlobalUnlock(handle);
    CloseClipboard();
}

/* Take ownership of the local clipboard with the client's formats, rendered on demand. */
static void TakeClientFormats(RDP_CLIPBOARD *c)
{
    UINT i, count;
    UINT local[MAX_FORMATS];
    EnterCriticalSection(&c->Lock);
    count = c->FormatCount;
    for (i = 0; i < count; ++i) local[i] = c->Formats[i].Local;
    LeaveCriticalSection(&c->Lock);
    if (!OpenClipboardRetry(c->Window)) { c->Log("Clipboard: cannot open the local clipboard"); return; }
    EmptyClipboard();
    for (i = 0; i < count; ++i) SetClipboardData(local[i], NULL);
    CloseClipboard();
    c->Log("Clipboard: client copied, %u formats offered locally", count);
}

static UINT32 RemoteFormat(RDP_CLIPBOARD *c, UINT local)
{
    UINT32 remote = 0;
    UINT i;
    EnterCriticalSection(&c->Lock);
    for (i = 0; i < c->FormatCount; ++i)
        if (c->Formats[i].Local == local) { remote = c->Formats[i].Remote; break; }
    LeaveCriticalSection(&c->Lock);
    return remote;
}

/* Delayed rendering: fetch the data from the client and hand it to the clipboard. */
static BOOL RenderFormat(RDP_CLIPBOARD *c, UINT local)
{
    CLIPRDR_FORMAT_DATA_REQUEST request = {0};
    UINT32 remote = RemoteFormat(c, local);
    HGLOBAL memory;
    BYTE *target;
    BOOL ok = FALSE;
    if (!remote || InterlockedCompareExchange(&c->Stopping, 0, 0)) return FALSE;
    ResetEvent(c->DataReady);
    request.common.msgType = CB_FORMAT_DATA_REQUEST;
    request.requestedFormatId = remote;
    if (c->Channel->ServerFormatDataRequest(c->Channel, &request) != CHANNEL_RC_OK) return FALSE;
    if (WaitForSingleObject(c->DataReady, RENDER_TIMEOUT) != WAIT_OBJECT_0)
    {
        c->Log("Clipboard: client did not deliver format %lu", (ULONG)remote);
        return FALSE;
    }
    EnterCriticalSection(&c->Lock);
    if (c->DataOk && c->Data && c->DataLength)
    {
        memory = GlobalAlloc(GMEM_MOVEABLE, c->DataLength);
        target = memory ? GlobalLock(memory) : NULL;
        if (target)
        {
            memcpy(target, c->Data, c->DataLength);
            GlobalUnlock(memory);
            ok = SetClipboardData(local, memory) != NULL;
            if (!ok) GlobalFree(memory);
        }
        else if (memory) GlobalFree(memory);
        if (ok)
        {
            ++c->FromClient;
            c->Log("Clipboard: received format %lu from the client, %lu bytes",
                   (ULONG)remote, (ULONG)c->DataLength);
        }
    }
    free(c->Data);
    c->Data = NULL;
    c->DataLength = 0;
    LeaveCriticalSection(&c->Lock);
    return ok;
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    RDP_CLIPBOARD *c = (RDP_CLIPBOARD *)GetWindowLongPtrA(window, GWLP_USERDATA);
    switch (message)
    {
    case WM_CREATE:
        c = ((CREATESTRUCTA *)lParam)->lpCreateParams;
        SetWindowLongPtrA(window, GWLP_USERDATA, (LONG_PTR)c);
        return 0;
    case WM_CLIPBOARDUPDATE:
        if (c && !InterlockedCompareExchange(&c->Stopping, 0, 0)) AnnounceLocal(c);
        return 0;
    case WM_DRAWCLIPBOARD:
        /* Clipboard viewer chain, used when the format listener API is missing. */
        if (c && !c->Listener)
        {
            if (!InterlockedCompareExchange(&c->Stopping, 0, 0)) AnnounceLocal(c);
            if (c->NextViewer) SendMessageA(c->NextViewer, message, wParam, lParam);
        }
        return 0;
    case WM_CHANGECBCHAIN:
        if (c && (HWND)wParam == c->NextViewer) c->NextViewer = (HWND)lParam;
        else if (c && c->NextViewer) SendMessageA(c->NextViewer, message, wParam, lParam);
        return 0;
    case WM_CLIP_CLIENT_LIST:
        TakeClientFormats(c);
        return 0;
    case WM_CLIP_DATA_REQUEST:
        ServeRequest(c, (UINT)wParam);
        return 0;
    case WM_RENDERFORMAT:
        RenderFormat(c, (UINT)wParam);
        return 0;
    case WM_RENDERALLFORMATS:
        /* The window goes away with the session; the client's data cannot
         * be fetched any more, so give up ownership instead. */
        if (OpenClipboard(window))
        {
            if (GetClipboardOwner() == window) EmptyClipboard();
            CloseClipboard();
        }
        return 0;
    case WM_CLIP_STOP:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (c && c->Listener)
        {
            REMOVE_LISTENER remove = (REMOVE_LISTENER)(void *)GetProcAddress(
                GetModuleHandleA("user32.dll"), "RemoveClipboardFormatListener");
            if (remove) remove(window);
        }
        else if (c)
            ChangeClipboardChain(window, c->NextViewer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

static DWORD WINAPI WindowThread(void *arg)
{
    RDP_CLIPBOARD *c = arg;
    WNDCLASSA wc = {0};
    MSG msg;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "LiberNTRDPClipboard";
    RegisterClassA(&wc);
    c->Window = CreateWindowExA(0, wc.lpszClassName, "LiberNT RDP clipboard", WS_POPUP,
                                0, 0, 0, 0, NULL, NULL, wc.hInstance, c);
    if (c->Window)
    {
        ADD_LISTENER add = (ADD_LISTENER)(void *)GetProcAddress(
            GetModuleHandleA("user32.dll"), "AddClipboardFormatListener");
        c->Listener = add && add(c->Window);
        if (!c->Listener)
        {
            /* Older user32: join the clipboard viewer chain instead. */
            SetLastError(ERROR_SUCCESS);
            c->NextViewer = SetClipboardViewer(c->Window);
            if (!c->NextViewer && GetLastError() != ERROR_SUCCESS)
            {
                c->Log("Clipboard: cannot watch the clipboard, error=%lu", GetLastError());
                DestroyWindow(c->Window);
                c->Window = NULL;
            }
        }
        if (c->Window)
            c->Log("Clipboard: watching with %s", c->Listener ? "a format listener" : "the viewer chain");
    }
    SetEvent(c->Created);
    if (!c->Window) return 1;
    while (GetMessageA(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

static UINT ClientCapabilities(CliprdrServerContext *channel, const CLIPRDR_CAPABILITIES *caps)
{
    RDP_CLIPBOARD *c = channel->custom;
    c->Log("Clipboard: client capabilities, %lu sets, long names=%d",
           (ULONG)caps->cCapabilitiesSets, channel->useLongFormatNames);
    return CHANNEL_RC_OK;
}

static UINT ClientFormatList(CliprdrServerContext *channel, const CLIPRDR_FORMAT_LIST *list)
{
    RDP_CLIPBOARD *c = channel->custom;
    CLIPRDR_FORMAT_LIST_RESPONSE response = {0};
    UINT32 i;
    UINT count = 0;
    EnterCriticalSection(&c->Lock);
    for (i = 0; i < list->numFormats && count < MAX_FORMATS; ++i)
    {
        const CLIPRDR_FORMAT *f = &list->formats[i];
        UINT local = f->formatId;
        if (f->formatId >= 0xC000)
            local = (f->formatName && *f->formatName) ? RegisterClipboardFormatA(f->formatName) : 0;
        if (!local || !Redirectable(local, f->formatName)) continue;
        c->Formats[count].Remote = f->formatId;
        c->Formats[count].Local = local;
        ++count;
    }
    c->FormatCount = count;
    LeaveCriticalSection(&c->Lock);
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    if (channel->ServerFormatListResponse(channel, &response) != CHANNEL_RC_OK)
        return ERROR_INTERNAL_ERROR;
    /* An empty list only says the client clipboard holds nothing we can use. */
    if (count) PostMessageA(c->Window, WM_CLIP_CLIENT_LIST, 0, 0);
    return CHANNEL_RC_OK;
}

static UINT ClientFormatListResponse(CliprdrServerContext *channel, const CLIPRDR_FORMAT_LIST_RESPONSE *response)
{
    (void)channel; (void)response;
    return CHANNEL_RC_OK;
}

static UINT ClientFormatDataRequest(CliprdrServerContext *channel, const CLIPRDR_FORMAT_DATA_REQUEST *request)
{
    RDP_CLIPBOARD *c = channel->custom;
    /* The client may only ask for formats the VM announced, which carry local IDs. */
    if (!PostMessageA(c->Window, WM_CLIP_DATA_REQUEST, request->requestedFormatId, 0))
        return SendDataResponse(c, NULL, 0);
    return CHANNEL_RC_OK;
}

static UINT ClientFormatDataResponse(CliprdrServerContext *channel, const CLIPRDR_FORMAT_DATA_RESPONSE *response)
{
    RDP_CLIPBOARD *c = channel->custom;
    EnterCriticalSection(&c->Lock);
    free(c->Data);
    c->Data = NULL;
    c->DataLength = 0;
    c->DataOk = response->common.msgFlags == CB_RESPONSE_OK &&
                response->common.dataLen <= DATA_LIMIT;
    if (c->DataOk && response->common.dataLen)
    {
        c->Data = malloc(response->common.dataLen);
        if (c->Data)
        {
            memcpy(c->Data, response->requestedFormatData, response->common.dataLen);
            c->DataLength = response->common.dataLen;
        }
    }
    LeaveCriticalSection(&c->Lock);
    SetEvent(c->DataReady);
    return CHANNEL_RC_OK;
}

RDP_CLIPBOARD *ClipboardStart(HANDLE vcm, rdpContext *rdp, CLIPBOARD_LOG log)
{
    RDP_CLIPBOARD *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    InitializeCriticalSection(&c->Lock);
    c->Log = log;
    c->Created = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->DataReady = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!c->Created || !c->DataReady) goto fail;
    c->Thread = CreateThread(NULL, 0, WindowThread, c, 0, NULL);
    if (!c->Thread) goto fail;
    WaitForSingleObject(c->Created, INFINITE);
    if (!c->Window) goto fail;
    c->Channel = cliprdr_server_context_new(vcm);
    if (!c->Channel) goto fail;
    c->Channel->custom = c;
    c->Channel->rdpcontext = rdp;
    c->Channel->useLongFormatNames = TRUE;
    c->Channel->streamFileClipEnabled = FALSE;
    c->Channel->fileClipNoFilePaths = FALSE;
    c->Channel->canLockClipData = FALSE;
    c->Channel->ClientCapabilities = ClientCapabilities;
    c->Channel->ClientFormatList = ClientFormatList;
    c->Channel->ClientFormatListResponse = ClientFormatListResponse;
    c->Channel->ClientFormatDataRequest = ClientFormatDataRequest;
    c->Channel->ClientFormatDataResponse = ClientFormatDataResponse;
    if (c->Channel->Start(c->Channel) != CHANNEL_RC_OK)
    {
        cliprdr_server_context_free(c->Channel);
        c->Channel = NULL;
        goto fail;
    }
    InterlockedExchange(&c->Started, 1);
    log("Clipboard redirection started");
    return c;
fail:
    log("Clipboard redirection failed to start");
    ClipboardStop(c);
    return NULL;
}

void ClipboardStop(RDP_CLIPBOARD *c)
{
    BOOL windowGone = TRUE;
    if (!c) return;
    InterlockedExchange(&c->Stopping, 1);
    if (c->DataReady) SetEvent(c->DataReady);
    if (c->Thread)
    {
        if (c->Window) PostMessageA(c->Window, WM_CLIP_STOP, 0, 0);
        windowGone = WaitForSingleObject(c->Thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(c->Thread);
    }
    if (c->Channel)
    {
        c->Log("Clipboard: session totals, %llu transfers to the client, %llu from the client",
               c->ToClient, c->FromClient);
        c->Channel->Stop(c->Channel);
        cliprdr_server_context_free(c->Channel);
        c->Channel = NULL;
    }
    if (!windowGone)
    {
        /* The window thread may still use the state; leak it rather than free it under the thread. */
        c->Log("Clipboard: window thread did not exit");
        return;
    }
    if (c->Created) CloseHandle(c->Created);
    if (c->DataReady) CloseHandle(c->DataReady);
    free(c->Data);
    DeleteCriticalSection(&c->Lock);
    free(c);
}
