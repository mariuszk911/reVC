#include "common.h"

#include "ParseFloat.h"
#include "main.h"
#include "FileMgr.h"
#include "Physical.h"
#include "HandlingMgr.h"

cHandlingDataMgr mod_HandlingManager;

const char *HandlingFilename = "HANDLING.CFG";

const char VehicleNames[NUMHANDLINGS][14] = {
	"LANDSTAL",
	"IDAHO",
	"STINGER",
	"LINERUN",
	"PEREN",
	"SENTINEL",
	"PATRIOT",
	"FIRETRUK",
	"TRASH",
	"STRETCH",
	"MANANA",
	"INFERNUS",
	"PONY",
	"MULE",
	"CHEETAH",
	"AMBULAN",
	"FBICAR",
	"MOONBEAM",
	"ESPERANT",
	"TAXI",
	"KURUMA",
	"BOBCAT",
	"MRWHOOP",
	"BFINJECT",
	"POLICE",
	"ENFORCER",
	"SECURICA",
	"BANSHEE",
	"BUS",
	"RHINO",
	"BARRACKS",
	"TRAIN",
	"HELI",
	"DODO",
	"COACH",
	"CABBIE",
	"STALLION",
	"RUMPO",
	"RCBANDIT",
	"MAFIA",
	"AIRTRAIN",
	"DEADDODO",
	"FLATBED",
	"YANKEE",
	"GOLFCART",
	"VOODOO",
	"WASHING",
	"CUBAN",
	"ROMERO",
	"PACKER",
	"ADMIRAL",
	"GANGBUR",
	"ZEBRA",
	"TOPFUN",
	"GLENDALE",
	"OCEANIC",
	"HERMES",
	"SABRE1",
	"SABRETUR",
	"PHEONIX",
	"WALTON",
	"REGINA",
	"COMET",
	"DELUXO",
	"BURRITO",
	"SPAND",
	"BAGGAGE",
	"KAUFMAN",
	"RANCHER",
	"FBIRANCH",
	"VIRGO",
	"GREENWOO",
	"HOTRING",
	"SANDKING",
	"BLISTAC",
	"BOXVILLE",
	"BENSON",
	"DESPERAD",
	"LOVEFIST",
	"BLOODRA",
	"BLOODRB",
	"BIKE",
	"MOPED",
	"DIRTBIKE",
	"ANGEL",
	"FREEWAY",
	"PREDATOR",
	"SPEEDER",
	"REEFER",
	"RIO",
	"SQUALO",
	"TROPIC",
	"COASTGRD",
	"DINGHY",
	"MARQUIS",
	"CUPBOAT",
	"SEAPLANE",
	"SPARROW",
	"SEASPAR",
	"MAVERICK",
	"COASTMAV",
	"POLMAV",
	"HUNTER",
	"RCBARON",
	"RCGOBLIN",
	"RCCOPTER"
};

cHandlingDataMgr::cHandlingDataMgr(void)
{
	memset(this, 0, sizeof(*this));
}

void
cHandlingDataMgr::Initialise(void)
{
	LoadHandlingData();
	field_0 = 0.1f;
	fWheelFriction = 0.9f;
	field_8 = 1.0f;
	field_C = 0.8f;
	field_10 = 0.98f;
}

