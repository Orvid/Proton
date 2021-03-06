#pragma once

#include "APIC.h"
#include "IDT.h"
#include "Thread.h"

void ThreadScheduler_Startup(size_t pEntryPoint, size_t pThreadStackSize);
void ThreadScheduler_Shutdown();

void ThreadScheduler_Add(Thread* pThread);
void ThreadScheduler_Remove(Thread* pThread);
void ThreadScheduler_Suspend(Thread* pThread);
void ThreadScheduler_Resume(Thread* pThread);

void ThreadScheduler_Timer(InterruptRegisters pRegisters);
void ThreadScheduler_Schedule(InterruptRegisters* pRegisters, APIC* pAPIC);

extern volatile bool_t gThreadScheduler_Running;
extern Process* gThreadScheduler_IdleProcess;
extern Process* gThreadScheduler_KernelProcess;
