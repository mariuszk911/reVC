#ifndef __GTA_C64INTRO_H__
#define __GTA_C64INTRO_H__

/*
 * Pure geometry, timing and screen-composition logic for the AmigaOS C64
 * "crunching" boot intro.
 *
 * Deliberately free of any game/platform dependency (no librw, no AmigaOS, no
 * <cstdio>) so the whole model can be exercised in the host unit test
 * (tests/amigaos/c64_intro_test.cpp). The AmigaOS renderer that consumes this
 * lives in src/skel/amigaos/amiga_c64_intro.cpp.
 *
 * The intro is a 40x25 character grid composed on the CPU (see Screen) and
 * uploaded as a 320x200 RGBA texture. Border raster bars, the drive LED and
 * the fade overlay are drawn as quads by the renderer.
 */

namespace c64intro {

const int kCols = 40;
const int kRows = 25;

// Screen-update pacing (~12 fps) and the on-screen timeline, milliseconds.
const unsigned kFrameMs       = 83;
const unsigned kTotalMs       = 4500;
const unsigned kProgressEndMs = 3750;
const unsigned kFadeStartMs   = 4250;
const unsigned kFadeMs        = 250;

// C64 PAL visible raster (384x272) and the 320x200 screen inset inside it.
const unsigned kScreenW = 320;
const unsigned kScreenH = 200;
const unsigned kRasterW = 384;
const unsigned kRasterH = 272;
const unsigned kBorderX = 32;   // (384 - 320) / 2
const unsigned kBorderY = 36;   // (272 - 200) / 2

// Typewriter speed and the moment the fake progress bar appears.
const unsigned kTypeMsPerChar = 20;
const unsigned kBarStartMs    = 800;

// Text timing, milliseconds.
const unsigned kNowLoadingMs = 250;
const unsigned kCreditMs     = 600;
const unsigned kCopyrightMs  = 800;
const unsigned kUnpackingMs  = 1000;
const unsigned kPleaseWaitMs = 1300;

// Grid rows used by the composition.
const int kRowNowLoading = 3;
const int kRowCredit     = 5;
const int kRowCopyright  = 6;
const int kRowUnpacking  = 8;
const int kRowPleaseWait = 9;
const int kRowBar        = 11;
const int kRowTrack      = 13;
const int kRowBuild      = 14;

// Bar geometry: '[' + 24 segments + ']' + ' ' + percentage, starting at col 8.
const int kBarCol      = 8;
const int kBarSegments = 24;

// Custom glyph codes (see src/skel/amigaos/c64_font.h).
const unsigned char kSpaceGlyph = 0x20;
const unsigned char kFullBlock  = 0x80;
const unsigned char kLightShade = 0x81;
const unsigned char kMediumShade = 0x82;
const unsigned char kDarkShade  = 0x83;
const unsigned char kLeftHalf   = 0x84;
const unsigned char kRightHalf  = 0x85;
const unsigned char kTopHalf    = 0x86;
const unsigned char kBottomHalf = 0x87;
const unsigned char kLastGlyph  = 0x87;

// C64 palette indices used by the composition.
const unsigned char kColWhite     = 1;
const unsigned char kColRed       = 2;
const unsigned char kColCyan      = 3;
const unsigned char kColPurple    = 4;
const unsigned char kColGreen     = 5;
const unsigned char kColBlue      = 6;
const unsigned char kColYellow    = 7;
const unsigned char kColOrange    = 8;
const unsigned char kColBrown     = 9;
const unsigned char kColLightRed  = 10;
const unsigned char kColDarkGrey  = 11;
const unsigned char kColGrey      = 12;
const unsigned char kColLightGreen= 13;
const unsigned char kColLightBlue = 14;
const unsigned char kColLightGrey = 15;

// Fixed strings. Keep every one of them <= kCols characters.
const char kTextNowLoading[] = "NOW LOADING...";
const char kTextCredit[]     = "* CRUNCHED WITH 'MIAMICRUNCH' *";
const char kTextCopyright[]  = "(C) 2026 REVC AMIGAOS";
const char kTextUnpacking[]  = "UNPACKING DATA...";
const char kTextPleaseWait[] = "PLEASE WAIT...";
const char kTextComplete[]   = "LOADING COMPLETE";
const char kTextReady[]      = "READY.";
const char kTextBuildPrefix[]= "PISTORM3D  BUILD ";

struct Rect { float x, y, w, h; };

// Aspect-preserving, screen-centred rect for `aspect` (w/h), fitted inside
// (screenW - 2*margin) by (screenH - 2*margin). Degenerate inputs (zero or
// negative screen/aspect, or a margin leaving no room) yield a zero rect.
inline Rect
FitAspect(float screenW, float screenH, float aspect, float margin)
{
	Rect r = { 0.0f, 0.0f, 0.0f, 0.0f };
	float availW, availH;

	if(screenW <= 0.0f || screenH <= 0.0f || aspect <= 0.0f)
		return r;
	availW = screenW - 2.0f * margin;
	availH = screenH - 2.0f * margin;
	if(availW <= 0.0f || availH <= 0.0f)
		return r;

	r.h = availH;
	r.w = availH * aspect;
	if(r.w > availW){
		r.w = availW;
		r.h = availW / aspect;
	}
	r.x = (screenW - r.w) * 0.5f;
	r.y = (screenH - r.h) * 0.5f;
	return r;
}

// The 320x200 screen inset inside a 384x272 outer rect: offsets 32/384 and
// 36/272, size 320/384 by 200/272 of the outer rect.
inline Rect
InnerScreen(const Rect &outer)
{
	Rect r;
	const float sx = (float)kScreenW / (float)kRasterW;
	const float sy = (float)kScreenH / (float)kRasterH;
	const float bx = (float)kBorderX / (float)kRasterW;
	const float by = (float)kBorderY / (float)kRasterH;

	r.x = outer.x + outer.w * bx;
	r.y = outer.y + outer.h * by;
	r.w = outer.w * sx;
	r.h = outer.h * sy;
	return r;
}

inline unsigned
FrameIndex(unsigned elapsedMs)
{
	return elapsedMs / kFrameMs;
}

// Monotonic piecewise-linear fake progress, 0..1000, from the key table in the
// plan. Clamped at both ends.
inline unsigned
ProgressAt(unsigned elapsedMs)
{
	static const unsigned keys[][2] = {
		{0, 0}, {180, 60}, {420, 60}, {650, 180}, {900, 180},
		{1150, 340}, {1500, 340}, {1800, 480}, {2050, 480},
		{2300, 620}, {2750, 690}, {3000, 690}, {3300, 860},
		{3600, 980}, {3750, 1000}
	};
	const unsigned n = (unsigned)(sizeof(keys) / sizeof(keys[0]));

	if(elapsedMs <= keys[0][0])
		return keys[0][1];
	for(unsigned i = 1; i < n; i++){
		if(elapsedMs <= keys[i][0]){
			unsigned t0 = keys[i - 1][0], t1 = keys[i][0];
			unsigned v0 = keys[i - 1][1], v1 = keys[i][1];
			if(t1 == t0)
				return v1;
			return v0 + ((v1 - v0) * (elapsedMs - t0)) / (t1 - t0);
		}
	}
	return keys[n - 1][1];
}

// Black-overlay alpha: 0 before kFadeStartMs, linear 0 -> 255 through the end
// of the intro, clamped.
inline unsigned
FadeAlpha(unsigned elapsedMs)
{
	if(elapsedMs <= kFadeStartMs)
		return 0u;
	if(elapsedMs >= kTotalMs)
		return 255u;
	return (255u * (elapsedMs - kFadeStartMs)) / kFadeMs;
}

// FNV-1a 32-bit; never zero (a zero xorshift seed would be stuck).
inline unsigned
HashSeed(const char *text)
{
	unsigned h = 2166136261u;

	if(text != 0){
		while(*text){
			h ^= (unsigned char)*text++;
			h *= 16777619u;
		}
	}
	return h ? h : 0x9E3779B9u;
}

// xorshift32.
inline unsigned
NextRandom(unsigned &state)
{
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

// Curated garbage field: uppercase, digits, punctuation and block glyphs.
// Never returns an index above kLastGlyph.
inline unsigned char
GarbageGlyph(unsigned r)
{
	static const unsigned char glyphs[] = {
		'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M',
		'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z',
		'0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
		'.', ',', '-', '*', '/', ':', '?', '!', '+', '=', '%', '#', '@',
		kFullBlock, kLightShade, kMediumShade, kDarkShade,
		kLeftHalf, kRightHalf, kTopHalf, kBottomHalf
	};
	const unsigned n = (unsigned)(sizeof(glyphs) / sizeof(glyphs[0]));
	return glyphs[r % n];
}

inline unsigned char
GarbageColour(unsigned r)
{
	static const unsigned char colours[] = {
		kColWhite, kColRed, kColCyan, kColPurple, kColGreen, kColYellow,
		kColOrange, kColLightRed, kColLightGreen, kColLightBlue, kColLightGrey
	};
	const unsigned n = (unsigned)(sizeof(colours) / sizeof(colours[0]));
	return colours[r % n];
}

struct Screen {
	unsigned char ch[kRows][kCols];
	unsigned char col[kRows][kCols];   // foreground palette index 0..15
};

struct State {
	unsigned rng;
	unsigned frame;
	char buildId[9];                   // uppercase, NUL-terminated
};

inline int
TextLength(const char *text)
{
	int n = 0;
	if(text != 0)
		while(text[n])
			n++;
	return n;
}

// All writes clamp to the 40x25 grid.
inline void
PutGlyph(Screen &scr, int row, int col, unsigned char glyph, unsigned char colour)
{
	if(row < 0 || row >= kRows || col < 0 || col >= kCols)
		return;
	scr.ch[row][col] = glyph;
	scr.col[row][col] = colour & 0x0F;
}

inline void
PutText(Screen &scr, int row, int col, const char *text, unsigned char colour)
{
	if(text == 0)
		return;
	for(int i = 0; text[i] != 0 && col + i < kCols; i++)
		PutGlyph(scr, row, col + i, (unsigned char)text[i], colour);
}

inline void
PutCentredText(Screen &scr, int row, const char *text, unsigned char colour)
{
	int len = TextLength(text);
	int col = (kCols - len) / 2;

	if(col < 0)
		col = 0;
	PutText(scr, row, col, text, colour);
}

// Typewriter line: the full width is reserved up front, characters before
// `visible` are shown and the rest stay spaces so the line does not shift.
inline void
PutTypedText(Screen &scr, int row, const char *text, unsigned elapsedMs,
             unsigned startMs, unsigned char colour)
{
	int len = TextLength(text);
	int col = (kCols - len) / 2;
	unsigned visible = 0;
	int shown;

	if(col < 0)
		col = 0;
	if(elapsedMs > startMs)
		visible = (elapsedMs - startMs) / kTypeMsPerChar;
	shown = (int)visible;
	if(shown > len)
		shown = len;
	for(int i = 0; i < len; i++)
		PutGlyph(scr, row, col + i,
		         i < shown ? (unsigned char)text[i] : kSpaceGlyph, colour);
}

inline unsigned
BarFilled(unsigned progress)
{
	if(progress > 1000)
		progress = 1000;
	return (progress * (unsigned)kBarSegments) / 1000;
}

inline unsigned
PercentAt(unsigned progress)
{
	if(progress > 1000)
		progress = 1000;
	return (progress * 100u) / 1000u;
}

inline void
DrawBar(Screen &scr, unsigned progress)
{
	unsigned filled = BarFilled(progress);
	unsigned pct = PercentAt(progress);
	int col = kBarCol;

	PutGlyph(scr, kRowBar, col++, (unsigned char)'[', kColLightGreen);
	for(int i = 0; i < kBarSegments; i++)
		PutGlyph(scr, kRowBar, col++,
		         (unsigned)i < filled ? kFullBlock : kLightShade, kColLightGreen);
	PutGlyph(scr, kRowBar, col++, (unsigned char)']', kColLightGreen);
	PutGlyph(scr, kRowBar, col++, kSpaceGlyph, kColLightGreen);

	if(pct >= 100){
		PutGlyph(scr, kRowBar, col++, (unsigned char)'1', kColLightGreen);
		PutGlyph(scr, kRowBar, col++, (unsigned char)'0', kColLightGreen);
		PutGlyph(scr, kRowBar, col++, (unsigned char)'0', kColLightGreen);
		PutGlyph(scr, kRowBar, col++, (unsigned char)'%', kColLightGreen);
	}else{
		PutGlyph(scr, kRowBar, col++, (unsigned char)('0' + pct / 10), kColLightGreen);
		PutGlyph(scr, kRowBar, col++, (unsigned char)('0' + pct % 10), kColLightGreen);
		PutGlyph(scr, kRowBar, col++, (unsigned char)'%', kColLightGreen);
	}
}

inline unsigned
TrackNumber(unsigned progress)
{
	unsigned track = 1 + progress / 125;
	return track > 99 ? 99 : track;
}

inline unsigned
SectorNumber(unsigned progress)
{
	return 1 + (progress / 25) % 18;
}

inline int
AppendString(char *dst, int pos, const char *text)
{
	if(text == 0)
		return pos;
	while(*text)
		dst[pos++] = *text++;
	return pos;
}

inline void
DrawTrackSector(Screen &scr, unsigned progress)
{
	unsigned track = TrackNumber(progress);
	unsigned sector = SectorNumber(progress);
	char line[40];
	int p = 0;

	p = AppendString(line, p, "TRACK ");
	line[p++] = (char)('0' + (track / 10) % 10);
	line[p++] = (char)('0' + track % 10);
	p = AppendString(line, p, "   SECTOR ");
	line[p++] = (char)('0' + (sector / 10) % 10);
	line[p++] = (char)('0' + sector % 10);
	line[p] = 0;
	PutCentredText(scr, kRowTrack, line, kColLightGrey);
}

inline void
DrawBuildLine(Screen &scr, const State &st)
{
	char line[40];
	int p = 0;

	p = AppendString(line, p, kTextBuildPrefix);
	for(int i = 0; i < 8; i++){
		char c = st.buildId[i];
		line[p++] = c ? c : ' ';
	}
	line[p] = 0;
	PutCentredText(scr, kRowBuild, line, kColGrey);
}

inline void
ClearCentre(Screen &scr)
{
	for(int row = 3; row <= 14; row++)
		for(int col = 0; col < kCols; col++){
			scr.ch[row][col] = kSpaceGlyph;
			scr.col[row][col] = kColBlue;
		}
}

inline void
ScrollGarbage(State &st, Screen &scr)
{
	for(int row = 0; row < kRows - 1; row++)
		for(int col = 0; col < kCols; col++){
			scr.ch[row][col] = scr.ch[row + 1][col];
			scr.col[row][col] = scr.col[row + 1][col];
		}
	for(int col = 0; col < kCols; col++){
		scr.ch[kRows - 1][col] = GarbageGlyph(NextRandom(st.rng));
		scr.col[kRows - 1][col] = GarbageColour(NextRandom(st.rng));
	}
}

// Seed the model and fill the initial garbage field. (The plan's API sketch
// omitted the Screen, but the field has to start somewhere; the renderer calls
// this once before the first Tick.)
inline void
Init(State &st, Screen &scr, unsigned seed, const char *buildId)
{
	const char *src = buildId;
	int n = 0;

	st.rng = seed ? seed : 0x9E3779B9u;
	st.frame = 0;
	for(int i = 0; i < 9; i++)
		st.buildId[i] = 0;

	if(src != 0){
		for(int i = 0; src[i] != 0 && n < 8; i++){
			char c = src[i];
			if(c >= 'a' && c <= 'z')
				c = (char)(c - 'a' + 'A');
			st.buildId[n++] = c;
		}
	}
	if(n == 0){
		st.buildId[0] = 'D';
		st.buildId[1] = 'E';
		st.buildId[2] = 'V';
		n = 3;
	}
	st.buildId[n] = 0;

	for(int row = 0; row < kRows; row++)
		for(int col = 0; col < kCols; col++){
			scr.ch[row][col] = GarbageGlyph(NextRandom(st.rng));
			scr.col[row][col] = GarbageColour(NextRandom(st.rng));
		}
}

// Rebuild the visible screen for `elapsedMs`. Callers tick only when the
// 12 fps frame index changes; the model itself is pure and deterministic.
inline void
Tick(State &st, Screen &scr, unsigned elapsedMs)
{
	unsigned progress;
	unsigned frame = FrameIndex(elapsedMs);

	st.frame = frame;

	// Garbage field: scroll every second frame, then mutate a few cells.
	if(frame % 2 == 0)
		ScrollGarbage(st, scr);
	for(int i = 0; i < 3; i++){
		int row = (int)(NextRandom(st.rng) % (unsigned)kRows);
		int col = (int)(NextRandom(st.rng) % (unsigned)kCols);
		PutGlyph(scr, row, col, GarbageGlyph(NextRandom(st.rng)),
		         GarbageColour(NextRandom(st.rng)));
	}

	// Centre text block is cleared every tick, so garbage only shows above and
	// below it and stale typewriter characters never survive a line change.
	ClearCentre(scr);

	PutTypedText(scr, kRowCredit, kTextCredit, elapsedMs, kCreditMs, kColYellow);
	PutTypedText(scr, kRowCopyright, kTextCopyright, elapsedMs, kCopyrightMs,
	             kColLightGrey);

	if(elapsedMs >= kProgressEndMs){
		progress = 1000;
		PutCentredText(scr, kRowUnpacking, kTextComplete, kColLightGreen);
		PutCentredText(scr, kRowPleaseWait, kTextReady, kColWhite);
		DrawBar(scr, 1000);
	}else{
		progress = ProgressAt(elapsedMs);
		PutTypedText(scr, kRowNowLoading, kTextNowLoading, elapsedMs,
		             kNowLoadingMs, kColWhite);
		PutTypedText(scr, kRowUnpacking, kTextUnpacking, elapsedMs,
		             kUnpackingMs, kColLightBlue);
		PutTypedText(scr, kRowPleaseWait, kTextPleaseWait, elapsedMs,
		             kPleaseWaitMs, kColWhite);
		if(elapsedMs >= kBarStartMs)
			DrawBar(scr, progress);
	}

	DrawTrackSector(scr, progress);
	DrawBuildLine(scr, st);
}

// Drive LED: on while the fake load is running, blinking every 250 ms.
inline bool
LedOn(unsigned elapsedMs)
{
	return ProgressAt(elapsedMs) < 1000 && ((elapsedMs / 250) % 2) == 0;
}

} // namespace c64intro

#endif // __GTA_C64INTRO_H__
