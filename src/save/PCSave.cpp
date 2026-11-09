#define WITHWINDOWS
#include "common.h"
#include "crossplatform.h"

#include "FileSwap.h"
#include "FileMgr.h"
#include "Font.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "GenericGameStorage.h"
#include "Messages.h"
#include "PCSave.h"
#include "Text.h"

// The save header is `{ int size; wchar FileName[24]; SYSTEMTIME; }` and the
// format is UCS-2: the engine's `wchar` is `typedef uint16_t wchar`
// (src/core/common.h), i.e. 24 x 2 bytes = 48, giving GenericGameStorage's
// hardcoded +0x40 field offset. C's wchar_t is 4 bytes on this toolchain and is
// NOT what the save format uses, so pin the engine typedef, not wchar_t.
static_assert(sizeof(wchar) == 2, "save-header name must be UCS-2 (2 bytes)");

const char* _psGetUserFilesFolder();

C_PcSave PcSaveHelper;

void
C_PcSave::SetSaveDirectory(const char *path)
{
#if defined ANDROID || defined __APPLE__ || defined AMIGAOS
	sprintf(DefaultPCSaveFileName, "%s/%s", path, "GTAVCsf");
#if defined ANDROID
    debug("SetSaveDirectory: %s", DefaultPCSaveFileName);
#endif
#else
    sprintf(DefaultPCSaveFileName, "%s\\%s", path, "GTAVCsf");
#endif
}

bool
C_PcSave::DeleteSlot(int32 slot)
{
#ifdef FIX_BUGS
	char FileName[MAX_PATH];
#else
	char FileName[200];
#endif

	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	sprintf(FileName, "%s%i.b", DefaultPCSaveFileName, slot + 1);
	DeleteFile(FileName);
	SlotSaveDate[slot][0] = '\0';
	return true;
}

int8
C_PcSave::SaveSlot(int32 slot)
{
	MakeValidSaveName(slot);
	PcSaveHelper.nErrorCode = SAVESTATUS_SUCCESSFUL;
	_psGetUserFilesFolder();
	int file = CFileMgr::OpenFile(ValidSaveName, "wb");
	if (file != 0) {
#ifdef MISSION_REPLAY
		if (!IsQuickSave)
#endif
			DoGameSpecificStuffBeforeSave();
		if (GenericSave(file)) {
			if (!!CFileMgr::CloseFile(file))
				nErrorCode = SAVESTATUS_ERR_SAVE_CLOSE;
			return 0;
		}

		return 2;
	}
	PcSaveHelper.nErrorCode = SAVESTATUS_ERR_SAVE_CREATE;
	return false;
}

bool
C_PcSave::PcClassSaveRoutine(int32 file, uint8 *data, uint32 size)
{
	// The block size is stored little-endian, like the field data.
	uint32 sizeLE = SAVE_LE32(size);
	CFileMgr::Write(file, (const char*)&sizeLE, sizeof(sizeLE));
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	CFileMgr::Write(file, (const char*)data, align4bytes(size));
	CheckSum += (uint8) size;
	CheckSum += (uint8) (size >> 8);
	CheckSum += (uint8) (size >> 16);
	CheckSum += (uint8) (size >> 24);
	for (int i = 0; i < align4bytes(size); i++) {
		CheckSum += *data++;
	}
	if (CFileMgr::GetErrorReadWrite(file)) {
		nErrorCode = SAVESTATUS_ERR_SAVE_WRITE;
		strncpy(SaveFileNameJustSaved, ValidSaveName, sizeof(ValidSaveName) - 1);
		return false;
	}

	return true;
}

void
C_PcSave::PopulateSlotInfo()
{
	for (int i = 0; i < SLOT_COUNT; i++) {
		Slots[i] = SLOT_EMPTY;
		SlotFileName[i][0] = '\0';
		SlotSaveDate[i][0] = '\0';
	}
	for (int i = 0; i < SLOT_COUNT; i++) {
#ifdef FIX_BUGS
		char savename[MAX_PATH];
#else
		char savename[52];
#endif
		struct {
			int size;
			wchar FileName[24];
			SYSTEMTIME SaveDateTime;
		} header;
		sprintf(savename, "%s%i%s", DefaultPCSaveFileName, i + 1, ".b");
		int file = CFileMgr::OpenFile(savename, "rb");
		if (file != 0) {
			CFileMgr::Read(file, (char*)&header, sizeof(header));
			// Match the raw on-disk bytes before any swap: the "empty slot"
			// sentinel is ASCII and must not be endian-converted first.
			if (strncmp((char*)&header, TopLineEmptyFile, sizeof(TopLineEmptyFile)-1) != 0) {
#ifdef BIGENDIAN
				// Header is stored little-endian: 24 x wchar name + a 16-byte
				// SYSTEMTIME (8 x uint16). Swap after the sentinel check, before
				// the name/date are used (a wrong date makes the month switch assert).
				Swap16Array((uint16_t*)header.FileName, sizeof(header.FileName)/sizeof(uint16_t));
				Swap16Array((uint16_t*)&header.SaveDateTime, sizeof(header.SaveDateTime)/sizeof(uint16_t));
#endif
				Slots[i] = SLOT_OK;
				memcpy(SlotFileName[i], &header.FileName, sizeof(header.FileName));
				
				SlotFileName[i][24] = '\0';
			}
			CFileMgr::CloseFile(file);
		}
		if (Slots[i] == SLOT_OK) {
			if (CheckDataNotCorrupt(i, savename)) {
				// D2: legacy/foreign save migration is bypassed on AmigaOS - the
				// Fix* byte-shufflers are endian-unsafe and FixSave renames the
				// file before rewriting it. Current-format saves validate above.
#if defined(FIX_INCOMPATIBLE_SAVES) && !defined(AMIGAOS)
				if (!FixSave(i, GetSaveType(savename))) {
					CMessages::InsertNumberInString(TheText.Get("FEC_SLC"), i + 1, -1, -1, -1, -1, -1, SlotFileName[i]);
					Slots[i] = SLOT_CORRUPTED;
					continue;
				}
#endif
				SYSTEMTIME st;
				memcpy(&st, &header.SaveDateTime, sizeof(SYSTEMTIME));
				const char *month;
				switch (st.wMonth)
				{
				case 1: month = "JAN"; break;
				case 2: month = "FEB"; break;
				case 3: month = "MAR"; break;
				case 4: month = "APR"; break;
				case 5: month = "MAY"; break;
				case 6: month = "JUN"; break;
				case 7: month = "JUL"; break;
				case 8: month = "AUG"; break;
				case 9: month = "SEP"; break;
				case 10: month = "OCT"; break;
				case 11: month = "NOV"; break;
				case 12: month = "DEC"; break;
				default: assert(0);
				}
				char date[70];
#ifdef MORE_LANGUAGES
				if (CGame::japaneseGame)
					sprintf(date, "%02d %02d %04d %02d:%02d:%02d", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
				else
#endif // MORE_LANGUAGES
					sprintf(date, "%02d %s %04d %02d:%02d:%02d", st.wDay, UnicodeToAsciiForSaveLoad(TheText.Get(month)), st.wYear, st.wHour, st.wMinute, st.wSecond);
				AsciiToUnicode(date, SlotSaveDate[i]);

			} else {
				CMessages::InsertNumberInString(TheText.Get("FEC_SLC"), i + 1, -1, -1, -1, -1, -1, SlotFileName[i]);
				Slots[i] = SLOT_CORRUPTED;
			}
		}
	}
}
