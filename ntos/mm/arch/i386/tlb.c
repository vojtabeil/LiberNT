#include <mm/nt/mint.h>

/* Beyond this many pages a whole flush is cheaper than one invalidation per page */
#define MI_I386_TLB_RANGE_LIMIT 256

static VOID
MiI386FlushLocalAll(VOID)
{
    ULONG Cr4 = __readcr4();

    if (Cr4 & CR4_PGE)
    {
        __writecr4(Cr4 & ~CR4_PGE);
        __writecr3(__readcr3());
        __writecr4(Cr4);
    }
    else
    {
        __writecr3(__readcr3());
    }
}

/* Pages == 0 flushes everything, global pages included */
static VOID
MiI386FlushLocal(ULONG_PTR Address, ULONG Pages)
{
    if (Pages == 0)
    {
        MiI386FlushLocalAll();
        return;
    }

    do
    {
        __invlpg((PVOID)Address);
        Address += PAGE_SIZE;
    } while (--Pages != 0);
}

/* Runs on each target at IPI level: the packet carries the first page and the page count */
static VOID
NTAPI
MiI386FlushTargetTb(
    _In_ PKIPI_CONTEXT PacketContext,
    _In_ PVOID Ignored,
    _In_ PVOID Address,
    _In_ PVOID Pages)
{
    UNREFERENCED_PARAMETER(Ignored);

    /* Flush before answering: the sender relies on the old translations being gone */
    MiI386FlushLocal((ULONG_PTR)Address, (ULONG)(ULONG_PTR)Pages);
    KiIpiSignalPacketDone(PacketContext);
}

/* Flushes this processor and, when Remote is set, the other active ones in it */
static VOID
MiI386FlushTargets(KAFFINITY Remote, ULONG_PTR Address, ULONG Pages)
{
    KIRQL OldIrql;
    KAFFINITY Active = KeActiveProcessors;

    /* Until a second processor is active there is nobody else to flush */
    if (Remote == 0 || (Active & (Active - 1)) == 0)
    {
        MiI386FlushLocal(Address, Pages);
        return;
    }

    /* The processor and so the remote set must not change under us */
    OldIrql = KeGetCurrentIrql();
    if (OldIrql < DISPATCH_LEVEL)
        KeRaiseIrql(DISPATCH_LEVEL, &OldIrql);

    Remote &= KeActiveProcessors & ~KeGetCurrentPrcb()->SetMember;
    if (Remote != 0)
    {
        ASSERT(KeGetCurrentIrql() < IPI_LEVEL);
        KiIpiSendPacket(Remote,
                        MiI386FlushTargetTb,
                        NULL,
                        Address,
                        (PULONG)(ULONG_PTR)Pages);
    }

    MiI386FlushLocal(Address, Pages);

    if (OldIrql < DISPATCH_LEVEL)
        KeLowerIrql(OldIrql);
}

static KAFFINITY
MiI386ScopeTargets(MI_TLB_SCOPE Scope)
{
    return (Scope == MiTlbAllProcessors) ? KeActiveProcessors : 0;
}

VOID
MiArchInvalidateTlbSingle(PVOID VirtualAddress, MI_TLB_SCOPE Scope)
{
    MiI386FlushTargets(MiI386ScopeTargets(Scope), (ULONG_PTR)VirtualAddress & ~(ULONG_PTR)(PAGE_SIZE - 1), 1);
}

VOID
MiArchInvalidateTlbRange(PVOID BaseAddress, SIZE_T Size, MI_TLB_SCOPE Scope)
{
    ULONG_PTR Address = (ULONG_PTR)BaseAddress & ~(ULONG_PTR)(PAGE_SIZE - 1);
    ULONG_PTR End;

    if (Size == 0)
        return;
    if (Size > (SIZE_T)-1 - (ULONG_PTR)BaseAddress || Size > MI_I386_TLB_RANGE_LIMIT * PAGE_SIZE)
    {
        MiArchInvalidateTlbAll(Scope);
        return;
    }
    End = ((ULONG_PTR)BaseAddress + Size - 1) & ~(ULONG_PTR)(PAGE_SIZE - 1);
    MiI386FlushTargets(MiI386ScopeTargets(Scope), Address, (ULONG)((End - Address) >> PAGE_SHIFT) + 1);
}

VOID
MiArchInvalidateTlbAll(MI_TLB_SCOPE Scope)
{
    MiI386FlushTargets(MiI386ScopeTargets(Scope), 0, 0);
}

VOID
MiArchTlbInvalidate(ULONG64 VirtualAddress, ULONG64 PageCount, BOOLEAN AllProcessors)
{
    MI_TLB_SCOPE Scope = AllProcessors ? MiTlbAllProcessors : MiTlbLocal;

    if (PageCount > ((SIZE_T)-1 >> PAGE_SHIFT))
        MiArchInvalidateTlbAll(Scope);
    else
        MiArchInvalidateTlbRange((PVOID)(ULONG_PTR)VirtualAddress,
                                 (SIZE_T)(PageCount << PAGE_SHIFT), Scope);
}

VOID
MiArchTlbInvalidateProcessors(ULONG64 VirtualAddress, ULONG64 PageCount, ULONG64 Processors)
{
    ULONG_PTR Address = (ULONG_PTR)VirtualAddress & ~(ULONG_PTR)(PAGE_SIZE - 1);

    if (PageCount == 0)
        return;
    if (PageCount > MI_I386_TLB_RANGE_LIMIT ||
        PageCount - 1 > ((~(ULONG_PTR)Address) >> PAGE_SHIFT))
        PageCount = 0;
    MiI386FlushTargets((KAFFINITY)Processors, Address, (ULONG)PageCount);
}

VOID
MiArchTlbInvalidateAll(BOOLEAN AllProcessors)
{
    MiArchInvalidateTlbAll(AllProcessors ? MiTlbAllProcessors : MiTlbLocal);
}
