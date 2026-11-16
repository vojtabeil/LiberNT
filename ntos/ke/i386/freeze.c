/*
 * PROJECT:     ReactOS Kernel
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Processor freeze support for i386
 * COPYRIGHT:   Copyright 2023-2024 Timo Kreuzer <timo.kreuzer@reactos.org>
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

/*
 * This is the amd64 freeze (ke/amd64/freeze.c) with the same IpiFrozen state
 * graph. The differences are in how a target is reached: i386 has no NMI
 * freeze, the request travels as IPI_FREEZE behind the ordinary IPI vector,
 * and a processor that spins with interrupts off picks it up through
 * KiFreezeIfRequested from the interprocessor request loops. While frozen, a
 * target keeps serving packet requests, so that the processor holding the
 * machine can still flush the other translation buffers.
 */

/* INCLUDES *******************************************************************/

#include <ntoskrnl.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static PKPRCB KiFreezeOwner;
static PKPRCB KiFreezeActiveSetOwner;
static volatile LONG64 KiFreezeEpoch;

/* How long a freeze, a thaw or the active set may keep a processor waiting */
#define KI_FREEZE_WAIT_SECONDS 10

typedef struct _KI_FREEZE_WAIT
{
    ULONG64 Start;
    ULONG64 Now;
    ULONG64 Frequency;
    ULONG64 Epoch;
    BOOLEAN Valid;
} KI_FREEZE_WAIT;

static VOID KiFreezeWaitForThaw(_In_ PKPRCB CurrentPrcb);

/* FUNCTIONS *****************************************************************/

static VOID
KiNoteFreezeEpoch(VOID)
{
    InterlockedIncrement64(&KiFreezeEpoch);
}

static ULONG64
KiQueryFreezeEpoch(VOID)
{
    return (ULONG64)InterlockedCompareExchange64(&KiFreezeEpoch, 0, 0);
}

BOOLEAN
NTAPI
KxIsFreezeOwner(
    _In_ PKPRCB Prcb)
{
    return KiFreezeOwner == Prcb;
}

/* After a failed freeze only the processor that owns the stop goes on; any other one halts */
static BOOLEAN
KiFreezeFailureRequiresReturn(
    _In_ PKPRCB Prcb)
{
    if (!KiHasFreezeFailure())
        return FALSE;
    if (KiIsBugCheckEntryOwner(Prcb))
        return TRUE;
    for (;;)
        HalHaltSystem();
}

/* Not counted while the debugger holds the machine or timing is relaxed; starts over when another freeze begins */
static BOOLEAN
KiFreezeWaitExpired(
    _Inout_ KI_FREEZE_WAIT *Wait)
{
    LARGE_INTEGER Counter, Frequency;
    ULONG64 Before, After;

    if (KiRelaxedTiming || KdEnteredDebugger)
    {
        Wait->Valid = FALSE;
        return FALSE;
    }

    Before = KiQueryFreezeEpoch();
    Counter = KeQueryPerformanceCounter(&Frequency);
    After = KiQueryFreezeEpoch();
    if (Before != After || Frequency.QuadPart <= 0)
    {
        Wait->Valid = FALSE;
        return FALSE;
    }

    Wait->Now = (ULONG64)Counter.QuadPart;
    Wait->Frequency = (ULONG64)Frequency.QuadPart;
    if (!Wait->Valid || Wait->Epoch != After || Wait->Now < Wait->Start)
    {
        Wait->Start = Wait->Now;
        Wait->Epoch = After;
        Wait->Valid = TRUE;
        return FALSE;
    }

    return (Wait->Now - Wait->Start) / Wait->Frequency >= KI_FREEZE_WAIT_SECONDS;
}

