/*
 * PROJECT:     LiberNT Hyper-V VMBus Driver
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     Hyper-V sockets: host-initiated connections and the \\.\HvSocket device
 * COPYRIGHT:   Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

/*
 * The host opens a Hyper-V socket connection by offering a channel whose
 * interface type is the service GUID and whose instance identifies the
 * connection (TLNPI provider offer). The guest accepts it by opening the
 * channel. Data travels as in-band packets with a pipe header; a packet
 * without data closes that direction. The host rescinds the offer when the
 * connection is gone or nobody opened it in time.
 *
 * Flow control comes from the rings: inbound packets stay in the ring until
 * a read wants them, and a write that does not fit asks the host, through
 * the pending send size, to signal when there is room.
 *
 * One spin lock protects the listeners, the connections and their request
 * queues. Channels are opened and closed at PASSIVE_LEVEL.
 */

#include "vmbus.h"
#include "hvsockioctl.h"

#define NDEBUG
#include <debug.h>

#define HVS_RING_SIZE       (64 * 1024)
#define HVS_RECV_SIZE       (64 * 1024)
#define HVS_PIPE_DATA       1
#define HVS_SEND_CHUNK      (PAGE_SIZE - sizeof(HVS_PIPE_HEADER))
#define HVS_MAX_BACKLOG     8

typedef struct _HVS_PIPE_HEADER
{
    ULONG PacketType;
    ULONG DataSize;
} HVS_PIPE_HEADER;

typedef enum _HVS_CONNECTION_STATE
{
    HvsOpening,
    HvsBacklog,
    HvsAccepted,
    HvsClosing
} HVS_CONNECTION_STATE;

typedef struct _HVS_LISTENER
{
    LIST_ENTRY ListEntry;
    GUID ServiceId;
    LIST_ENTRY Backlog;
    ULONG BacklogCount;
    LIST_ENTRY AcceptQueue;
} HVS_LISTENER, *PHVS_LISTENER;

typedef struct _HVS_CONNECTION
{
    LIST_ENTRY ListEntry;
    PVMBUS_CHANNEL Channel;
    PHVS_LISTENER Listener;
    HVS_CONNECTION_STATE State;
    PIO_WORKITEM WorkItem;
    LIST_ENTRY ReadQueue;
    LIST_ENTRY WriteQueue;
    BOOLEAN PeerClosed;
    BOOLEAN Reset;
    PUCHAR RecvBuffer;
    ULONG RecvOffset;
    ULONG RecvEnd;
    PUCHAR SendBuffer;
} HVS_CONNECTION, *PHVS_CONNECTION;

/* What the file object of a handle stands for */
typedef enum _HVS_FILE_TYPE
{
    HvsFileNone,
    HvsFileListener,
    HvsFileAccepting,
    HvsFileConnection
} HVS_FILE_TYPE;

static PDEVICE_OBJECT HvsDevice;
static KSPIN_LOCK HvsLock;
static LIST_ENTRY HvsListeners;

#define HvsFileType(File)       ((HVS_FILE_TYPE)(ULONG_PTR)(File)->FsContext2)
#define HvsSetFile(File, Type, Context) \
    ((File)->FsContext = (Context), (File)->FsContext2 = (PVOID)(ULONG_PTR)(Type))

static
PHVS_LISTENER
HvsFindListener(
    _In_ const GUID *ServiceId)
{
    PLIST_ENTRY Entry;

    for (Entry = HvsListeners.Flink; Entry != &HvsListeners; Entry = Entry->Flink)
    {
        PHVS_LISTENER Listener = CONTAINING_RECORD(Entry, HVS_LISTENER, ListEntry);
        if (IsEqualGUID(&Listener->ServiceId, ServiceId))
            return Listener;
    }

    return NULL;
}

/* Request queues: the cancel routine and the dequeue race through IoSetCancelRoutine */

