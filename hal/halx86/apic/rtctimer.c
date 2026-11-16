/*
 * PROJECT:         ReactOS HAL
 * LICENSE:         GNU GPL - See COPYING in the top level directory
 * FILE:            hal/halx86/apic/rtctimer.c
 * PURPOSE:         HAL APIC Management and Control Code
 * PROGRAMMERS:     Timo Kreuzer (timo.kreuzer@reactos.org)
 * REFERENCES:      https://wiki.osdev.org/RTC
 *                  https://forum.osdev.org/viewtopic.php?f=13&t=20825&start=0
 *                  https://web.archive.org/web/20240119203005/http://www.bioscentral.com/misc/cmosmap.htm
 */

/* INCLUDES *******************************************************************/

#include <hal.h>
#include "apicp.h"
#include <smp.h>
#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

static const UCHAR RtcMinimumClockRate = 6;  /* Minimum rate  6: 1024 Hz / 0.97 ms */
static const UCHAR RtcMaximumClockRate = 10; /* Maximum rate 10: 64 Hz / 15.6 ms */
static UCHAR HalpCurrentClockRate = 10;  /* Initial rate  10: 64 Hz / 15.6 ms */
static ULONG HalpCurrentTimeIncrement;
static ULONG HalpMinimumTimeIncrement;
static ULONG HalpMaximumTimeIncrement;
static ULONG HalpCurrentFractionalIncrement;
static ULONG HalpRunningFraction;
static BOOLEAN HalpSetClockRate;
static UCHAR HalpNextClockRate;
static ULONG HalpNextTimeIncrement;

static LONG HalpClockIpiTickOffset;

BOOLEAN HalpApicTimerClock = FALSE;
static ULONG64 HalpApicTimerFrequency;

extern LARGE_INTEGER HalpCpuClockFrequency;
extern BOOLEAN HalIsProfiling;
extern BOOLEAN HalpUsePlatformTick;

static BOOLEAN HalpClockTscUsable;
static BOOLEAN HalpIsClockTscUsable(VOID);

/*
 * Hyper-V's synthetic timer 0 in direct mode: a periodic timer the hypervisor
 * keeps itself and delivers as an ordinary interrupt on a vector of the
 * guest's choosing. It is the platform clock there, in place of the RTC: the
 * RTC is a device of the virtual machine's worker process, so every tick cost
 * two port accesses answered in user mode on the host, about 50 us each with
 * the processor held meanwhile, a thousand times a second, and far longer
 * whenever the host was busy.
 */
#define HV_X64_MSR_GUEST_OS_ID          0x40000000
#define HV_X64_MSR_STIMER0_CONFIG       0x400000B0
#define HV_X64_MSR_STIMER0_COUNT        0x400000B1
#define HV_GUEST_OS_ID_LIBERNT          0x8000000A000065F4ULL
#define HV_ACCESS_SYNTHETIC_TIMERS      0x00000008 /* CPUID 0x40000003 EAX */
#define HV_STIMER_DIRECT_MODE_AVAILABLE 0x00080000 /* CPUID 0x40000003 EDX */
#define HV_STIMER_ENABLE                0x00000001
#define HV_STIMER_PERIODIC              0x00000002
#define HV_STIMER_DIRECT_MODE           0x00001000
#define HV_STIMER_VECTOR_SHIFT          4

static BOOLEAN HalpHvTimerClock;

static
BOOLEAN
HalpCanUseHvTimerClock(VOID)
{
    INT CpuInfo[4];
    ULONG64 Frequency;

    /* The same interface that states the frequencies; the clock is paced by the counter it describes */
    if (!HalpReadHypervisorFrequency(HV_X64_MSR_TSC_FREQUENCY, &Frequency))
        return FALSE;

    __cpuid(CpuInfo, 0x40000003);
    return (CpuInfo[0] & HV_ACCESS_SYNTHETIC_TIMERS) &&
           (CpuInfo[3] & HV_STIMER_DIRECT_MODE_AVAILABLE);
}

