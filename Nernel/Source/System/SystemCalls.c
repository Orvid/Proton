#include <Common.h>
#include <System/APIC.h>
#include <System/Atomics.h>
#include <System/Console.h>
#include <System/GDT.h>
#include <System/Multiboot.h>
#include <System/SystemClock.h>

#include <reent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

struct _reent gDefaultReent;
bool_t gMultitasking = FALSE;	
uint8_t gMallocBusy = 0;

Thread* GetCurrentThread()
{
	if (!gMultitasking) return NULL;
	uint32_t taskRegister = TSS_GetTaskRegister();
	uint32_t apicIndex = (taskRegister - 0x2B) >> 3;
	APIC* apic = gAPIC_Array[apicIndex];
	return apic->CurrentThread;
}

struct _reent* __getreent()
{
	if (!gMultitasking) return &gDefaultReent;
	return &GetCurrentThread()->Reentrant;
}

void __malloc_lock(struct _reent* pReent)
{
	Atomic_CompareExchange(&gMallocBusy, 0, 1);
	Thread* thread = (Thread*)pReent->_new._reent._unused_rand;
	thread->MallocLockDepth++;
}

void __malloc_unlock(struct _reent* pReent)
{
	Thread* thread = (Thread*)pReent->_new._reent._unused_rand;
	thread->MallocLockDepth--;
	if (!thread->MallocLockDepth)
	{
		gMallocBusy = 0;
	}
}

long sysconf(int pSetting)
{
    if (pSetting == _SC_PAGE_SIZE) return 1;
    errno = EINVAL;
    return -1;
}

void* sbrk(ptrdiff_t pAdjustment)
{
	static uint8_t currentMemoryBlockIndex;
	MemoryBlock* memoryBlock = &gMemoryBlocks[currentMemoryBlockIndex];
	if (pAdjustment >= 0)
	{
        uint32_t adjustment = (uint32_t)pAdjustment;
		uint8_t originalIndex = currentMemoryBlockIndex;
		while ((memoryBlock->Size - memoryBlock->Used) < adjustment &&
               currentMemoryBlockIndex < gMemoryBlockCount)
		{
			++currentMemoryBlockIndex;
		    if (currentMemoryBlockIndex >= gMemoryBlockCount)
		    {
			    currentMemoryBlockIndex = originalIndex;
			    Panic("Insufficient memory for allocator break");
		    }
			memoryBlock = &gMemoryBlocks[currentMemoryBlockIndex];
		}
		memoryBlock->Used += adjustment;
		return (void*)(memoryBlock->Address + (memoryBlock->Used - adjustment));
	}
	memoryBlock->Used += pAdjustment;
	if (memoryBlock->Used == 0 && currentMemoryBlockIndex > 0) --currentMemoryBlockIndex;
    return NULL;
}

void _fini() { }

int open(const char* pPath, int pFlags, mode_t pMode)
{
    Panic("OPEN");
    errno = ENFILE;
    return -1;
}

int close(int pDescriptorIndex)
{
    Panic("CLOSE");
    errno = EBADF;
    return -1;
}

int fstat(int pDescriptorIndex, struct stat* pStats)
{
	char buf[256];
	snprintf(buf, 256, "FSTAT: %i\n", pDescriptorIndex);
    Panic(buf);
    errno = EBADF;
    return -1;
}

int stat(const char* pPath, struct stat* pStats)
{
    Panic("STAT");
    errno = EBADF;
    return -1;
}

int isatty(int pDescriptorIndex)
{
    Panic("ISATTY");
    errno = EBADF;
    return 0;
}

int write(int pDescriptorIndex, const void* pData, size_t pLength)
{
    if (pDescriptorIndex == STDOUT_FILENO ||
        pDescriptorIndex == STDERR_FILENO)
    {
        Console_WriteString((const char*)pData, pLength);
        return (int)pLength;
    }
    Panic("WRITE");
    errno = EBADF;
    return -1;
}

off_t lseek(int pDescriptorIndex, off_t pOffset, int pWhence)
{
    Panic("LSEEK");
    errno = EBADF;
    return -1;
}

int read(int pDescriptorIndex, void* pData, size_t pLength)
{
    Panic("READ");
    errno = EBADF;
    return -1;
}

int gettimeofday(struct timeval* pTime, void* pTimeZone)
{
	if (pTime)
	{
        pTime->tv_sec = 0;
        pTime->tv_usec = 0;
        if (SystemClock_IsReady())
        {
	        pTime->tv_sec = (time_t)SystemClock_GetSecondsSinceEpoch();
	        pTime->tv_usec = (suseconds_t)SystemClock_GetMilliseconds();
        }
	}
	if (pTimeZone)
	{
		struct timezone* tz = (struct timezone*)pTimeZone;
		tz->tz_minuteswest = 0;
		tz->tz_dsttime = 0;
	}
	return 0;
}

pid_t getpid()
{
	return 0;
}

int usleep(useconds_t pMicroseconds)
{
	if (pMicroseconds == 0) return 0;
	if (pMicroseconds >= 1000000)
	{
		errno = EINVAL;
		return -1;
	}

	uint32_t taskRegister = TSS_GetTaskRegister();
	uint32_t apicIndex = (taskRegister - 0x2B) >> 3;
	APIC* apic = gAPIC_Array[apicIndex];
	apic->CurrentThread->SleepRemaining = (*(size_t*)(apic->BaseAddress + APIC__Register__Timer__InitialCount) / (1000 / APIC__Timer__CycleHertz)) * (pMicroseconds / 1000);
	printf("Preempting Timer @ 0x%x of 0x%x, usleep for 0x%llx\n", (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount), (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__InitialCount), (unsigned long long)apic->CurrentThread->SleepRemaining);
	apic->PreemptedTimerCount = (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount);
	*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount) = 0;
	while (apic->CurrentThread->SleepRemaining) IOWAIT();
	printf("Done usleeping!\n");
	return 0;
}

unsigned int sleep(unsigned int pSeconds)
{
	if (pSeconds == 0) return 0;

	uint32_t taskRegister = TSS_GetTaskRegister();
	uint32_t apicIndex = (taskRegister - 0x2B) >> 3;
	APIC* apic = gAPIC_Array[apicIndex];
	apic->CurrentThread->SleepRemaining = (*(size_t*)(apic->BaseAddress + APIC__Register__Timer__InitialCount) / (1000 / APIC__Timer__CycleHertz)) * (pSeconds * 1000);
	printf("Preempting Timer @ 0x%x of 0x%x, sleep for 0x%llx\n", (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount), (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__InitialCount), (unsigned long long)apic->CurrentThread->SleepRemaining);
	apic->PreemptedTimerCount = (unsigned int)*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount);
	*(size_t*)(apic->BaseAddress + APIC__Register__Timer__CurrentCount) = 0;
	while (apic->CurrentThread->SleepRemaining) IOWAIT();
	printf("Done sleeping!\n");
	return 0;
}