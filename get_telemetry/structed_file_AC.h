#pragma once

#include <cstddef>


typedef int AC_STATUS;

#define AC_OFF 0
#define AC_REPLAY 1
#define AC_LIVE 2
#define AC_PAUSE 3

typedef int AC_SESSION_TYPE;

#define AC_UNKNOWN -1
#define AC_PRACTICE 0
#define AC_QUALIFY 1
#define AC_RACE 2
#define AC_HOTLAP 3
#define AC_TIME_ATTACK 4
#define AC_DRIFT 5
#define AC_DRAG 6


#pragma pack(push)
#pragma pack(4)

struct SPageFilePhysics
{
	int packetId = 0;
	float gas = 0;
	float brake = 0;
	float fuel = 0;
	int gear = 0;
	int rpms = 0;
	float steerAngle = 0;
	float speedKmh = 0;
	float velocity[3];
	float accG[3];
	float wheelSlip[4];
	float wheelLoad[4];
	float wheelsPressure[4];
	float wheelAngularSpeed[4];
	float tyreWear[4];
	float tyreDirtyLevel[4];
	float tyreCoreTemperature[4];
	float camberRAD[4];
	float suspensionTravel[4];
	float drs = 0;
	float tc = 0;
	float heading = 0;
	float pitch = 0;
	float roll = 0;
	float cgHeight;
	float carDamage[5];
	int numberOfTyresOut = 0;
	int pitLimiterOn = 0;
	float abs = 0;
};

// Prefix of Assetto Corsa's acpmf_static page.  Only the fields needed by the
// haptic bridge are declared; the real mapping continues after tyreRadius.
struct SPageFileStatic
{
	wchar_t smVersion[15];
	wchar_t acVersion[15];
	int numberOfSessions = 0;
	int numCars = 0;
	wchar_t carModel[33];
	wchar_t track[33];
	wchar_t playerName[33];
	wchar_t playerSurname[33];
	wchar_t playerNick[33];
	int sectorCount = 0;
	float maxTorque = 0;
	float maxPower = 0;
	int maxRpm = 0;
	float maxFuel = 0;
	float suspensionMaxTravel[4];
	float tyreRadius[4];
};

static_assert(offsetof(SPageFileStatic, suspensionMaxTravel) == 420, "Unexpected acpmf_static layout");

#pragma pack(pop)
