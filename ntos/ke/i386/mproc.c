/*
 * PROJECT:     LiberNT Kernel
 * LICENSE:     GPL-3.0-or-later (https://spdx.org/licenses/GPL-3.0-or-later)
 * PURPOSE:     i386 multiprocessor startup helpers
 * COPYRIGHT:   Copyright 2023 Justin Miller <justin.miller@reactos.org>
 *              Copyright 2023 Victor Perevertkin <victor.perevertkin@reactos.org>
 *              Copyright 2026 Ahmed ARIF <arif.ing@outlook.com>
 */

#include <ntoskrnl.h>

#define NDEBUG
#include <debug.h>

/* An application processor that has not reported in within 5 s is hung */
#define AP_START_WAIT_TIME (5 * 10000000ULL)

typedef struct _APINFO
{
    DECLSPEC_ALIGN(PAGE_SIZE) KIDTENTRY Idt[256];
    DECLSPEC_ALIGN(PAGE_SIZE) KGDTENTRY Gdt[128];
    DECLSPEC_ALIGN(16) UINT8 NMIStackData[DOUBLE_FAULT_STACK_SIZE];
    KIPCR Pcr;
    ETHREAD Thread;
    KTSS Tss;
    KTSS TssDoubleFault;
    KTSS TssNMI;
} APINFO, *PAPINFO;

typedef struct _AP_SETUP_STACK
{
    PVOID ReturnAddr;
    PVOID KxLoaderBlock;
} AP_SETUP_STACK, *PAP_SETUP_STACK;

/* The double fault and NMI arrive through task gates: their TSS is a whole context */
static
CODE_SEG("INIT")
VOID
KiInitializeApTrapTss(
    _Out_ PKTSS Tss,
    _In_ PVOID Handler,
    _In_ PVOID Stack)
{
    KiInitializeTSS(Tss);
    Tss->CR3 = __readcr3();
    Tss->Esp0 = (ULONG_PTR)Stack;
    Tss->Esp = (ULONG_PTR)Stack;
    Tss->Eip = PtrToUlong(Handler);
    Tss->Cs = KGDT_R0_CODE;
    Tss->Fs = KGDT_R0_PCR;
    Tss->Ss = KGDT_R0_DATA;
    Tss->Es = KGDT_R3_DATA | RPL_MASK;
    Tss->Ds = KGDT_R3_DATA | RPL_MASK;
}

