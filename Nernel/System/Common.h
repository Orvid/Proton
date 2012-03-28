#pragma once

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "Log.h"

#ifdef _WIN32
__declspec(noreturn) extern void Halt();
__declspec(noreturn) void Panic(const char* pMessage);
#else
extern void Halt() __attribute__((noreturn));
void Panic(const char* pMessage) __attribute__((noreturn));
#endif