/* Answers TRUE when the caller is to stop waiting: it owns the stop and carries on with it */
static BOOLEAN
KiFreezeWaitFailed(
    _In_ PKPRCB Prcb,
    _Inout_ KI_FREEZE_WAIT *Wait)
{
    if (KiFreezeFailureRequiresReturn(Prcb))
        return TRUE;
    if (KiRelaxedTiming || KdEnteredDebugger ||
        KiQueryFreezeEpoch() != Wait->Epoch)
    {
        Wait->Valid = FALSE;
        return FALSE;
    }

    KiSetFreezeFailure();
    if (KiIsBugCheckEntryOwner(Prcb))
        return TRUE;

    KeBugCheckEx(IPI_WATCHDOG_TIMEOUT,
                 (ULONG_PTR)Wait->Frequency,
                 (ULONG_PTR)Wait->Now,
                 (ULONG_PTR)Wait->Start,
                 0);
}

/* Called from the IPI service routine with the interrupted context */
BOOLEAN
KiProcessorFreezeHandler(
    _In_ PKTRAP_FRAME TrapFrame,
    _In_opt_ PKEXCEPTION_FRAME ExceptionFrame)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();

    /* Make sure this is a freeze request */
    if ((CurrentPrcb == NULL) ||
        (TrapFrame == NULL) ||
        (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE))
    {
        /* Not a freeze request, return FALSE to signal it is unhandled */
        return FALSE;
    }

    if (KiIsBugCheckEntryOwner(CurrentPrcb))
        return TRUE;

    /* Never freeze while holding the KD port: the release path completes
       the request through KiFreezeIfRequested */
    if (KdpPortOwnerPrcb == CurrentPrcb)
    {
        return TRUE;
    }

    /* Claim the request, but do not publish a stable context before saving it. */
    if (InterlockedCompareExchange((PLONG)&CurrentPrcb->IpiFrozen, IPI_FROZEN_STATE_SAVING, IPI_FROZEN_STATE_TARGET_FREEZE) !=
        IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        return FALSE;
    }

    /* Save the processor state */
    KiSaveProcessorState(TrapFrame, ExceptionFrame);
    KeMemoryBarrier();

    /* The owner may consume ProcessorState only after this publication. */
    InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen, IPI_FROZEN_STATE_FROZEN);

    /* Wait for the freeze owner to release us */
    KiFreezeWaitForThaw(CurrentPrcb);

    /* TODO: i386 does not write a context changed by the debugger back to the trap frame */

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Return TRUE to signal that we handled the freeze */
    return TRUE;
}

/* Spin until the owner thaws us, servicing a Kd processor switch while frozen */
static VOID
KiFreezeWaitForThaw(
    _In_ PKPRCB CurrentPrcb)
{
    while (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_THAW)
    {
        if (KiFreezeFailureRequiresReturn(CurrentPrcb))
            return;

        /* Check for Kd processor switch */
        if (CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE)
        {
            KCONTINUE_STATUS ContinueStatus;

            /* Enter the debugger */
            ContinueStatus = KdReportProcessorChange();

            /* Set the state back to frozen */
            CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_FROZEN;

            /* If the status is ContinueSuccess, we need to release the freeze owner */
            if (ContinueStatus == ContinueSuccess)
            {
                /* Release the freeze owner */
                KiFreezeOwner->IpiFrozen = IPI_FROZEN_STATE_THAW;
            }
        }

        /* The owner may still flush the translation buffers */
        KiIpiServiceFrozenRequests();

        YieldProcessor();
        KeMemoryBarrier();
    }
}