CODE_SEG("INIT")
VOID
NTAPI
KeStartAllProcessors(VOID)
{
    PVOID KernelStack = NULL, DPCStack = NULL;
    PAPINFO APInfo = NULL;
    ULONG ProcessorCount;
    ULONG MaximumProcessors;
    ULONG Index;
    ULONGLONG StartTime;

    /* NOTE: NT6+ HAL exports HalEnumerateProcessors() and
     * HalQueryMaximumProcessorCount() that help determining
     * the number of detected processors on the system. */
    MaximumProcessors = KeMaximumProcessors;

    if (KeNumprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeNumprocSpecified);

    if (KeBootprocSpecified)
        MaximumProcessors = min(MaximumProcessors, KeBootprocSpecified);

    /* Start ProcessorCount at 1 because we already have the boot CPU */
    // TODO: Support processor nodes
    for (ProcessorCount = 1; ProcessorCount < MaximumProcessors; ++ProcessorCount)
    {
        KernelStack = NULL;
        DPCStack = NULL;

        APInfo = ExAllocatePoolZero(NonPagedPool, sizeof(*APInfo), TAG_KERNEL);
        if (!APInfo)
            break;
        ASSERT(ALIGN_DOWN_POINTER_BY(APInfo, PAGE_SIZE) == APInfo);

        KernelStack = MmCreateKernelStack(FALSE, 0);
        if (!KernelStack)
            break;

        DPCStack = MmCreateKernelStack(FALSE, 0);
        if (!DPCStack)
            break;

        KiInitializePcr(ProcessorCount,
                        &APInfo->Pcr,
                        &APInfo->Idt[0],
                        &APInfo->Gdt[0],
                        &APInfo->Tss,
                        (PKTHREAD)&APInfo->Thread,
                        DPCStack);

        KDESCRIPTOR bspGdt, bspIdt;
        __sgdt(&bspGdt.Limit);
        __sidt(&bspIdt.Limit);
        RtlCopyMemory(&APInfo->Gdt, (PVOID)bspGdt.Base, bspGdt.Limit + 1);
        RtlCopyMemory(&APInfo->Idt, (PVOID)bspIdt.Base, bspIdt.Limit + 1);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_R0_PCR), (ULONG_PTR)&APInfo->Pcr);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_DF_TSS), (ULONG_PTR)&APInfo->TssDoubleFault);
        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_NMI_TSS), (ULONG_PTR)&APInfo->TssNMI);

        KiSetGdtDescriptorBase(KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS), (ULONG_PTR)&APInfo->Tss);

        KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS)->HighWord.Bits.Type = I386_TSS;

        /* The boot TSS: ring 0 stack segment and I/O maps, as the boot processor's */
        KiInitializeTSS2(&APInfo->Tss, KiGetGdtEntry(&APInfo->Gdt, KGDT_TSS));
        KiInitializeTSS(&APInfo->Tss);

        /* Both trap tasks share the stack; it grows down from the end */
        KiInitializeApTrapTss(&APInfo->TssDoubleFault,
                              KiTrap08,
                              &APInfo->NMIStackData[DOUBLE_FAULT_STACK_SIZE]);
        KiInitializeApTrapTss(&APInfo->TssNMI,
                              KiTrap02,
                              &APInfo->NMIStackData[DOUBLE_FAULT_STACK_SIZE]);

        PKPROCESSOR_STATE ProcessorState = &APInfo->Pcr.Prcb->ProcessorState;
        RtlZeroMemory(ProcessorState, sizeof(*ProcessorState));

        ProcessorState->SpecialRegisters.Cr0 = __readcr0();
        ProcessorState->SpecialRegisters.Cr3 = __readcr3();
        ProcessorState->SpecialRegisters.Cr4 = __readcr4();

        ProcessorState->ContextFrame.SegCs = KGDT_R0_CODE;
        ProcessorState->ContextFrame.SegDs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegEs = KGDT_R3_DATA;
        ProcessorState->ContextFrame.SegSs = KGDT_R0_DATA;
        ProcessorState->ContextFrame.SegFs = KGDT_R0_PCR;

        ProcessorState->SpecialRegisters.Gdtr.Base = (ULONG_PTR)APInfo->Gdt;
        ProcessorState->SpecialRegisters.Gdtr.Limit = sizeof(APInfo->Gdt) - 1;
        ProcessorState->SpecialRegisters.Idtr.Base = (ULONG_PTR)APInfo->Idt;
        ProcessorState->SpecialRegisters.Idtr.Limit = sizeof(APInfo->Idt) - 1;

        ProcessorState->SpecialRegisters.Tr = KGDT_TSS;

        ProcessorState->ContextFrame.Esp = (ULONG_PTR)KernelStack;
        ProcessorState->ContextFrame.Eip = (ULONG_PTR)KiSystemStartup;
        ProcessorState->ContextFrame.EFlags = __readeflags() & ~EFLAGS_INTERRUPT_MASK;

        ProcessorState->ContextFrame.Esp = (ULONG)((ULONG_PTR)ProcessorState->ContextFrame.Esp - sizeof(AP_SETUP_STACK));
        PAP_SETUP_STACK ApStack = (PAP_SETUP_STACK)ProcessorState->ContextFrame.Esp;
        ApStack->KxLoaderBlock = KeLoaderBlock;
        ApStack->ReturnAddr = NULL;

        // Update the LOADER_PARAMETER_BLOCK structure for the new processor
        ASSERT(KeLoaderBlock->Prcb == 0);
        KeLoaderBlock->KernelStack = (ULONG_PTR)KernelStack;
        KeLoaderBlock->Prcb = (ULONG_PTR)APInfo->Pcr.Prcb;
        KeLoaderBlock->Thread = (ULONG_PTR)&APInfo->Thread;

        // Start the CPU
        DPRINT1("KeStartAllProcessors: starting CPU %lu\n", ProcessorCount);
        if (!HalStartNextProcessor(KeLoaderBlock, ProcessorState))
        {
            break;
        }

        /* Wait for the AP to consume the loader block (KiInitializeKernel) */
        StartTime = KeQueryInterruptTime();
        while (KeLoaderBlock->Prcb != 0)
        {
            if ((KeQueryInterruptTime() - StartTime) >= AP_START_WAIT_TIME)
            {
                KeBugCheckEx(CLOCK_WATCHDOG_TIMEOUT,
                             (ULONG_PTR)(AP_START_WAIT_TIME / KeMaximumIncrement),
                             0,
                             (ULONG_PTR)APInfo->Pcr.Prcb,
                             ProcessorCount);
            }

            KeMemoryBarrier();
            YieldProcessor();
        }

        /* The structures now belong to the running processor */
        APInfo = NULL;
        KernelStack = NULL;
        DPCStack = NULL;
    }

    ProcessorCount--;

    /* Each started processor makes itself active once it can take an IPI */
    StartTime = KeQueryInterruptTime();
    for (Index = 1; Index <= ProcessorCount; Index++)
    {
        while ((KeActiveProcessors & AFFINITY_MASK(Index)) == 0)
        {
            if ((KeQueryInterruptTime() - StartTime) >= AP_START_WAIT_TIME)
            {
                KeBugCheckEx(CLOCK_WATCHDOG_TIMEOUT,
                             (ULONG_PTR)(AP_START_WAIT_TIME / KeMaximumIncrement),
                             0,
                             (ULONG_PTR)KiProcessorBlock[Index],
                             Index);
            }

            KeMemoryBarrier();
            YieldProcessor();
        }
    }

    if (APInfo)
        ExFreePoolWithTag(APInfo, TAG_KERNEL);
    if (KernelStack)
        MmDeleteKernelStack(KernelStack, FALSE);
    if (DPCStack)
        MmDeleteKernelStack(DPCStack, FALSE);

    DPRINT1("KeStartAllProcessors: Successful AP startup count is %lu, %u processor(s) active (mask 0x%lx)\n",
            ProcessorCount, KeNumberProcessors, KeActiveProcessors);
}