/* The timer takes any period, in the kernel's own unit of 100 ns */
static
VOID
HalpSetHvTimerIncrement(ULONG Increment)
{
    HalpCurrentTimeIncrement = Increment;
    HalpCurrentFractionalIncrement = 0;

    /* The synthetic registers answer only to a guest that has said what it is */
    if (__readmsr(HV_X64_MSR_GUEST_OS_ID) == 0)
        __writemsr(HV_X64_MSR_GUEST_OS_ID, HV_GUEST_OS_ID_LIBERNT);

    __writemsr(HV_X64_MSR_STIMER0_CONFIG, 0);
    __writemsr(HV_X64_MSR_STIMER0_COUNT, Increment);
    __writemsr(HV_X64_MSR_STIMER0_CONFIG,
               HV_STIMER_ENABLE | HV_STIMER_PERIODIC | HV_STIMER_DIRECT_MODE |
               ((ULONG64)APIC_CLOCK_VECTOR << HV_STIMER_VECTOR_SHIFT));
}

static
BOOLEAN
HalpCanUseApicTimerClock(VOID)
{
    INT CpuInfo[4];

    __cpuid(CpuInfo, 1);

    if (CpuInfo[2] & (1UL << 31))
        return TRUE;

    __cpuid(CpuInfo, 0);
    if (CpuInfo[0] < 6)
        return FALSE;
    __cpuid(CpuInfo, 6);
    return (CpuInfo[0] & (1UL << 2)) != 0;
}

static
VOID
HalpCalibrateApicTimer(VOID)
{
    LVT_REGISTER LvtEntry;
    ULONG64 Start, Ticks;
    ULONG Elapsed;

    /* The timer runs with divide-by-1, so the reported frequency is its rate. */
    if (HalpReadHypervisorFrequency(HV_X64_MSR_APIC_FREQUENCY, &HalpApicTimerFrequency))
    {
        DPRINT1("Local APIC timer clock from the hypervisor: %I64u Hz\n", HalpApicTimerFrequency);
        return;
    }

    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_CLOCK_VECTOR;
    LvtEntry.Mask = 1;
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    ApicWrite(APIC_TICR, MAXULONG);

    Ticks = HalpCpuClockFrequency.QuadPart / 100;
    Start = __rdtsc();
    while (__rdtsc() - Start < Ticks)
        YieldProcessor();
    Elapsed = MAXULONG - ApicRead(APIC_TCCR);

    ApicWrite(APIC_TICR, 0);
    HalpApicTimerFrequency = (ULONG64)Elapsed * 100;
    DPRINT1("Local APIC timer clock: %I64u Hz\n", HalpApicTimerFrequency);
}

/*!
    \brief Converts the CMOS RTC rate into the time increment in 0.1ns intervals.

    Rate Frequency Interval (ms) Precise increment (0.1ns)
    ------------------------------------------------------
     0   disabled
     1   32768      0.03052            305,175
     2   16384      0.06103            610,351
     3    8192      0.12207          1,220,703
     4    4096      0.24414          2,441,406
     5    2048      0.48828          4,882,812
     6    1024      0.97656          9,765,625 <- minimum
     7     512      1.95313         19,531,250
     8     256      3.90625         39,062,500
     9     128      7.8125          78,125,000
    10      64     15.6250         156,250,000 <- maximum / default
    11      32     31.25           312,500,000
    12      16     62.5            625,000,000
    13       8    125            1,250,000,000
    14       4    250            2,500,000,000
    15       2    500            5,000,000,000

*/
FORCEINLINE
ULONG
RtcClockRateToPreciseIncrement(UCHAR Rate)
{
    /* Calculate frequency */
    ULONG Frequency = 32768 >> (Rate - 1);

    /* Calculate interval in 0.1ns interval: Interval = (1 / Frequency) * 10,000,000,000 */
    return 10000000000ULL / Frequency;
}

static
VOID
HalpProgramApicTimerClock(UCHAR ClockRate)
{
    LVT_REGISTER LvtEntry;
    ULONG64 Count;

    Count = HalpApicTimerFrequency * RtcClockRateToPreciseIncrement(ClockRate) / 10000000000ULL;

    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_CLOCK_VECTOR;
    LvtEntry.TimerMode = 1;
    LvtEntry.Mask = 0;
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TICR, (ULONG)max(Count, 1));
}

/* The local APIC timer runs at any period, so it is given the exact increment asked for */
static
VOID
HalpSetApicTimerIncrement(ULONG Increment)
{
    LVT_REGISTER LvtEntry;
    ULONG64 Count;

    HalpCurrentTimeIncrement = Increment;
    HalpCurrentFractionalIncrement = 0;

    Count = HalpApicTimerFrequency * Increment / 10000000ULL;

    LvtEntry.Long = 0;
    LvtEntry.Vector = APIC_CLOCK_VECTOR;
    LvtEntry.TimerMode = 1;
    LvtEntry.Mask = 0;
    ApicWrite(APIC_TDCR, TIMER_DV_DivideBy1);
    ApicWrite(APIC_TMRLVTR, LvtEntry.Long);
    ApicWrite(APIC_TICR, (ULONG)max(Count, 1));
}

