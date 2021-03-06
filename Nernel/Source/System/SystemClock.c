#include <System/PIT.h>
#include <System/RTC.h>
#include <System/SystemClock.h>

uint32_t gSystemClock_StartupTime = 0;

void SystemClock_Startup()
{
    gSystemClock_StartupTime = RTC_GetSecondsSinceEpoch();
}

void SystemClock_Shutdown()
{
    gSystemClock_StartupTime = 0;
}

bool_t SystemClock_IsReady() { return gSystemClock_StartupTime != 0; }

uint32_t SystemClock_GetSecondsSinceEpoch() { return gSystemClock_StartupTime + gPIT_SecondsElapsed; }

uint16_t SystemClock_GetMilliseconds() { return gPIT_MillisecondsElapsed; }

uint64_t SystemClock_GetTicks() { return gPIT_MillisecondsElapsed + (gPIT_SecondsElapsed * 1000); }
