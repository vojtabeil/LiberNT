/*
 * PROJECT:     ReactOS Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Win8+ process, timer and overlapped helpers
 * COPYRIGHT:   Adapted from corresponding Wine kernelbase implementations
 */

#include "k32_vista.h"

static BOOL
BaseValidateTimerWakeContext(
    _In_opt_ PREASON_CONTEXT WakeContext)
{
    UNICODE_STRING ReasonString;
    WCHAR ModulePath[MAX_PATH];
    ULONG Index;

    if (!WakeContext) return TRUE;
    if (WakeContext->Version != POWER_REQUEST_CONTEXT_VERSION)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (WakeContext->Flags == POWER_REQUEST_CONTEXT_SIMPLE_STRING)
    {
        if (!WakeContext->Reason.SimpleReasonString || !*WakeContext->Reason.SimpleReasonString)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        return TRUE;
    }
    if (WakeContext->Flags != POWER_REQUEST_CONTEXT_DETAILED_STRING)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!GetModuleFileNameW(WakeContext->Reason.Detailed.LocalizedReasonModule, ModulePath, ARRAYSIZE(ModulePath))) return FALSE;
    if (WakeContext->Reason.Detailed.ReasonStringCount > MAXULONG / sizeof(ReasonString) || (WakeContext->Reason.Detailed.ReasonStringCount && !WakeContext->Reason.Detailed.ReasonStrings))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    for (Index = 0; Index < WakeContext->Reason.Detailed.ReasonStringCount; ++Index) RtlInitUnicodeString(&ReasonString, WakeContext->Reason.Detailed.ReasonStrings[Index]);
    return TRUE;
}

BOOL
WINAPI
SetWaitableTimerEx(
    _In_ HANDLE hTimer,
    _In_ const LARGE_INTEGER *lpDueTime,
    _In_ LONG lPeriod,
    _In_opt_ PTIMERAPCROUTINE pfnCompletionRoutine,
    _In_opt_ LPVOID lpArgToCompletionRoutine,
    _In_opt_ PREASON_CONTEXT WakeContext,
    _In_ ULONG TolerableDelay)
{
    BOOL Result;

    /* Scheduling at the nominal due time is always within the caller's maximum tolerance. */
    UNREFERENCED_PARAMETER(TolerableDelay);
    if (!BaseValidateTimerWakeContext(WakeContext)) return FALSE;
    if (!lpDueTime || lPeriod < 0)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Result = SetWaitableTimer(hTimer, lpDueTime, lPeriod, pfnCompletionRoutine, lpArgToCompletionRoutine, WakeContext != NULL);
    if (Result) SetLastError(WakeContext ? ERROR_NOT_SUPPORTED : ERROR_SUCCESS);
    return Result;
}

LONG
WINAPI
GetCurrentPackageFullName(
    _Inout_ UINT32 *packageFullNameLength,
    _Out_writes_opt_(*packageFullNameLength) PWSTR packageFullName)
{
    UNREFERENCED_PARAMETER(packageFullName);

    if (!packageFullNameLength)
        return ERROR_INVALID_PARAMETER;

    return APPMODEL_ERROR_NO_PACKAGE;
}

typedef struct _K32_COPYFILE2_EXTENDED_PARAMETERS
{
    DWORD dwSize;
    DWORD dwCopyFlags;
    BOOL *pfCancel;
    PVOID pProgressRoutine;
    PVOID pvCallbackContext;
} K32_COPYFILE2_EXTENDED_PARAMETERS;

typedef struct _K32_CREATEFILE2_EXTENDED_PARAMETERS
{
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} K32_CREATEFILE2_EXTENDED_PARAMETERS;

/* COPYFILE2_* types from winbase.h are only visible for _WIN32_WINNT >= 0x601,
 * this module is built for 0x600, so mirror them here (same layout). */
typedef enum _K32_COPYFILE2_MESSAGE_TYPE
{
    K32_COPYFILE2_CALLBACK_NONE = 0,
    K32_COPYFILE2_CALLBACK_CHUNK_STARTED,
    K32_COPYFILE2_CALLBACK_CHUNK_FINISHED,
    K32_COPYFILE2_CALLBACK_STREAM_STARTED,
    K32_COPYFILE2_CALLBACK_STREAM_FINISHED,
    K32_COPYFILE2_CALLBACK_POLL_CONTINUE,
    K32_COPYFILE2_CALLBACK_ERROR
} K32_COPYFILE2_MESSAGE_TYPE;