static
VOID
NTAPI
HvsCancelIrp(
    _Inout_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    KIRQL Irql;

    UNREFERENCED_PARAMETER(DeviceObject);
    IoReleaseCancelSpinLock(Irp->CancelIrql);

    KeAcquireSpinLock(&HvsLock, &Irql);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    KeReleaseSpinLock(&HvsLock, Irql);

    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* Called with HvsLock held. Returns FALSE when the IRP was cancelled already. */
static
BOOLEAN
HvsQueueIrp(
    _Inout_ PLIST_ENTRY Queue,
    _Inout_ PIRP Irp)
{
    IoSetCancelRoutine(Irp, HvsCancelIrp);
    if (Irp->Cancel && IoSetCancelRoutine(Irp, NULL) != NULL)
        return FALSE;

    IoMarkIrpPending(Irp);
    InsertTailList(Queue, &Irp->Tail.Overlay.ListEntry);
    return TRUE;
}

/* Called with HvsLock held: the first IRP that is not being cancelled, still queued */
static
PIRP
HvsPeekIrp(
    _In_ PLIST_ENTRY Queue)
{
    PLIST_ENTRY Entry;

    for (Entry = Queue->Flink; Entry != Queue; Entry = Entry->Flink)
    {
        PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
        if (!Irp->Cancel)
            return Irp;
    }

    return NULL;
}

/* Called with HvsLock held: take the IRP off its queue unless its cancel routine runs */
static
BOOLEAN
HvsTakeIrp(
    _Inout_ PIRP Irp)
{
    if (IoSetCancelRoutine(Irp, NULL) == NULL)
        return FALSE;

    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    return TRUE;
}

static
VOID
HvsFinish(
    _Inout_ PLIST_ENTRY Done,
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    InsertTailList(Done, &Irp->Tail.Overlay.ListEntry);
}

static
VOID
HvsCompleteDone(
    _Inout_ PLIST_ENTRY Done)
{
    while (!IsListEmpty(Done))
    {
        PIRP Irp = CONTAINING_RECORD(RemoveHeadList(Done), IRP, Tail.Overlay.ListEntry);
        IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
    }
}

/* Called with HvsLock held: fail every queued IRP */
static
VOID
HvsFlushQueue(
    _Inout_ PLIST_ENTRY Queue,
    _Inout_ PLIST_ENTRY Done,
    _In_ NTSTATUS Status)
{
    PIRP Irp;

    while ((Irp = HvsPeekIrp(Queue)) != NULL)
    {
        if (!HvsTakeIrp(Irp))
            break;
        HvsFinish(Done, Irp, Status, 0);
    }
}

/* Data path, called with HvsLock held */

static
BOOLEAN
HvsReceive(
    _Inout_ PHVS_CONNECTION Connection)
{
    PVMBUS_PACKET_DESCRIPTOR Descriptor;
    HVS_PIPE_HEADER Header;
    ULONG Length;
    ULONG Offset;
    NTSTATUS Status;

    for (;;)
    {
        Status = VmbChannelReceivePacket(Connection->Channel, Connection->RecvBuffer, HVS_RECV_SIZE, &Length);
        if (Status == STATUS_NO_MORE_ENTRIES)
            return FALSE;

        if (!NT_SUCCESS(Status))
        {
            DPRINT1("HVSOCK: relid %lu receive failed with 0x%lx\n", Connection->Channel->ChildRelId, Status);
            Connection->PeerClosed = TRUE;
            Connection->Reset = TRUE;
            return FALSE;
        }

        Descriptor = (PVMBUS_PACKET_DESCRIPTOR)Connection->RecvBuffer;
        Offset = (ULONG)Descriptor->DataOffset8 * 8;
        if (Descriptor->Type != VMBUS_PACKET_TYPE_INBAND || Offset + sizeof(Header) > Length)
            continue;

        RtlCopyMemory(&Header, Connection->RecvBuffer + Offset, sizeof(Header));
        if (Header.DataSize == 0)
        {
            /* The host closed its sending side */
            Connection->PeerClosed = TRUE;
            return FALSE;
        }

        Connection->RecvOffset = Offset + sizeof(Header);
        Connection->RecvEnd = min(Connection->RecvOffset + Header.DataSize, Length);
        if (Connection->RecvOffset < Connection->RecvEnd)
            return TRUE;
    }
}

static
NTSTATUS
HvsSendChunk(
    _Inout_ PHVS_CONNECTION Connection,
    _In_reads_bytes_(Length) const VOID *Data,
    _In_ ULONG Length)
{
    PVMBUS_CHANNEL Channel = Connection->Channel;
    HVS_PIPE_HEADER Header;
    ULONG Needed;
    NTSTATUS Status;

    Header.PacketType = HVS_PIPE_DATA;
    Header.DataSize = Length;
    RtlCopyMemory(Connection->SendBuffer, &Header, sizeof(Header));
    if (Length != 0)
        RtlCopyMemory(Connection->SendBuffer + sizeof(Header), Data, Length);

    Status = VmbChannelSendPacket(Channel, VMBUS_PACKET_TYPE_INBAND, 0, 0,
                                  Connection->SendBuffer, sizeof(Header) + Length);
    if (Status != STATUS_INSUFFICIENT_RESOURCES)
    {
        if (NT_SUCCESS(Status) && Channel->Open)
            InterlockedExchange((LONG volatile *)&Channel->Outbound.Control->PendingSendSize, 0);
        return Status;
    }

    /* Ask the host to signal once the packet fits, then try again in case it already does */
    Needed = ALIGN_UP_BY(sizeof(VMBUS_PACKET_DESCRIPTOR) + sizeof(Header) + Length, 8) + sizeof(ULONG64);
    InterlockedExchange((LONG volatile *)&Channel->Outbound.Control->PendingSendSize, (LONG)Needed);
    KeMemoryBarrier();
    Status = VmbChannelSendPacket(Channel, VMBUS_PACKET_TYPE_INBAND, 0, 0,
                                  Connection->SendBuffer, sizeof(Header) + Length);
    if (NT_SUCCESS(Status))
        InterlockedExchange((LONG volatile *)&Channel->Outbound.Control->PendingSendSize, 0);
    return Status;
}

static
VOID
HvsService(
    _Inout_ PHVS_CONNECTION Connection,
    _Inout_ PLIST_ENTRY Done)
{
    PIO_STACK_LOCATION Stack;
    PIRP Irp;
    ULONG Length;
    ULONG Sent;
    ULONG Chunk;
    NTSTATUS Status;

    if (Connection->State != HvsAccepted)
        return;

    /* Reads */
    while ((Irp = HvsPeekIrp(&Connection->ReadQueue)) != NULL)
    {
        Stack = IoGetCurrentIrpStackLocation(Irp);
        Length = Stack->Parameters.Read.Length;

        if (Connection->RecvOffset >= Connection->RecvEnd && !Connection->PeerClosed)
        {
            if (!Connection->Channel->Open || !HvsReceive(Connection))
            {
                if (!Connection->PeerClosed && Connection->Channel->Open)
                    break;
            }
        }

        if (!HvsTakeIrp(Irp))
            continue;

        if (Connection->RecvOffset < Connection->RecvEnd)
        {
            Length = min(Length, Connection->RecvEnd - Connection->RecvOffset);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Connection->RecvBuffer + Connection->RecvOffset, Length);
            Connection->RecvOffset += Length;
            HvsFinish(Done, Irp, STATUS_SUCCESS, Length);
        }
        else
        {
            /* End of stream */
            HvsFinish(Done, Irp, Connection->Reset ? STATUS_CONNECTION_RESET : STATUS_SUCCESS, 0);
        }
    }

    /* Writes */
    while ((Irp = HvsPeekIrp(&Connection->WriteQueue)) != NULL)
    {
        Stack = IoGetCurrentIrpStackLocation(Irp);
        Length = Stack->Parameters.Write.Length;
        Sent = (ULONG)Irp->IoStatus.Information;

        if (Connection->Channel->Rescinded || !Connection->Channel->Open || Connection->Reset)
        {
            if (HvsTakeIrp(Irp))
                HvsFinish(Done, Irp, STATUS_CONNECTION_RESET, Sent);
            continue;
        }

        Chunk = min(Length - Sent, (ULONG)HVS_SEND_CHUNK);
        Status = HvsSendChunk(Connection, (PUCHAR)Irp->AssociatedIrp.SystemBuffer + Sent, Chunk);
        if (Status == STATUS_INSUFFICIENT_RESOURCES)
            break;

        if (!NT_SUCCESS(Status))
        {
            if (HvsTakeIrp(Irp))
                HvsFinish(Done, Irp, STATUS_CONNECTION_RESET, Sent);
            continue;
        }

        Sent += Chunk;
        Irp->IoStatus.Information = Sent;
        if (Sent >= Length && HvsTakeIrp(Irp))
            HvsFinish(Done, Irp, STATUS_SUCCESS, Sent);
    }
}