VOID
RtcSetClockRate(UCHAR ClockRate)
{
    UCHAR RegisterA;
    ULONG PreciseIncrement;

    /* Update the global values */
    HalpCurrentClockRate = ClockRate;
    PreciseIncrement = RtcClockRateToPreciseIncrement(ClockRate);
    HalpCurrentTimeIncrement = PreciseIncrement / 1000;
    HalpCurrentFractionalIncrement = PreciseIncrement % 1000;

    if (HalpApicTimerClock)
    {
        HalpProgramApicTimerClock(ClockRate);
        return;
    }

    /* Acquire CMOS lock */
    HalpAcquireCmosSpinLock();

    // TODO: disable NMI

    /* Read value of register A */
    RegisterA = HalpReadCmos(RTC_REGISTER_A);

    /* Change lower 4 bits to new rate */
    RegisterA &= 0xF0;
    RegisterA |= ClockRate;

    /* Write the new value */
    HalpWriteCmos(RTC_REGISTER_A, RegisterA);

    /* Release CMOS lock */
    HalpReleaseCmosSpinLock();
}

CODE_SEG("INIT")
VOID
NTAPI
HalpInitializeClock(VOID)
{
    ULONG_PTR EFlags;
    UCHAR RegisterB;

    /* Save EFlags and disable interrupts */
    EFlags = __readeflags();
    _disable();

    HalpClockTscUsable = HalpIsClockTscUsable();

    /* Asked to keep the local APIC timer free, a Hyper-V guest takes the hypervisor's own timer rather than the RTC */
    if (HalpUsePlatformTick && HalpCanUseHvTimerClock())
    {
        HalpHvTimerClock = TRUE;

        HalpMinimumTimeIncrement = 10000;
        HalpMaximumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMaximumClockRate) / 1000;
        KeSetTimeIncrement(HalpMaximumTimeIncrement, HalpMinimumTimeIncrement);

        HalpSetHvTimerIncrement(HalpMaximumTimeIncrement);

        __writeeflags(EFlags);
        DPRINT1("Clock initialized (Hyper-V synthetic timer)\n");
        return;
    }

    /* The RTC drives the clock where the local APIC timer cannot, or where the boot options keep it free */
    if (!HalpUsePlatformTick && HalpCanUseApicTimerClock())
    {
        HalpApicTimerClock = TRUE;
        KeSetProfileIrql(max(PROFILE_LEVEL, CLOCK_LEVEL));
        HalpCalibrateApicTimer();

        RtcSetClockRate(HalpCurrentClockRate);

        /* The finest period is a millisecond, which the timer gives exactly */
        HalpMinimumTimeIncrement = 10000;
        HalpMaximumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMaximumClockRate) / 1000;
        KeSetTimeIncrement(HalpMaximumTimeIncrement, HalpMinimumTimeIncrement);

        __writeeflags(EFlags);
        DPRINT1("Clock initialized (local APIC timer)\n");
        return;
    }

    // TODO: disable NMI

    /* Set initial rate */
    RtcSetClockRate(HalpCurrentClockRate);

    /* Calculate minumum and maximum increment */
    HalpMinimumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMinimumClockRate) / 1000;
    HalpMaximumTimeIncrement = RtcClockRateToPreciseIncrement(RtcMaximumClockRate) / 1000;

    /* Notify the kernel about the maximum and minimum increment */
    KeSetTimeIncrement(HalpMaximumTimeIncrement, HalpMinimumTimeIncrement);

    /* Enable the timer interrupt */
    HalEnableSystemInterrupt(APIC_CLOCK_VECTOR, CLOCK_LEVEL, Latched);

    /*
     * Firmware can leave an RTC interrupt pending, and calibrating the TSC
     * against the PM timer does not acknowledge it. Clear register C after
     * unmasking the IOAPIC route so the next periodic interrupt produces a
     * fresh edge. Keep CPU interrupts disabled until the RTC is rearmed.
     */
    HalpAcquireCmosSpinLock();

    /*
     * The periodic interrupt is enabled last, once the rate is set, and by
     * turning it off and on: the order the RTC is programmed in. Hyper-V's
     * RTC starts its periodic timer on that transition only, and enabled
     * before the rate was written it never raised an interrupt.
     */
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB & ~RTC_REG_B_PI);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);

    HalpReadCmos(RTC_REGISTER_C);
    HalpReleaseCmosSpinLock();

    /* Restore interrupt state */
    __writeeflags(EFlags);

    DPRINT1("Clock initialized\n");
}