typedef enum _K32_COPYFILE2_MESSAGE_ACTION
{
    K32_COPYFILE2_PROGRESS_CONTINUE = 0,
    K32_COPYFILE2_PROGRESS_CANCEL,
    K32_COPYFILE2_PROGRESS_STOP,
    K32_COPYFILE2_PROGRESS_QUIET,
    K32_COPYFILE2_PROGRESS_PAUSE
} K32_COPYFILE2_MESSAGE_ACTION;

typedef enum _K32_COPYFILE2_COPY_PHASE
{
    K32_COPYFILE2_PHASE_NONE = 0,
    K32_COPYFILE2_PHASE_PREPARE_SOURCE,
    K32_COPYFILE2_PHASE_PREPARE_DEST,
    K32_COPYFILE2_PHASE_READ_SOURCE,
    K32_COPYFILE2_PHASE_WRITE_DESTINATION
} K32_COPYFILE2_COPY_PHASE;

typedef struct _K32_COPYFILE2_MESSAGE
{
    K32_COPYFILE2_MESSAGE_TYPE Type;
    DWORD dwPadding;
    union
    {
        struct
        {
            DWORD dwStreamNumber;
            DWORD dwReserved;
            HANDLE hSourceFile;
            HANDLE hDestinationFile;
            ULARGE_INTEGER uliChunkNumber;
            ULARGE_INTEGER uliChunkSize;
            ULARGE_INTEGER uliStreamSize;
            ULARGE_INTEGER uliTotalFileSize;
        } ChunkStarted;
        struct
        {
            DWORD dwStreamNumber;
            DWORD dwFlags;
            HANDLE hSourceFile;
            HANDLE hDestinationFile;
            ULARGE_INTEGER uliChunkNumber;
            ULARGE_INTEGER uliChunkSize;
            ULARGE_INTEGER uliStreamSize;
            ULARGE_INTEGER uliStreamBytesTransferred;
            ULARGE_INTEGER uliTotalFileSize;
            ULARGE_INTEGER uliTotalBytesTransferred;
        } ChunkFinished;
        struct
        {
            DWORD dwStreamNumber;
            DWORD dwReserved;
            HANDLE hSourceFile;
            HANDLE hDestinationFile;
            ULARGE_INTEGER uliStreamSize;
            ULARGE_INTEGER uliTotalFileSize;
        } StreamStarted;
        struct
        {
            DWORD dwStreamNumber;
            DWORD dwReserved;
            HANDLE hSourceFile;
            HANDLE hDestinationFile;
            ULARGE_INTEGER uliStreamSize;
            ULARGE_INTEGER uliStreamBytesTransferred;
            ULARGE_INTEGER uliTotalFileSize;
            ULARGE_INTEGER uliTotalBytesTransferred;
        } StreamFinished;
        struct
        {
            DWORD dwReserved;
        } PollContinue;
        struct
        {
            K32_COPYFILE2_COPY_PHASE CopyPhase;
            DWORD dwStreamNumber;
            HRESULT hrFailure;
            DWORD dwReserved;
            ULARGE_INTEGER uliChunkNumber;
            ULARGE_INTEGER uliStreamSize;
            ULARGE_INTEGER uliStreamBytesTransferred;
            ULARGE_INTEGER uliTotalFileSize;
            ULARGE_INTEGER uliTotalBytesTransferred;
        } Error;
    } Info;
} K32_COPYFILE2_MESSAGE;

C_ASSERT(FIELD_OFFSET(K32_COPYFILE2_MESSAGE, Info) == 8);
C_ASSERT(FIELD_OFFSET(K32_COPYFILE2_MESSAGE, Info.ChunkFinished.uliChunkNumber) == 8 + 2 * sizeof(DWORD) + 2 * sizeof(HANDLE));
C_ASSERT(FIELD_OFFSET(K32_COPYFILE2_MESSAGE, Info.Error.uliChunkNumber) == 8 + 4 * sizeof(DWORD));