static
VOID
NTAPI
HvsChannelCallback(
    _In_opt_ PVOID Context)
{
    PHVS_CONNECTION Connection = Context;
    LIST_ENTRY Done;
    KIRQL Irql;

    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    HvsService(Connection, &Done);
    KeReleaseSpinLock(&HvsLock, Irql);
    HvsCompleteDone(&Done);
}

/* Connection lifetime */

static
VOID
HvsFreeConnection(
    _In_ PHVS_CONNECTION Connection)
{
    if (Connection->WorkItem != NULL)
        IoFreeWorkItem(Connection->WorkItem);
    if (Connection->RecvBuffer != NULL)
        ExFreePoolWithTag(Connection->RecvBuffer, VMBUS_TAG);
    if (Connection->SendBuffer != NULL)
        ExFreePoolWithTag(Connection->SendBuffer, VMBUS_TAG);
    ExFreePoolWithTag(Connection, VMBUS_TAG);
}

/* PASSIVE_LEVEL: close the channel and give it back to the bus */
static
VOID
HvsCloseConnection(
    _In_ PHVS_CONNECTION Connection)
{
    PVMBUS_CHANNEL Channel = Connection->Channel;
    PVMBUS_FDO_EXTENSION Bus = Channel->Bus;
    LIST_ENTRY Done;
    BOOLEAN SendEof;
    KIRQL Irql;

    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    Connection->State = HvsClosing;
    HvsFlushQueue(&Connection->ReadQueue, &Done, STATUS_CANCELLED);
    HvsFlushQueue(&Connection->WriteQueue, &Done, STATUS_CANCELLED);
    SendEof = Channel->Open && !Channel->Rescinded;
    if (SendEof)
        HvsSendChunk(Connection, NULL, 0);
    KeReleaseSpinLock(&HvsLock, Irql);
    HvsCompleteDone(&Done);

    VmbChannelClose(Channel);
    DPRINT("HVSOCK: relid %lu closed%s\n", Channel->ChildRelId, Channel->Rescinded ? " (rescinded)" : "");

    KeAcquireSpinLock(&HvsLock, &Irql);
    Channel->Owner = NULL;
    KeReleaseSpinLock(&HvsLock, Irql);

    /* Frees the channel once the host has rescinded it (now or later) */
    VmbusQueueRelease(Bus);
    HvsFreeConnection(Connection);
}