void
cHandlingDataMgr::LoadHandlingData(void)
{
	char *start, *end;
	char line[201];	// weird value
	char delim[4];	// not sure
	char *word;
	int field, handlingId;
	int keepGoing;
	tHandlingData *handling;
	tFlyingHandlingData *flyingHandling;
	tBoatHandlingData *boatHandling;
	tBikeHandlingData *bikeHandling;

	CFileMgr::SetDir("DATA");
	CFileMgr::LoadFile(HandlingFilename, work_buff, sizeof(work_buff), "r");
	CFileMgr::SetDir("");

	start = (char*)work_buff;
	end = start+1;
	handling = nil;
	flyingHandling = nil;
	boatHandling = nil;
	bikeHandling = nil;
	keepGoing = 1;

	while(keepGoing){
		// find end of line
		while(*end != '\n') end++;

		// get line
		int32 lineLength = end - start;
#ifdef GTA_PS2
		// CRs are left untouched. we assume every line ends in one
		lineLength--;
#endif
		strncpy(line, start, lineLength);
		line[lineLength] = '\0';
		start = end+1;
		end = start+1;

		// yeah, this is kinda crappy
		if(strcmp(line, ";the end") == 0)
			keepGoing = 0;
		else if(line[0] != ';'){
			if(line[0] == '!'){
				// Bike data
				field = 0;
				strcpy(delim, " \t");
				// FIX: game seems to use a do-while loop here
				for(word = strtok(line, delim); word; word = strtok(nil, delim)){
					switch(field){
					case  0: break;
					case  1:
						handlingId = FindExactWord(word, (const char*)VehicleNames, 14, NUMHANDLINGS);
						assert(handlingId >= 0 && handlingId < NUMHANDLINGS);
						bikeHandling = GetBikePointer(handlingId);
						bikeHandling->nIdentifier = (tVehicleType)handlingId;
						break;
					case  2: bikeHandling->fLeanFwdCOM = rfAtof(word); break;
					case  3: bikeHandling->fLeanFwdForce = rfAtof(word); break;
					case  4: bikeHandling->fLeanBakCOM = rfAtof(word); break;
					case  5: bikeHandling->fLeanBackForce = rfAtof(word); break;
					case  6: bikeHandling->fMaxLean = rfAtof(word); break;
					case  7: bikeHandling->fFullAnimLean = rfAtof(word); break;
					case  8: bikeHandling->fDesLean = rfAtof(word); break;
					case  9: bikeHandling->fSpeedSteer = rfAtof(word); break;
					case 10: bikeHandling->fSlipSteer = rfAtof(word); break;
					case 11: bikeHandling->fNoPlayerCOMz = rfAtof(word); break;
					case 12: bikeHandling->fWheelieAng = rfAtof(word); break;
					case 13: bikeHandling->fStoppieAng = rfAtof(word); break;
					case 14: bikeHandling->fWheelieSteer = rfAtof(word); break;
					case 15: bikeHandling->fWheelieStabMult = rfAtof(word); break;
					case 16: bikeHandling->fStoppieStabMult = rfAtof(word); break;
					}
					field++;
				}
				ConvertBikeDataToGameUnits(bikeHandling);
			}else if(line[0] == '$'){
				// Flying data
				field = 0;
				strcpy(delim, " \t");
				// FIX: game seems to use a do-while loop here
				for(word = strtok(line, delim); word; word = strtok(nil, delim)){
					switch(field){
					case  0: break;
					case  1:
						handlingId = FindExactWord(word, (const char*)VehicleNames, 14, NUMHANDLINGS);
						assert(handlingId >= 0 && handlingId < NUMHANDLINGS);
						flyingHandling = GetFlyingPointer(handlingId);
						flyingHandling->nIdentifier = (tVehicleType)handlingId;
						break;
					case  2: flyingHandling->fThrust = rfAtof(word); break;
					case  3: flyingHandling->fThrustFallOff = rfAtof(word); break;
					case  4: flyingHandling->fYaw = rfAtof(word); break;
					case  5: flyingHandling->fYawStab = rfAtof(word); break;
					case  6: flyingHandling->fSideSlip = rfAtof(word); break;
					case  7: flyingHandling->fRoll = rfAtof(word); break;
					case  8: flyingHandling->fRollStab = rfAtof(word); break;
					case  9: flyingHandling->fPitch = rfAtof(word); break;
					case 10: flyingHandling->fPitchStab = rfAtof(word); break;
					case 11: flyingHandling->fFormLift = rfAtof(word); break;
					case 12: flyingHandling->fAttackLift = rfAtof(word); break;
					case 13: flyingHandling->fMoveRes = rfAtof(word); break;
					case 14: flyingHandling->vecTurnRes.x = rfAtof(word); break;
					case 15: flyingHandling->vecTurnRes.y = rfAtof(word); break;
					case 16: flyingHandling->vecTurnRes.z = rfAtof(word); break;
					case 17: flyingHandling->vecSpeedRes.x = rfAtof(word); break;
					case 18: flyingHandling->vecSpeedRes.y = rfAtof(word); break;
					case 19: flyingHandling->vecSpeedRes.z = rfAtof(word); break;
					}
					field++;
				}
			}else if(line[0] == '%'){
				// Boat data
				field = 0;
				strcpy(delim, " \t");
				// FIX: game seems to use a do-while loop here
				for(word = strtok(line, delim); word; word = strtok(nil, delim)){
					switch(field){
					case  0: break;
					case  1:
						handlingId = FindExactWord(word, (const char*)VehicleNames, 14, NUMHANDLINGS);
						assert(handlingId >= 0 && handlingId < NUMHANDLINGS);
						boatHandling = GetBoatPointer(handlingId);
						boatHandling->nIdentifier = (tVehicleType)handlingId;
						break;
					case  2: boatHandling->fThrustY = rfAtof(word); break;
					case  3: boatHandling->fThrustZ = rfAtof(word); break;
					case  4: boatHandling->fThrustAppZ = rfAtof(word); break;
					case  5: boatHandling->fAqPlaneForce = rfAtof(word); break;
					case  6: boatHandling->fAqPlaneLimit = rfAtof(word); break;
					case  7: boatHandling->fAqPlaneOffset = rfAtof(word); break;
					case  8: boatHandling->fWaveAudioMult = rfAtof(word); break;
					case  9: boatHandling->vecMoveRes.x = rfAtof(word); break;
					case 10: boatHandling->vecMoveRes.y = rfAtof(word); break;
					case 11: boatHandling->vecMoveRes.z = rfAtof(word); break;
					case 12: boatHandling->vecTurnRes.x = rfAtof(word); break;
					case 13: boatHandling->vecTurnRes.y = rfAtof(word); break;
					case 14: boatHandling->vecTurnRes.z = rfAtof(word); break;
					case 15: boatHandling->fLook_L_R_BehindCamHeight = rfAtof(word); break;
					}
					field++;
				}
			}else{
				field = 0;
				strcpy(delim, " \t");
				// FIX: game seems to use a do-while loop here
				for(word = strtok(line, delim); word; word = strtok(nil, delim)){
					switch(field){
					case  0:
						handlingId = FindExactWord(word, (const char*)VehicleNames, 14, NUMHANDLINGS);
						assert(handlingId >= 0 && handlingId < NUMHANDLINGS);
						handling = &HandlingData[handlingId];
						handling->nIdentifier = (tVehicleType)handlingId;
						break;
					case  1: handling->fMass = rfAtof(word); break;
					case  2: handling->Dimension.x = rfAtof(word); break;
					case  3: handling->Dimension.y = rfAtof(word); break;
					case  4: handling->Dimension.z = rfAtof(word); break;
					case  5: handling->CentreOfMass.x = rfAtof(word); break;
					case  6: handling->CentreOfMass.y = rfAtof(word); break;
					case  7: handling->CentreOfMass.z = rfAtof(word); break;
					case  8: handling->nPercentSubmerged = atoi(word); break;
					case  9: handling->fTractionMultiplier = rfAtof(word); break;
					case 10: handling->fTractionLoss = rfAtof(word); break;
					case 11: handling->fTractionBias = rfAtof(word); break;
					case 12: handling->Transmission.nNumberOfGears = atoi(word); break;
					case 13: handling->Transmission.fMaxVelocity = rfAtof(word); break;
					case 14: handling->Transmission.fEngineAcceleration = rfAtof(word) * 0.4; break;
					case 15: handling->Transmission.nDriveType = word[0]; break;
					case 16: handling->Transmission.nEngineType = word[0]; break;
					case 17: handling->fBrakeDeceleration = rfAtof(word); break;
					case 18: handling->fBrakeBias = rfAtof(word); break;
					case 19: handling->bABS = !!atoi(word); break;
					case 20: handling->fSteeringLock = rfAtof(word); break;
					case 21: handling->fSuspensionForceLevel = rfAtof(word); break;
					case 22: handling->fSuspensionDampingLevel = rfAtof(word); break;
					case 23: handling->fSeatOffsetDistance = rfAtof(word); break;
					case 24: handling->fCollisionDamageMultiplier = rfAtof(word); break;
					case 25: handling->nMonetaryValue = atoi(word); break;
					case 26: handling->fSuspensionUpperLimit = rfAtof(word); break;
					case 27: handling->fSuspensionLowerLimit = rfAtof(word); break;
					case 28: handling->fSuspensionBias = rfAtof(word); break;
					case 29: handling->fSuspensionAntidiveMultiplier = rfAtof(word); break;
					case 30:
						sscanf(word, "%x", &handling->Flags);
						handling->Transmission.Flags = handling->Flags;
						break;
					case 31: handling->FrontLights = atoi(word); break;
					case 32: handling->RearLights = atoi(word); break;
					}
					field++;
				}
				ConvertDataToGameUnits(handling);
			}
		}
	}
}