typedef K32_COPYFILE2_MESSAGE_ACTION
(CALLBACK *K32_PCOPYFILE2_PROGRESS_ROUTINE)(
    _In_ const K32_COPYFILE2_MESSAGE *pMessage,
    _In_opt_ PVOID pvCallbackContext);

#ifndef COPY_FILE_DIRECTORY
#define COPY_FILE_DIRECTORY                     0x00000080
#endif
#ifndef COPY_FILE_REQUEST_SECURITY_PRIVILEGES
#define COPY_FILE_REQUEST_SECURITY_PRIVILEGES   0x00002000
#endif
#ifndef COPY_FILE_RESUME_FROM_PAUSE
#define COPY_FILE_RESUME_FROM_PAUSE             0x00004000
#endif
#ifndef COPY_FILE_SKIP_ALTERNATE_STREAMS
#define COPY_FILE_SKIP_ALTERNATE_STREAMS        0x00008000
#endif
#ifndef COPY_FILE_NO_OFFLOAD
#define COPY_FILE_NO_OFFLOAD                    0x00040000
#endif
#ifndef COPY_FILE_OPEN_AND_COPY_REPARSE_POINT
#define COPY_FILE_OPEN_AND_COPY_REPARSE_POINT   0x00200000
#endif
#ifndef COPY_FILE_IGNORE_EDP_BLOCK
#define COPY_FILE_IGNORE_EDP_BLOCK              0x00400000
#endif
#ifndef COPY_FILE_IGNORE_SOURCE_ENCRYPTION
#define COPY_FILE_IGNORE_SOURCE_ENCRYPTION      0x00800000
#endif
#ifndef COPY_FILE_DONT_REQUEST_DEST_WRITE_DAC
#define COPY_FILE_DONT_REQUEST_DEST_WRITE_DAC   0x02000000
#endif
#ifndef COPY_FILE_DISABLE_PRE_ALLOCATION
#define COPY_FILE_DISABLE_PRE_ALLOCATION        0x04000000
#endif
#ifndef COPY_FILE_ENABLE_LOW_FREE_SPACE_MODE
#define COPY_FILE_ENABLE_LOW_FREE_SPACE_MODE    0x08000000
#endif
#ifndef COPY_FILE_REQUEST_COMPRESSED_TRAFFIC
#define COPY_FILE_REQUEST_COMPRESSED_TRAFFIC    0x10000000
#endif
#ifndef COPY_FILE_ENABLE_SPARSE_COPY
#define COPY_FILE_ENABLE_SPARSE_COPY            0x20000000
#endif

/* Every flag documented for CopyFile2. Anything else is rejected with E_INVALIDARG. */
#define K32_COPYFILE2_VALID_FLAGS \
    (COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE | COPY_FILE_OPEN_SOURCE_FOR_WRITE | \
     COPY_FILE_ALLOW_DECRYPTED_DESTINATION | COPY_FILE_DIRECTORY | COPY_FILE_COPY_SYMLINK | \
     COPY_FILE_NO_BUFFERING | COPY_FILE_REQUEST_SECURITY_PRIVILEGES | COPY_FILE_RESUME_FROM_PAUSE | \
     COPY_FILE_SKIP_ALTERNATE_STREAMS | COPY_FILE_NO_OFFLOAD | COPY_FILE_OPEN_AND_COPY_REPARSE_POINT | \
     COPY_FILE_IGNORE_EDP_BLOCK | COPY_FILE_IGNORE_SOURCE_ENCRYPTION | COPY_FILE_DONT_REQUEST_DEST_WRITE_DAC | \
     COPY_FILE_DISABLE_PRE_ALLOCATION | COPY_FILE_ENABLE_LOW_FREE_SPACE_MODE | \
     COPY_FILE_REQUEST_COMPRESSED_TRAFFIC | COPY_FILE_ENABLE_SPARSE_COPY)

/* Flags with a meaning for CopyFileExW. The rest are hints (offload, compression,
 * pre-allocation, ...) that only change how the data travels, not the result. */
#define K32_COPYFILE2_COPYFILEEX_FLAGS \
    (COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE | COPY_FILE_OPEN_SOURCE_FOR_WRITE | \
     COPY_FILE_ALLOW_DECRYPTED_DESTINATION | COPY_FILE_COPY_SYMLINK | COPY_FILE_NO_BUFFERING)