static
VOID
NTAPI
HvsCloseWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    HvsCloseConnection(Context);
}

/* Called with HvsLock held: hand a ready connection to a waiting accept, or keep it */
static
VOID
HvsDeliver(
    _Inout_ PHVS_LISTENER Listener,
    _Inout_ PHVS_CONNECTION Connection,
    _Inout_ PLIST_ENTRY Done)
{
    PHVSOCK_ACCEPT_INFO Info;
    PIO_STACK_LOCATION Stack;
    PIRP Irp;

    while ((Irp = HvsPeekIrp(&Listener->AcceptQueue)) != NULL)
    {
        if (!HvsTakeIrp(Irp))
            continue;

        Stack = IoGetCurrentIrpStackLocation(Irp);
        Connection->State = HvsAccepted;
        Connection->Listener = NULL;
        HvsSetFile(Stack->FileObject, HvsFileConnection, Connection);

        Info = Irp->AssociatedIrp.SystemBuffer;
        Info->ServiceId = Connection->Channel->InterfaceType;
        Info->InstanceId = Connection->Channel->InterfaceInstance;
        HvsFinish(Done, Irp, STATUS_SUCCESS, sizeof(*Info));
        return;
    }

    Connection->State = HvsBacklog;
    Connection->Listener = Listener;
    InsertTailList(&Listener->Backlog, &Connection->ListEntry);
    Listener->BacklogCount++;
}