/* Complete a freeze request with no trap frame: the published context is the caller's own */
VOID
NTAPI
KiFreezeIfRequested(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    BOOLEAN Enable;

    if (CurrentPrcb->IpiFrozen != IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        return;
    }

    if (KiIsBugCheckEntryOwner(CurrentPrcb))
        return;

    /* Same rule as the IPI path: never freeze while holding the KD port */
    if (KdpPortOwnerPrcb == CurrentPrcb)
    {
        return;
    }

    Enable = KeDisableInterrupts();
    if (InterlockedCompareExchange((PLONG)&CurrentPrcb->IpiFrozen, IPI_FROZEN_STATE_SAVING, IPI_FROZEN_STATE_TARGET_FREEZE) ==
        IPI_FROZEN_STATE_TARGET_FREEZE)
    {
        RtlCaptureContext(&CurrentPrcb->ProcessorState.ContextFrame);
        KiSaveProcessorControlState(&CurrentPrcb->ProcessorState);
        KeMemoryBarrier();

        /* The owner may consume ProcessorState only after this publication. */
        InterlockedExchange((PLONG)&CurrentPrcb->IpiFrozen, IPI_FROZEN_STATE_FROZEN);

        KiFreezeWaitForThaw(CurrentPrcb);

        /* We are running again now */
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;
    }
    KeRestoreInterrupts(Enable);
}

static
BOOLEAN
KiAcquireFreezeActiveSet(
    _In_ PKPRCB Prcb)
{
    KI_FREEZE_WAIT Wait = {0};

    ASSERT(Prcb != NULL);
    ASSERT(Prcb == KeGetCurrentPrcb());

    if (KiFreezeFailureRequiresReturn(Prcb))
        return FALSE;

    if (InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner,
                                          NULL,
                                          NULL) == Prcb)
        return FALSE;

    while (InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner,
                                             Prcb,
                                             NULL) != NULL)
    {
        if (KiFreezeFailureRequiresReturn(Prcb))
            return FALSE;
        if (KiFreezeWaitExpired(&Wait) &&
            InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner,
                                              NULL,
                                              NULL) != NULL &&
            KiFreezeWaitFailed(Prcb, &Wait))
            return FALSE;
        YieldProcessor();
    }

    return TRUE;
}

static
VOID
KiReleaseFreezeActiveSet(
    _In_ PKPRCB Prcb,
    _In_ BOOLEAN Acquired)
{
    ASSERT(Prcb == KeGetCurrentPrcb());
    ASSERT(KiFreezeActiveSetOwner == Prcb);

    if (Acquired)
        InterlockedExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner, NULL);
}

/* Active means the processor can take an IPI, and a frozen machine takes no new processor */
CODE_SEG("INIT")
VOID
NTAPI
KiPublishActiveProcessor(
    _In_ PKPRCB Prcb)
{
    BOOLEAN Acquired;

    ASSERT(Prcb != NULL);
    ASSERT(Prcb == KeGetCurrentPrcb());

    for (;;)
    {
        Acquired = KiAcquireFreezeActiveSet(Prcb);
        if (KiHasFreezeFailure())
        {
            for (;;)
                HalHaltSystem();
        }
        ASSERT(Acquired);

        if (InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeOwner,
                                              NULL,
                                              NULL) == NULL)
        {
            InterlockedOr((PLONG)&KeActiveProcessors, (LONG)Prcb->SetMember);
            KiReleaseFreezeActiveSet(Prcb, Acquired);
            break;
        }

        KiReleaseFreezeActiveSet(Prcb, Acquired);
        YieldProcessor();
    }
}

/* Mark, send to and wait for one snapshot of the published, active processors */
static
KAFFINITY
KiFreezeTargetSet(
    _In_ PKPRCB CurrentPrcb)
{
    KAFFINITY Targets = 0;
    ULONG Index;
    PKPRCB TargetPrcb;

    for (Index = 0; Index < (ULONG)KeNumberProcessors && Index < MAXIMUM_PROCESSORS; Index++)
    {
        TargetPrcb = KiProcessorBlock[Index];
        if (TargetPrcb == NULL || TargetPrcb == CurrentPrcb)
            continue;

        if ((KeActiveProcessors & TargetPrcb->SetMember) == 0)
            continue;

        Targets |= TargetPrcb->SetMember;
    }

    return Targets;
}