/* Size of the chunks the CopyFileExW copy loop (client/file/copy.c) transfers. */
#define K32_COPYFILE2_CHUNK_SIZE 0x40000

/* Translates the CopyFileExW progress callbacks into COPYFILE2_MESSAGEs. */
typedef struct _K32_COPYFILE2_CONTEXT
{
    K32_PCOPYFILE2_PROGRESS_ROUTINE Routine;
    PVOID RoutineContext;
    BOOL Quiet;
    BOOL Paused;
    BOOL StreamStarted;
    BOOL StreamFinished;
    DWORD StreamNumber;
    ULONGLONG ChunkNumber;
    ULONGLONG StreamSize;
    ULONGLONG StreamBytes;
    ULONGLONG TotalSize;
    ULONGLONG TotalBytes;
} K32_COPYFILE2_CONTEXT, *PK32_COPYFILE2_CONTEXT;

/* Sends one message. Returns FALSE if the copy must end; *Result then holds the
 * PROGRESS_* value for CopyFileExW. */
static BOOL
CopyFile2Send(
    _Inout_ PK32_COPYFILE2_CONTEXT Context,
    _In_ const K32_COPYFILE2_MESSAGE *Message,
    _Out_ DWORD *Result)
{
    K32_COPYFILE2_MESSAGE_ACTION Action;

    *Result = PROGRESS_CONTINUE;
    if (Context->Quiet)
    {
        *Result = PROGRESS_QUIET;
        return TRUE;
    }

    Action = Context->Routine(Message, Context->RoutineContext);
    switch (Action)
    {
        case K32_COPYFILE2_PROGRESS_CANCEL:
            /* Stop and delete the partial destination. */
            *Result = PROGRESS_CANCEL;
            return FALSE;
        case K32_COPYFILE2_PROGRESS_STOP:
            /* Stop and keep the partial destination for a later restart. */
            *Result = PROGRESS_STOP;
            return FALSE;
        case K32_COPYFILE2_PROGRESS_PAUSE:
            /* CopyFileExW cannot resume in place: keep the partial destination like
             * STOP, a COPY_FILE_RESUME_FROM_PAUSE call copies the file again. */
            Context->Paused = TRUE;
            *Result = PROGRESS_STOP;
            return FALSE;
        case K32_COPYFILE2_PROGRESS_QUIET:
            Context->Quiet = TRUE;
            *Result = PROGRESS_QUIET;
            return TRUE;
        case K32_COPYFILE2_PROGRESS_CONTINUE:
        default:
            return TRUE;
    }
}

static VOID
CopyFile2FillChunkStarted(
    _In_ PK32_COPYFILE2_CONTEXT Context,
    _Out_ K32_COPYFILE2_MESSAGE *Message,
    _In_ HANDLE SourceFile,
    _In_ HANDLE DestinationFile)
{
    ULONGLONG Remaining = Context->StreamSize - Context->StreamBytes;

    RtlZeroMemory(Message, sizeof(*Message));
    Message->Type = K32_COPYFILE2_CALLBACK_CHUNK_STARTED;
    Message->Info.ChunkStarted.dwStreamNumber = Context->StreamNumber;
    Message->Info.ChunkStarted.hSourceFile = SourceFile;
    Message->Info.ChunkStarted.hDestinationFile = DestinationFile;
    Message->Info.ChunkStarted.uliChunkNumber.QuadPart = Context->ChunkNumber;
    Message->Info.ChunkStarted.uliChunkSize.QuadPart = min(Remaining, (ULONGLONG)K32_COPYFILE2_CHUNK_SIZE);
    Message->Info.ChunkStarted.uliStreamSize.QuadPart = Context->StreamSize;
    Message->Info.ChunkStarted.uliTotalFileSize.QuadPart = Context->TotalSize;
}