static
VOID
NTAPI
HvsOpenWorker(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_opt_ PVOID Context)
{
    PHVS_CONNECTION Connection = Context;
    PVMBUS_CHANNEL Channel = Connection->Channel;
    PHVS_LISTENER Listener;
    LIST_ENTRY Done;
    NTSTATUS Status;
    KIRQL Irql;

    UNREFERENCED_PARAMETER(DeviceObject);

    Status = VmbChannelOpen(Channel, HVS_RING_SIZE, HvsChannelCallback, Connection);
    DPRINT("HVSOCK: relid %lu open: 0x%lx\n", Channel->ChildRelId, Status);

    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    Listener = HvsFindListener(&Channel->InterfaceType);
    if (NT_SUCCESS(Status) && Listener != NULL && !Channel->Rescinded)
    {
        /* Data that arrives before the first read stays in the ring */
        HvsDeliver(Listener, Connection, &Done);
        KeReleaseSpinLock(&HvsLock, Irql);
        HvsCompleteDone(&Done);
        return;
    }
    KeReleaseSpinLock(&HvsLock, Irql);

    HvsCloseConnection(Connection);
}

/* DISPATCH_LEVEL, from the bus: a channel offer with the TLNPI provider flag */
VOID
HvsOnOffer(
    _In_ PVMBUS_CHANNEL Channel,
    _In_reads_bytes_(8) const UCHAR *PipeUserDefined)
{
    PHVS_CONNECTION Connection;
    PHVS_LISTENER Listener;

    /* Bytes 0-3: pipe mode; byte 4: the host opened the connection */
    if (HvsDevice == NULL || PipeUserDefined[4] == 0)
        return;

    KeAcquireSpinLockAtDpcLevel(&HvsLock);
    Listener = HvsFindListener(&Channel->InterfaceType);
    if (Listener == NULL || Listener->BacklogCount >= HVS_MAX_BACKLOG || Channel->Owner != NULL)
    {
        KeReleaseSpinLockFromDpcLevel(&HvsLock);
        return;
    }
    KeReleaseSpinLockFromDpcLevel(&HvsLock);

    Connection = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Connection), VMBUS_TAG);
    if (Connection == NULL)
        return;

    RtlZeroMemory(Connection, sizeof(*Connection));
    Connection->Channel = Channel;
    Connection->State = HvsOpening;
    InitializeListHead(&Connection->ReadQueue);
    InitializeListHead(&Connection->WriteQueue);
    Connection->WorkItem = IoAllocateWorkItem(HvsDevice);
    Connection->RecvBuffer = ExAllocatePoolWithTag(NonPagedPool, HVS_RECV_SIZE, VMBUS_TAG);
    Connection->SendBuffer = ExAllocatePoolWithTag(NonPagedPool, PAGE_SIZE, VMBUS_TAG);
    if (Connection->WorkItem == NULL || Connection->RecvBuffer == NULL || Connection->SendBuffer == NULL)
    {
        HvsFreeConnection(Connection);
        return;
    }

    KeAcquireSpinLockAtDpcLevel(&HvsLock);
    Channel->Owner = Connection;
    KeReleaseSpinLockFromDpcLevel(&HvsLock);

    IoQueueWorkItem(Connection->WorkItem, HvsOpenWorker, DelayedWorkQueue, Connection);
}