VOID
HalpRestoreClock(VOID)
{
    UCHAR RegisterB;

    if (HalpApicTimerClock)
    {
        HalpSetApicTimerIncrement(HalpCurrentTimeIncrement);
        return;
    }

    if (HalpHvTimerClock)
    {
        HalpSetHvTimerIncrement(HalpCurrentTimeIncrement);
        return;
    }

    /* As at boot: the rate first, then the periodic interrupt turned off and on */
    RtcSetClockRate(HalpCurrentClockRate);

    HalpAcquireCmosSpinLock();
    RegisterB = HalpReadCmos(RTC_REGISTER_B);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB & ~RTC_REG_B_PI);
    HalpWriteCmos(RTC_REGISTER_B, RegisterB | RTC_REG_B_PI);
    HalpReadCmos(RTC_REGISTER_C);
    HalpReleaseCmosSpinLock();
}

/*
 * A periodic interrupt that is not taken before the next one is due is lost:
 * neither the RTC nor the local APIC timer queues them. A virtual processor
 * the host does not run for a few milliseconds loses them by the dozen, and
 * a hypervisor may then send some of them late, in a burst, or not at all
 * (Hyper-V's RTC delivered 530 to 660 of 1024 a second on a busy host). The
 * time of day, which counted interrupts, fell behind by seconds a minute.
 *
 * Where a hypervisor states the time stamp counter's frequency, the counter
 * says how much time really passed. The interrupts still set the pace; the
 * counter decides how many periods each one is worth: two or more after a
 * gap, none for one that comes early because the gap before it was already
 * paid for. What was handed to the kernel and what the counter measured are
 * kept in step to within half a period.
 *
 * Without a stated frequency the counter's rate is only as good as a 50 ms
 * calibration, worse than the RTC's crystal, and the interrupts are counted
 * as before.
 */
static ULONG64 HalpClockLastTsc;

/* Counter ticks that passed and have not been handed to the kernel yet (negative: handed in advance) */
static LONG64 HalpClockDebt;

static
BOOLEAN
HalpIsClockTscUsable(VOID)
{
    ULONG64 Frequency;

    return HalpReadHypervisorFrequency(HV_X64_MSR_TSC_FREQUENCY, &Frequency);
}

/* Answers the time this clock interrupt stands for: the period, a multiple of it, or nothing */
static
ULONG
HalpRecoverLostClockTicks(
    _In_ ULONG Increment)
{
    ULONG64 Tsc, Elapsed;
    LONG64 Period, Extra;

    if (!HalpClockTscUsable || Increment == 0)
        return Increment;

    Tsc = __rdtsc();
    Elapsed = Tsc - HalpClockLastTsc;
    HalpClockLastTsc = Tsc;

    /* The first interrupt has nothing to be compared with */
    if (Elapsed == Tsc)
        return Increment;

    Period = (LONG64)((ULONG64)HalpCpuClockFrequency.QuadPart * Increment / 10000000);
    if (Period == 0)
        return Increment;

    HalpClockDebt += (LONG64)Elapsed - Period;

    /* More than a second out is a machine that was stopped (saved, paused, in the debugger), not lost ticks */
    if (HalpClockDebt > HalpCpuClockFrequency.QuadPart || HalpClockDebt < -HalpCpuClockFrequency.QuadPart)
    {
        HalpClockDebt = 0;
        return Increment;
    }

    if (HalpClockDebt >= Period / 2)
    {
        Extra = (HalpClockDebt + Period / 2) / Period;
        HalpClockDebt -= Extra * Period;
        return (ULONG)((1 + Extra) * Increment);
    }

    if (HalpClockDebt <= -(Period / 2))
    {
        HalpClockDebt += Period;
        return 0;
    }

    return Increment;
}

VOID
FASTCALL
HalpClockInterruptHandler(IN PKTRAP_FRAME TrapFrame)
{
    ULONG LastIncrement;
    KIRQL Irql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);
#ifdef _M_AMD64
    /* This is for debugging */
    TrapFrame->ErrorCode = 0xc10c4;