static VOID
CopyFile2FillStreamFinished(
    _In_ PK32_COPYFILE2_CONTEXT Context,
    _Out_ K32_COPYFILE2_MESSAGE *Message,
    _In_ HANDLE SourceFile,
    _In_ HANDLE DestinationFile)
{
    RtlZeroMemory(Message, sizeof(*Message));
    Message->Type = K32_COPYFILE2_CALLBACK_STREAM_FINISHED;
    Message->Info.StreamFinished.dwStreamNumber = Context->StreamNumber;
    Message->Info.StreamFinished.hSourceFile = SourceFile;
    Message->Info.StreamFinished.hDestinationFile = DestinationFile;
    Message->Info.StreamFinished.uliStreamSize.QuadPart = Context->StreamSize;
    Message->Info.StreamFinished.uliStreamBytesTransferred.QuadPart = Context->StreamBytes;
    Message->Info.StreamFinished.uliTotalFileSize.QuadPart = Context->TotalSize;
    Message->Info.StreamFinished.uliTotalBytesTransferred.QuadPart = Context->TotalBytes;
    Context->StreamFinished = TRUE;
}

/*
 * CopyFileExW calls this once with CALLBACK_STREAM_SWITCH before the first chunk
 * of a stream and with CALLBACK_CHUNK_FINISHED after every chunk. Windows'
 * CopyFile2 reports STREAM_STARTED, CHUNK_STARTED/CHUNK_FINISHED pairs and
 * STREAM_FINISHED; a chunk-finished callback is also the point right before the
 * next chunk, so it carries the next CHUNK_STARTED as well.
 */
static DWORD
CALLBACK
CopyFile2Progress(
    _In_ LARGE_INTEGER TotalFileSize,
    _In_ LARGE_INTEGER TotalBytesTransferred,
    _In_ LARGE_INTEGER StreamSize,
    _In_ LARGE_INTEGER StreamBytesTransferred,
    _In_ DWORD dwStreamNumber,
    _In_ DWORD dwCallbackReason,
    _In_ HANDLE hSourceFile,
    _In_ HANDLE hDestinationFile,
    _In_opt_ LPVOID lpData)
{
    PK32_COPYFILE2_CONTEXT Context = lpData;
    K32_COPYFILE2_MESSAGE Message;
    DWORD Result = PROGRESS_CONTINUE;
    DWORD LastError = GetLastError();
    ULONGLONG ChunkSize;

    UNREFERENCED_PARAMETER(dwStreamNumber);

    Context->TotalSize = TotalFileSize.QuadPart;
    Context->TotalBytes = TotalBytesTransferred.QuadPart;

    if (dwCallbackReason == CALLBACK_STREAM_SWITCH)
    {
        /* Stream numbers start at one. */
        Context->StreamNumber++;
        Context->StreamStarted = TRUE;
        Context->StreamFinished = FALSE;
        Context->ChunkNumber = 0;
        Context->StreamSize = StreamSize.QuadPart;
        Context->StreamBytes = StreamBytesTransferred.QuadPart;

        RtlZeroMemory(&Message, sizeof(Message));
        Message.Type = K32_COPYFILE2_CALLBACK_STREAM_STARTED;
        Message.Info.StreamStarted.dwStreamNumber = Context->StreamNumber;
        Message.Info.StreamStarted.hSourceFile = hSourceFile;
        Message.Info.StreamStarted.hDestinationFile = hDestinationFile;
        Message.Info.StreamStarted.uliStreamSize.QuadPart = Context->StreamSize;
        Message.Info.StreamStarted.uliTotalFileSize.QuadPart = Context->TotalSize;
        if (!CopyFile2Send(Context, &Message, &Result))
            goto Done;

        if (Context->StreamBytes >= Context->StreamSize)
            CopyFile2FillStreamFinished(Context, &Message, hSourceFile, hDestinationFile);
        else
            CopyFile2FillChunkStarted(Context, &Message, hSourceFile, hDestinationFile);
        CopyFile2Send(Context, &Message, &Result);
        goto Done;
    }

    if (dwCallbackReason != CALLBACK_CHUNK_FINISHED)
        goto Done;

    ChunkSize = StreamBytesTransferred.QuadPart - Context->StreamBytes;
    Context->StreamSize = max(Context->StreamSize, (ULONGLONG)StreamSize.QuadPart);
    Context->StreamBytes = StreamBytesTransferred.QuadPart;
    if (ChunkSize == 0)
        goto Done;

    RtlZeroMemory(&Message, sizeof(Message));
    Message.Type = K32_COPYFILE2_CALLBACK_CHUNK_FINISHED;
    Message.Info.ChunkFinished.dwStreamNumber = Context->StreamNumber;
    Message.Info.ChunkFinished.hSourceFile = hSourceFile;
    Message.Info.ChunkFinished.hDestinationFile = hDestinationFile;
    Message.Info.ChunkFinished.uliChunkNumber.QuadPart = Context->ChunkNumber;
    Message.Info.ChunkFinished.uliChunkSize.QuadPart = ChunkSize;
    Message.Info.ChunkFinished.uliStreamSize.QuadPart = Context->StreamSize;
    Message.Info.ChunkFinished.uliStreamBytesTransferred.QuadPart = Context->StreamBytes;
    Message.Info.ChunkFinished.uliTotalFileSize.QuadPart = Context->TotalSize;
    Message.Info.ChunkFinished.uliTotalBytesTransferred.QuadPart = Context->TotalBytes;
    Context->ChunkNumber++;
    if (!CopyFile2Send(Context, &Message, &Result))
        goto Done;

    if (Context->StreamFinished)
    {
        /* The source grew after STREAM_FINISHED; just report the extra chunks. */
        goto Done;
    }
    if (Context->StreamBytes >= Context->StreamSize)
        CopyFile2FillStreamFinished(Context, &Message, hSourceFile, hDestinationFile);
    else
        CopyFile2FillChunkStarted(Context, &Message, hSourceFile, hDestinationFile);
    CopyFile2Send(Context, &Message, &Result);

Done:
    SetLastError(LastError);
    return Result;
}