/* DISPATCH_LEVEL, from the bus: the host took the connection back */
VOID
HvsOnRescind(
    _In_ PVMBUS_CHANNEL Channel)
{
    PHVS_CONNECTION Connection;
    LIST_ENTRY Done;

    InitializeListHead(&Done);
    KeAcquireSpinLockAtDpcLevel(&HvsLock);
    Connection = Channel->Owner;
    if (Connection != NULL)
    {
        Connection->PeerClosed = TRUE;
        if (Connection->State == HvsBacklog)
        {
            /* Nobody accepted it yet: close it in the background */
            RemoveEntryList(&Connection->ListEntry);
            Connection->Listener->BacklogCount--;
            Connection->Listener = NULL;
            Connection->State = HvsClosing;
            IoQueueWorkItem(Connection->WorkItem, HvsCloseWorker, DelayedWorkQueue, Connection);
        }
        else
        {
            HvsService(Connection, &Done);
        }
    }
    KeReleaseSpinLockFromDpcLevel(&HvsLock);
    HvsCompleteDone(&Done);
}

/* The \\.\HvSocket device */

static
NTSTATUS
HvsComplete(
    _Inout_ PIRP Irp,
    _In_ NTSTATUS Status,
    _In_ ULONG_PTR Information)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

static
NTSTATUS
HvsCreate(
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    /* Host-only services: only LocalSystem (which holds the TCB privilege) gets in */
    if (Irp->RequestorMode != KernelMode &&
        !SeSinglePrivilegeCheck(RtlConvertLongToLuid(SE_TCB_PRIVILEGE), Irp->RequestorMode))
    {
        return HvsComplete(Irp, STATUS_ACCESS_DENIED, 0);
    }

    HvsSetFile(Stack->FileObject, HvsFileNone, NULL);
    return HvsComplete(Irp, STATUS_SUCCESS, 0);
}

static
NTSTATUS
HvsListen(
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PHVS_LISTENER Listener;
    KIRQL Irql;

    if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(GUID))
        return HvsComplete(Irp, STATUS_INVALID_PARAMETER, 0);

    Listener = ExAllocatePoolWithTag(NonPagedPool, sizeof(*Listener), VMBUS_TAG);
    if (Listener == NULL)
        return HvsComplete(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

    RtlZeroMemory(Listener, sizeof(*Listener));
    RtlCopyMemory(&Listener->ServiceId, Irp->AssociatedIrp.SystemBuffer, sizeof(GUID));
    InitializeListHead(&Listener->Backlog);
    InitializeListHead(&Listener->AcceptQueue);

    KeAcquireSpinLock(&HvsLock, &Irql);
    if (HvsFileType(Stack->FileObject) != HvsFileNone || HvsFindListener(&Listener->ServiceId) != NULL)
    {
        KeReleaseSpinLock(&HvsLock, Irql);
        ExFreePoolWithTag(Listener, VMBUS_TAG);
        return HvsComplete(Irp, STATUS_ADDRESS_ALREADY_EXISTS, 0);
    }
    InsertTailList(&HvsListeners, &Listener->ListEntry);
    HvsSetFile(Stack->FileObject, HvsFileListener, Listener);
    KeReleaseSpinLock(&HvsLock, Irql);

    DPRINT("HVSOCK: listening on {%08lx-...}\n", Listener->ServiceId.Data1);
    return HvsComplete(Irp, STATUS_SUCCESS, 0);
}