#endif

    /* Start the interrupt */
    if (!HalBeginSystemInterrupt(CLOCK_LEVEL, APIC_CLOCK_VECTOR, &Irql))
    {
        /* Spurious, just end the interrupt */
#ifdef _M_IX86
        KiEoiHelper(TrapFrame);
#endif
        return;
    }

    /* Read register C, so that the next interrupt can happen */
    if (!HalpApicTimerClock && !HalpHvTimerClock)
    {
        HalpAcquireCmosSpinLock();
        HalpReadCmos(RTC_REGISTER_C);
        HalpReleaseCmosSpinLock();
    }

    /* Save increment */
    LastIncrement = HalpCurrentTimeIncrement;

    /* Check if the running fraction has accounted for 100 ns */
    HalpRunningFraction += HalpCurrentFractionalIncrement;
    if (HalpRunningFraction >= 1000)
    {
        LastIncrement++;
        HalpRunningFraction -= 1000;
    }

    /* What this interrupt is worth: more after a gap, nothing when it comes early */
    LastIncrement = HalpRecoverLostClockTicks(LastIncrement);

    /* Check if someone changed the time rate */
    if (HalpSetClockRate)
    {
        /* Set new clock rate */
        if (HalpApicTimerClock)
            HalpSetApicTimerIncrement(HalpNextTimeIncrement);
        else if (HalpHvTimerClock)
            HalpSetHvTimerIncrement(HalpNextTimeIncrement);
        else
            RtcSetClockRate(HalpNextClockRate);

        /* We're done */
        HalpSetClockRate = FALSE;
    }

    /* Update AP runtime counters at the same cadence as the boot CPU. */
    HalpClockIpiTickOffset += (LONG)LastIncrement;
    if (HalpClockIpiTickOffset >= (LONG)HalpMaximumTimeIncrement)
    {
        HalpClockIpiTickOffset %= (LONG)HalpMaximumTimeIncrement;
        HalpBroadcastClockIpi(CLOCK_IPI_VECTOR);
    }

    if (HalpApicTimerClock && HalIsProfiling)
        KeProfileInterruptWithSource(TrapFrame, ProfileTime);

    /* Update the system time -- on x86 the kernel will exit this trap  */
    if (LastIncrement != 0)
        KeUpdateSystemTime(TrapFrame, LastIncrement, Irql);

    /* End the interrupt */
    KiEndInterrupt(Irql, TrapFrame);
}

VOID
FASTCALL
HalpClockIpiHandler(IN PKTRAP_FRAME TrapFrame)
{
    KIRQL Irql;

    /* Enter trap */
    KiEnterInterruptTrap(TrapFrame);
#ifdef _M_AMD64
    /* This is for debugging */
    TrapFrame->ErrorCode = 0xc10c4;
#endif

    /* Start the interrupt */
    if (!HalBeginSystemInterrupt(CLOCK_LEVEL, CLOCK_IPI_VECTOR, &Irql))
    {
        /* Spurious, just end the interrupt */
#ifdef _M_IX86
        KiEoiHelper(TrapFrame);
#endif
        return;
    }

    if (HalpApicTimerClock && HalIsProfiling)
        KeProfileInterruptWithSource(TrapFrame, ProfileTime);

    /* Call the kernel to update runtimes */
    KeUpdateRunTime(TrapFrame, Irql);

    /* End the interrupt */
    KiEndInterrupt(Irql, TrapFrame);
}

ULONG
NTAPI
HalSetTimeIncrement(IN ULONG Increment)
{
    UCHAR Rate;
    ULONG NextIncrement;

    if (HalpApicTimerClock || HalpHvTimerClock)
    {
        Increment = min(max(Increment, HalpMinimumTimeIncrement), HalpMaximumTimeIncrement);
        HalpNextTimeIncrement = Increment;
        HalpSetClockRate = TRUE;
        return Increment;
    }

    /* Lookup largest value below given Increment */
    for (Rate = RtcMinimumClockRate; Rate < RtcMaximumClockRate; Rate++)
    {
        /* Check if this is the largest rate possible */
        NextIncrement = RtcClockRateToPreciseIncrement(Rate + 1) / 1000;
        if (NextIncrement > Increment)
            break;
    }

    /* Set the rate and tell HAL we want to change it */
    HalpNextClockRate = Rate;
    HalpSetClockRate = TRUE;

    /* Return the real increment */
    return RtcClockRateToPreciseIncrement(Rate) / 1000;
}