static HRESULT
CopyFile2Directory(
    _In_ PCWSTR pwszExistingFileName,
    _In_ PCWSTR pwszNewFileName,
    _In_ DWORD Flags)
{
    DWORD Attributes = GetFileAttributesW(pwszExistingFileName);

    if (Attributes == INVALID_FILE_ATTRIBUTES)
        return HRESULT_FROM_WIN32(GetLastError());
    if (!(Attributes & FILE_ATTRIBUTE_DIRECTORY))
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);

    /* Creates the directory with the source's attributes (and streams/EAs). */
    if (CreateDirectoryExW(pwszExistingFileName, pwszNewFileName, NULL))
        return S_OK;

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (Flags & COPY_FILE_FAIL_IF_EXISTS)
            return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
        Attributes = GetFileAttributesW(pwszNewFileName);
        if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_DIRECTORY))
            return S_OK;
        return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    }

    return HRESULT_FROM_WIN32(GetLastError());
}

/*
 * CopyFile2 on top of CopyFileExW. Semantics follow the Windows documentation
 * (learn.microsoft.com, CopyFile2 / COPYFILE2_MESSAGE / COPYFILE2_MESSAGE_ACTION):
 * - CANCEL and pfCancel end with HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED), the
 *   destination is deleted; STOP ends with the same result but keeps it;
 * - PAUSE ends with HRESULT_FROM_WIN32(ERROR_REQUEST_PAUSED);
 * - QUIET continues the copy without further messages;
 * - a failure is reported to the routine with a COPYFILE2_CALLBACK_ERROR message.
 */