static
NTSTATUS
HvsAccept(
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PHVS_CONNECTION Connection;
    PHVS_LISTENER Listener;
    LIST_ENTRY Done;
    GUID ServiceId;
    KIRQL Irql;

    if (Stack->Parameters.DeviceIoControl.InputBufferLength < sizeof(GUID) ||
        Stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(HVSOCK_ACCEPT_INFO))
    {
        return HvsComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    RtlCopyMemory(&ServiceId, Irp->AssociatedIrp.SystemBuffer, sizeof(GUID));

    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    Listener = HvsFindListener(&ServiceId);
    /* A handle whose accept was cancelled may accept again */
    if (Listener == NULL ||
        (HvsFileType(Stack->FileObject) != HvsFileNone && HvsFileType(Stack->FileObject) != HvsFileAccepting))
    {
        KeReleaseSpinLock(&HvsLock, Irql);
        return HvsComplete(Irp, Listener == NULL ? STATUS_INVALID_ADDRESS : STATUS_INVALID_DEVICE_STATE, 0);
    }

    if (!HvsQueueIrp(&Listener->AcceptQueue, Irp))
    {
        KeReleaseSpinLock(&HvsLock, Irql);
        return HvsComplete(Irp, STATUS_CANCELLED, 0);
    }
    HvsSetFile(Stack->FileObject, HvsFileAccepting, NULL);

    if (!IsListEmpty(&Listener->Backlog))
    {
        Connection = CONTAINING_RECORD(RemoveHeadList(&Listener->Backlog), HVS_CONNECTION, ListEntry);
        Listener->BacklogCount--;
        HvsDeliver(Listener, Connection, &Done);
    }
    KeReleaseSpinLock(&HvsLock, Irql);

    HvsCompleteDone(&Done);
    return STATUS_PENDING;
}

static
NTSTATUS
HvsReadWrite(
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PHVS_CONNECTION Connection;
    LIST_ENTRY Done;
    ULONG Length;
    KIRQL Irql;

    Length = (Stack->MajorFunction == IRP_MJ_READ) ? Stack->Parameters.Read.Length : Stack->Parameters.Write.Length;
    if (Length == 0)
        return HvsComplete(Irp, STATUS_SUCCESS, 0);

    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    if (HvsFileType(Stack->FileObject) != HvsFileConnection)
    {
        KeReleaseSpinLock(&HvsLock, Irql);
        return HvsComplete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    }

    Connection = Stack->FileObject->FsContext;
    Irp->IoStatus.Information = 0;
    if (!HvsQueueIrp((Stack->MajorFunction == IRP_MJ_READ) ? &Connection->ReadQueue : &Connection->WriteQueue, Irp))
    {
        KeReleaseSpinLock(&HvsLock, Irql);
        return HvsComplete(Irp, STATUS_CANCELLED, 0);
    }
    HvsService(Connection, &Done);
    KeReleaseSpinLock(&HvsLock, Irql);

    HvsCompleteDone(&Done);
    return STATUS_PENDING;
}

static
NTSTATUS
HvsCleanup(
    _Inout_ PIRP Irp,
    _In_ PIO_STACK_LOCATION Stack)
{
    PFILE_OBJECT File = Stack->FileObject;
    PHVS_CONNECTION Connection = NULL;
    PHVS_LISTENER Listener = NULL;
    LIST_ENTRY Backlog;
    LIST_ENTRY Done;
    PLIST_ENTRY Entry;
    PIRP Pending;
    KIRQL Irql;

    InitializeListHead(&Backlog);
    InitializeListHead(&Done);
    KeAcquireSpinLock(&HvsLock, &Irql);
    switch (HvsFileType(File))
    {
        case HvsFileListener:
            Listener = File->FsContext;
            RemoveEntryList(&Listener->ListEntry);
            HvsFlushQueue(&Listener->AcceptQueue, &Done, STATUS_CANCELLED);
            while (!IsListEmpty(&Listener->Backlog))
            {
                Entry = RemoveHeadList(&Listener->Backlog);
                CONTAINING_RECORD(Entry, HVS_CONNECTION, ListEntry)->State = HvsClosing;
                InsertTailList(&Backlog, Entry);
            }
            break;

        case HvsFileAccepting:
            /* Its accept is queued on some listener; find and fail it */
            for (Entry = HvsListeners.Flink; Entry != &HvsListeners; Entry = Entry->Flink)
            {
                PHVS_LISTENER Candidate = CONTAINING_RECORD(Entry, HVS_LISTENER, ListEntry);
                PLIST_ENTRY IrpEntry;

                for (IrpEntry = Candidate->AcceptQueue.Flink; IrpEntry != &Candidate->AcceptQueue; IrpEntry = IrpEntry->Flink)
                {
                    Pending = CONTAINING_RECORD(IrpEntry, IRP, Tail.Overlay.ListEntry);
                    if (IoGetCurrentIrpStackLocation(Pending)->FileObject == File && HvsTakeIrp(Pending))
                    {
                        HvsFinish(&Done, Pending, STATUS_CANCELLED, 0);
                        break;
                    }
                }
            }
            break;

        case HvsFileConnection:
            Connection = File->FsContext;
            break;

        default:
            break;
    }
    HvsSetFile(File, HvsFileNone, NULL);
    KeReleaseSpinLock(&HvsLock, Irql);
    HvsCompleteDone(&Done);

    while (!IsListEmpty(&Backlog))
        HvsCloseConnection(CONTAINING_RECORD(RemoveHeadList(&Backlog), HVS_CONNECTION, ListEntry));

    if (Connection != NULL)
        HvsCloseConnection(Connection);

    if (Listener != NULL)
        ExFreePoolWithTag(Listener, VMBUS_TAG);

    return HvsComplete(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS
HvsDispatch(
    _In_ PDEVICE_OBJECT DeviceObject,
    _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

    UNREFERENCED_PARAMETER(DeviceObject);

    switch (Stack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            return HvsCreate(Irp);

        case IRP_MJ_CLOSE:
            return HvsComplete(Irp, STATUS_SUCCESS, 0);

        case IRP_MJ_CLEANUP:
            return HvsCleanup(Irp, Stack);

        case IRP_MJ_READ:
        case IRP_MJ_WRITE:
            return HvsReadWrite(Irp, Stack);

        case IRP_MJ_DEVICE_CONTROL:
            switch (Stack->Parameters.DeviceIoControl.IoControlCode)
            {
                case IOCTL_HVSOCK_LISTEN:
                    return HvsListen(Irp, Stack);
                case IOCTL_HVSOCK_ACCEPT:
                    return HvsAccept(Irp, Stack);
                default:
                    break;
            }
            break;

        default:
            break;
    }

    return HvsComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

BOOLEAN
HvsIsDevice(
    _In_ PDEVICE_OBJECT DeviceObject)
{
    return HvsDevice != NULL && DeviceObject == HvsDevice;
}

NTSTATUS
HvsInitialize(
    _In_ PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING Name = RTL_CONSTANT_STRING(HVSOCK_DEVICE_NAME);
    UNICODE_STRING Link = RTL_CONSTANT_STRING(HVSOCK_DOS_DEVICE_NAME);
    PDEVICE_OBJECT Device;
    NTSTATUS Status;

    KeInitializeSpinLock(&HvsLock);
    InitializeListHead(&HvsListeners);

    Status = IoCreateDevice(DriverObject, 0, &Name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE, &Device);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = IoCreateSymbolicLink(&Link, &Name);
    if (!NT_SUCCESS(Status))
    {
        IoDeleteDevice(Device);
        return Status;
    }

    Device->Flags |= DO_BUFFERED_IO;
    Device->Flags &= ~DO_DEVICE_INITIALIZING;
    HvsDevice = Device;
    return STATUS_SUCCESS;
}