int
cHandlingDataMgr::FindExactWord(const char *word, const char *words, int wordLen, int numWords)
{
	int i;

	for(i = 0; i < numWords; i++){
		// BUG: the game does something really stupid here, it's fixed here
		if(strncmp(word, words, wordLen) == 0)
			return i;
		words += wordLen;
	}
	return numWords;
}


void
cHandlingDataMgr::ConvertDataToGameUnits(tHandlingData *handling)
{
	// convert distance to m, time to 1/50s
	float velocity, a, b;

	handling->Transmission.fEngineAcceleration *= 1.0f/(50.0f*50.0f);
	handling->Transmission.fMaxVelocity *= 1000.0f/(60.0f*60.0f * 50.0f);
	handling->fBrakeDeceleration *= 1.0f/(50.0f*50.0f);
	handling->fTurnMass = (sq(handling->Dimension.x) + sq(handling->Dimension.y)) * handling->fMass / 12.0f;
	if(handling->fTurnMass < 10.0f)
		handling->fTurnMass *= 5.0f;
	handling->fInvMass = 1.0f/handling->fMass;
	handling->fCollisionDamageMultiplier *= 2000.0f/handling->fMass;
	handling->fBuoyancy = 100.0f/handling->nPercentSubmerged * GRAVITY*handling->fMass;

	// Don't quite understand this. What seems to be going on is that
	// we calculate a drag (air resistance) deceleration for a given velocity and
	// find the intersection between that and the max engine acceleration.
	// at that point the car cannot accelerate any further and we've found the max velocity.
	a = 0.0f;
	b = 100.0f;
	velocity = handling->Transmission.fMaxVelocity;
	while(a < b && velocity > 0.0f){
		velocity -= 0.01f;
		// what's the 1/6?
		a = handling->Transmission.fEngineAcceleration/6.0f;
		// no density or drag coefficient here...
		float a_drag = 0.5f*SQR(velocity) * handling->Dimension.x*handling->Dimension.z / handling->fMass;
		// can't make sense of this... maybe  v - v/(drag + 1)  ? but that doesn't make so much sense either
		b = -velocity * (1.0f/(a_drag + 1.0f) - 1.0f);
	}

	if(handling->nIdentifier == HANDLING_RCBANDIT){
		handling->Transmission.fMaxCruiseVelocity = handling->Transmission.fMaxVelocity;
		handling->Transmission.fMaxReverseVelocity = -handling->Transmission.fMaxVelocity;
	}else if(handling->nIdentifier >= HANDLING_BIKE && handling->nIdentifier <= HANDLING_FREEWAY){
		handling->Transmission.fMaxCruiseVelocity = velocity;
		handling->Transmission.fMaxVelocity = velocity * 1.2f;
		handling->Transmission.fMaxReverseVelocity = -0.05f;
	}else{
		handling->Transmission.fMaxCruiseVelocity = velocity;
		handling->Transmission.fMaxVelocity = velocity * 1.2f;
		handling->Transmission.fMaxReverseVelocity = -0.2f;
	}

	if(handling->Transmission.nDriveType == '4')
		handling->Transmission.fEngineAcceleration /= 4.0f;
	else
		handling->Transmission.fEngineAcceleration /= 2.0f;

	handling->Transmission.InitGearRatios();
}