VOID
NTAPI
KxFreezeExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    BOOLEAN ReacquireActiveSet = FALSE;
    BOOLEAN Acquired;
    KAFFINITY Targets;
    KI_FREEZE_WAIT Wait = {0};
    ULONG i;

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;

    /* Avoid blocking on recursive debug action */
    if (KiFreezeOwner == CurrentPrcb)
    {
        return;
    }

    /* Try to acquire the freeze owner */
    while (InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeOwner, CurrentPrcb, NULL))
    {
        /* The owner snapshots under the active set: do not hold it while waiting for that owner */
        if (!ReacquireActiveSet &&
            InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner,
                                              NULL,
                                              NULL) == CurrentPrcb)
        {
            InterlockedExchangePointer((volatile PVOID *)&KiFreezeActiveSetOwner, NULL);
            ReacquireActiveSet = TRUE;
        }

        /* Someone else was faster. Its freeze request reaches us here at
           the latest, since interrupts are off on this processor. */
        while (KiFreezeOwner != NULL)
        {
            if (KiFreezeFailureRequiresReturn(CurrentPrcb))
                return;
            if (KiFreezeWaitExpired(&Wait) &&
                InterlockedCompareExchangePointer((volatile PVOID *)&KiFreezeOwner,
                                                  NULL,
                                                  NULL) != NULL &&
                KiFreezeWaitFailed(CurrentPrcb, &Wait))
                return;
            KiFreezeIfRequested();
            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    KiNoteFreezeEpoch();

    /* We are the owner now and active */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;

    /* No processor becomes active between the snapshot and the thaw */
    Acquired = KiAcquireFreezeActiveSet(CurrentPrcb);
    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;
    Targets = KiFreezeTargetSet(CurrentPrcb);
    Wait.Valid = FALSE;
    KiReleaseFreezeActiveSet(CurrentPrcb, Acquired && !ReacquireActiveSet);

    /* Mark exactly the snapshot */
    for (i = 0; i < (ULONG)KeNumberProcessors && i < MAXIMUM_PROCESSORS; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (KiFreezeFailureRequiresReturn(CurrentPrcb))
            return;
        if (TargetPrcb == NULL || (Targets & TargetPrcb->SetMember) == 0)
            continue;

        /* Only the active processor is allowed to change IpiFrozen */
        ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING);

        RtlZeroMemory(&TargetPrcb->ProcessorState.ContextFrame, sizeof(TargetPrcb->ProcessorState.ContextFrame));
        KeMemoryBarrier();

        /* Request target to freeze */
        TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_TARGET_FREEZE;
    }

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;

    /* Send the freeze IPI to exactly the same set */
    if (Targets != 0)
        KiIpiSend(Targets, IPI_FREEZE);

    /* Wait for exactly the same set to be frozen */
    for (i = 0; i < (ULONG)KeNumberProcessors && i < MAXIMUM_PROCESSORS; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb == NULL || (Targets & TargetPrcb->SetMember) == 0)
            continue;

        while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN)
        {
            BOOLEAN StopOwnerPending = FALSE;
            ULONG j;

            if (KiFreezeFailureRequiresReturn(CurrentPrcb))
                return;

            /* The processor that owns the stop never freezes: leave the machine to it now rather than after the watchdog */
            for (j = i; j < (ULONG)KeNumberProcessors && j < MAXIMUM_PROCESSORS && !StopOwnerPending; j++)
            {
                PKPRCB PendingPrcb = KiProcessorBlock[j];

                StopOwnerPending = PendingPrcb != NULL && (Targets & PendingPrcb->SetMember) != 0 &&
                                   KiIsBugCheckEntryOwner(PendingPrcb);
            }

            if (StopOwnerPending)
            {
                KiSetFreezeFailure();
                if (KiFreezeFailureRequiresReturn(CurrentPrcb))
                    return;
            }

            if (KiFreezeWaitExpired(&Wait) &&
                TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_FROZEN &&
                KiFreezeWaitFailed(CurrentPrcb, &Wait))
                return;
            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;

    /* All targets are frozen, we can continue */
}

