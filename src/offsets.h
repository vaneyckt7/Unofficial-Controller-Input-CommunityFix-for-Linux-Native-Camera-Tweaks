#pragma once

#include <stdint.h>


struct Addresses
{
	uint64_t CalculateCameraAngle_CallSite;
	uint64_t roll_movss;
	uint64_t CameraInputHandler;
};
struct Addresses* GetAddresses(void);

struct Sigs
{
	const char* CalculateCameraAngle_Callsite;
	const char* roll_movss;
	const char* CameraInputHandler;
	const char* CameraInputHandler_EventLoad;
};
struct Sigs* GetSigs(void);