void
cHandlingDataMgr::ConvertBikeDataToGameUnits(tBikeHandlingData *handling)
{
	handling->fMaxLean = Sin(DEGTORAD(handling->fMaxLean));
	handling->fFullAnimLean = DEGTORAD(handling->fFullAnimLean);
	handling->fWheelieAng = Sin(DEGTORAD(handling->fWheelieAng));
	handling->fStoppieAng = Sin(DEGTORAD(handling->fStoppieAng));
}

int32
cHandlingDataMgr::GetHandlingId(const char *name)
{
	int i;
	for(i = 0; i < NUMHANDLINGS; i++)
		if(strncmp(VehicleNames[i], name, 14) == 0)
			break;
	return i;
}

tFlyingHandlingData*
cHandlingDataMgr::GetFlyingPointer(uint8 id)
{
	if(id >= HANDLING_SEAPLANE && id <= HANDLING_RCCOPTER)
		return &FlyingHandlingData[id-HANDLING_SEAPLANE];
	return &FlyingHandlingData[0];
}

tBoatHandlingData*
cHandlingDataMgr::GetBoatPointer(uint8 id)
{
	if(id >= HANDLING_PREDATOR && id <= HANDLING_SEAPLANE)
		return &BoatHandlingData[id-HANDLING_PREDATOR];
	return &BoatHandlingData[0];
}