HRESULT
WINAPI
CopyFile2(
    _In_ PCWSTR pwszExistingFileName,
    _In_ PCWSTR pwszNewFileName,
    _In_opt_ K32_COPYFILE2_EXTENDED_PARAMETERS *pExtendedParameters)
{
    K32_COPYFILE2_CONTEXT Context;
    K32_COPYFILE2_MESSAGE Message;
    BOOL *Cancel = NULL;
    DWORD Flags = 0;
    DWORD Error;
    HRESULT hr;

    RtlZeroMemory(&Context, sizeof(Context));

    if (!pwszExistingFileName || !pwszNewFileName)
        return E_INVALIDARG;

    if (pExtendedParameters)
    {
        if (pExtendedParameters->dwSize != sizeof(*pExtendedParameters))
            return E_INVALIDARG;
        if (pExtendedParameters->dwCopyFlags & ~K32_COPYFILE2_VALID_FLAGS)
            return E_INVALIDARG;

        Flags = pExtendedParameters->dwCopyFlags;
        Cancel = pExtendedParameters->pfCancel;
        Context.Routine = (K32_PCOPYFILE2_PROGRESS_ROUTINE)pExtendedParameters->pProgressRoutine;
        Context.RoutineContext = pExtendedParameters->pvCallbackContext;
    }

    if (Cancel && *Cancel)
        return HRESULT_FROM_WIN32(ERROR_REQUEST_ABORTED);

    if (Flags & COPY_FILE_DIRECTORY)
        return CopyFile2Directory(pwszExistingFileName, pwszNewFileName, Flags);

    if (CopyFileExW(pwszExistingFileName,
                    pwszNewFileName,
                    Context.Routine ? CopyFile2Progress : NULL,
                    Context.Routine ? &Context : NULL,
                    Cancel,
                    Flags & K32_COPYFILE2_COPYFILEEX_FLAGS))
    {
        return S_OK;
    }

    Error = GetLastError();
    if (Error == ERROR_REQUEST_ABORTED && Context.Paused)
        Error = ERROR_REQUEST_PAUSED;
    if (Error == ERROR_SUCCESS)
        Error = ERROR_GEN_FAILURE;
    hr = HRESULT_FROM_WIN32(Error);

    if (Context.Routine && !Context.Quiet &&
        Error != ERROR_REQUEST_ABORTED && Error != ERROR_REQUEST_PAUSED)
    {
        RtlZeroMemory(&Message, sizeof(Message));
        Message.Type = K32_COPYFILE2_CALLBACK_ERROR;
        if (!Context.StreamStarted)
        {
            /* Nothing was transferred: the source or the destination could not be opened. */
            Message.Info.Error.CopyPhase =
                (GetFileAttributesW(pwszExistingFileName) == INVALID_FILE_ATTRIBUTES) ?
                K32_COPYFILE2_PHASE_PREPARE_SOURCE : K32_COPYFILE2_PHASE_PREPARE_DEST;
        }
        else
        {
            Message.Info.Error.CopyPhase =
                (Error == ERROR_DISK_FULL || Error == ERROR_HANDLE_DISK_FULL) ?
                K32_COPYFILE2_PHASE_WRITE_DESTINATION : K32_COPYFILE2_PHASE_READ_SOURCE;
        }
        Message.Info.Error.dwStreamNumber = Context.StreamNumber;
        Message.Info.Error.hrFailure = hr;
        Message.Info.Error.uliChunkNumber.QuadPart = Context.ChunkNumber;
        Message.Info.Error.uliStreamSize.QuadPart = Context.StreamSize;
        Message.Info.Error.uliStreamBytesTransferred.QuadPart = Context.StreamBytes;
        Message.Info.Error.uliTotalFileSize.QuadPart = Context.TotalSize;
        Message.Info.Error.uliTotalBytesTransferred.QuadPart = Context.TotalBytes;
        /* The copy has already failed: the returned action has nothing left to change. */
        Context.Routine(&Message, Context.RoutineContext);
    }

    SetLastError(Error);
    return hr;
}

HANDLE
WINAPI
CreateFile2(
    _In_ PCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_ DWORD dwCreationDisposition,
    _In_opt_ K32_CREATEFILE2_EXTENDED_PARAMETERS *pCreateExParams)
{
    DWORD Attributes = FILE_ATTRIBUTE_NORMAL;
    LPSECURITY_ATTRIBUTES Security = NULL;
    HANDLE Template = NULL;

    if (pCreateExParams)
    {
        if (pCreateExParams->dwSize != sizeof(*pCreateExParams))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }

        Attributes = (pCreateExParams->dwFileAttributes &
                      (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                       FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_TEMPORARY |
                       FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_INTEGRITY_STREAM)) |
                     (pCreateExParams->dwFileFlags &
                      (FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_NO_BUFFERING |
                       FILE_FLAG_OPEN_NO_RECALL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED |
                       FILE_FLAG_POSIX_SEMANTICS | FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_SEQUENTIAL_SCAN |
                       FILE_FLAG_WRITE_THROUGH)) |
                     pCreateExParams->dwSecurityQosFlags;
        Security = pCreateExParams->lpSecurityAttributes;
        Template = pCreateExParams->hTemplateFile;
    }

    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, Security, dwCreationDisposition, Attributes, Template);
}
