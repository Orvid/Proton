#include "APIC.h"
#include "Common.h"
#include "Console.h"
#include "CPUID.h"
#include "GDT.h"
#include "IDT.h"
#include "Log.h"
#include "Multiboot.h"
#include "PIC.h"
#include "PIT.h"
#include "RTC.h"
#include "SerialLogger.h"
#include "SystemClock.h"
#include "ThreadScheduler.h"

void CPUInterruptHandler(InterruptRegisters pRegisters)
{
	char buf[128];
	snprintf(buf, 128, "CPU Exception: %d", (int)pRegisters.int_no);
	Panic(buf);
}

void Startup()
{
	time_t startupTime = time(NULL);
	Log_WriteLine(LOGLEVEL__Information, "Nernel Started @ %24.24s", ctime(&startupTime));
	while(TRUE);
}

void Main(uint32_t pMultibootMagic, MultibootHeader* pMultibootHeader)
{
	Multiboot_Startup(pMultibootMagic, pMultibootHeader);
	SerialLogger_Startup();
	Console_Startup();
	GDT_Startup();
	IDT_Startup();
	for (uint8_t interrupt = 0; interrupt < IDT__IRQ__RemappedBase; ++interrupt) IDT_RegisterHandler(interrupt, &CPUInterruptHandler);
	PIC_Startup();
	PIT_Startup();
	PIC_StartInterrupts();
	RTC_Startup();
	APIC_Create(APIC__LocalMSR);
	SystemClock_Startup();
	//CPUID_Startup();
	ThreadScheduler_Startup((size_t)&Startup, 0x1000000);
	while(TRUE);
}

void Panic(const char* pMessage)
{
	//Console_Clear(CONSOLE_CREATEATTRIBUTES(CONSOLE_DARKBLACK, CONSOLE_LIGHTRED));
	Console_WriteLine(pMessage);
	Halt();
}