VOID
NTAPI
KxThawExecution(
    VOID)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    KI_FREEZE_WAIT Wait = {0};
    ULONG i;

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);

    /* Thaw the marked set, not whatever KeNumberProcessors says now */
    for (i = 0; i < (ULONG)KeNumberProcessors && i < MAXIMUM_PROCESSORS; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (KiFreezeFailureRequiresReturn(CurrentPrcb))
            return;
        if (TargetPrcb == NULL || TargetPrcb == CurrentPrcb)
            continue;

        if (TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_RUNNING)
            continue;

        /* Make sure they are still frozen */
        ASSERT(TargetPrcb->IpiFrozen == IPI_FROZEN_STATE_FROZEN);

        /* Request target to thaw */
        TargetPrcb->IpiFrozen = IPI_FROZEN_STATE_THAW;
    }

    /* Wait for all targets to be running */
    for (i = 0; i < (ULONG)KeNumberProcessors && i < MAXIMUM_PROCESSORS; i++)
    {
        PKPRCB TargetPrcb = KiProcessorBlock[i];
        if (TargetPrcb == NULL || TargetPrcb == CurrentPrcb)
            continue;

        while (TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING)
        {
            if (KiFreezeFailureRequiresReturn(CurrentPrcb))
                return;
            if (KiFreezeWaitExpired(&Wait) &&
                TargetPrcb->IpiFrozen != IPI_FROZEN_STATE_RUNNING &&
                KiFreezeWaitFailed(CurrentPrcb, &Wait))
                return;
            YieldProcessor();
            KeMemoryBarrier();
        }
    }

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return;

    /* We are running again now */
    CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_RUNNING;

    /* Release the freeze owner */
    InterlockedExchangePointer((volatile PVOID *)&KiFreezeOwner, NULL);
}

KCONTINUE_STATUS
NTAPI
KxSwitchKdProcessor(
    _In_ ULONG ProcessorIndex)
{
    PKPRCB CurrentPrcb = KeGetCurrentPrcb();
    PKPRCB TargetPrcb;

    if (KiFreezeFailureRequiresReturn(CurrentPrcb))
        return ContinueSuccess;

    /* Make sure that the processor index is valid */
    ASSERT(ProcessorIndex < (ULONG)KeNumberProcessors);

    /* We are no longer active */
    ASSERT(CurrentPrcb->IpiFrozen & IPI_FROZEN_FLAG_ACTIVE);
    CurrentPrcb->IpiFrozen &= ~IPI_FROZEN_FLAG_ACTIVE;

    /* Inform the target processor that it's his turn now */
    TargetPrcb = KiProcessorBlock[ProcessorIndex];
    TargetPrcb->IpiFrozen |= IPI_FROZEN_FLAG_ACTIVE;

    /* If we are not the freeze owner, we return back to the freeze loop */
    if (KiFreezeOwner != CurrentPrcb)
    {
        return ContinueNextProcessor;
    }

    /* Loop until it's our turn again */
    while (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_OWNER)
    {
        if (KiFreezeFailureRequiresReturn(CurrentPrcb))
            return ContinueSuccess;
        YieldProcessor();
        KeMemoryBarrier();
    }

    /* Check if we have been thawed */
    if (CurrentPrcb->IpiFrozen == IPI_FROZEN_STATE_THAW)
    {
        /* Another CPU has completed, we can leave the debugger now */
        CurrentPrcb->IpiFrozen = IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE;
        return ContinueSuccess;
    }

    /* We have been reselected, return to Kd to continue in the debugger */
    ASSERT(CurrentPrcb->IpiFrozen == (IPI_FROZEN_STATE_OWNER | IPI_FROZEN_FLAG_ACTIVE));

    return ContinueProcessorReselected;
}

/* EOF */
