/*
 * nulldraw_test - glDrawElements frame benchmark for PiStorm3D minigl (29.x).
 *
 * Measures a whole frame through glDrawElements on interleaved client arrays in
 * three submission modes, to compare the unlocked per-index gather with the
 * DrawLockedTriangles fast path:
 *
 *   unlocked   no lock; plain glDrawElements per draw (per-index gather)
 *   modellock  one glLockArrays(modelBase, modelVerts) per model, then all of the
 *              model's draws. The lock spans >1024 vertices, so on 29.1 it
 *              declines and costs like unlocked.
 *   perdraw    glLockArrays(base, verts) per draw; verts <= 1024 so the fast path
 *              engages (the app-side workaround)
 *
 * The frame is shaped after the logs: 500 draws, ~350k indices, ~700 indices/draw
 * (17x588 / 2x1008 / 1x1998 per 20 draws), interleaved pos(f3)+col(ub4)+uv(f2),
 * a texture bound per draw. Each draw is a triangulated WxH grid inside a 32x32
 * cell of the 640x480 frame, so the frame really covers the screen (about 1.7x
 * overdraw) and the coverage check below can catch dropped triangles.
 *
 * Build (adjust the SDK paths):
 *   m68k-amigaos-gcc -m68040 -mhard-float -mcrt=nix20 -O2 \
 *     -I<minigl>/include nulldraw_test.c -L<minigl>/lib -lminigl -lm -o nulldraw_test
 *
 * Run with minigl.library reachable (same drawer or LIBS:). Writes
 * nulldraw_test.log and prints ms/frame, us/draw and ns/index per mode.
 *
 * Each measured frame is submitted with mglSwitchDisplay(): glFinish only waits
 * on an outstanding render, it does not submit, so timing a loop without the
 * switch accumulates everything into one control list and measures packing, not
 * a frame. A cross-mode coverage check at the end samples the framebuffer and
 * requires the three modes to produce the same pixels (they change submission
 * only, not output).
 *
 * CONTEXT (why the wide-lock path matters to reVC): the game locks a whole
 * geometry (an atomic) with glLockArrays and draws its material ranges under that
 * lock. Its array path admits up to 16384 vertices, so those locks routinely
 * exceed MGL_MAXVERTS (1024) -- while the other callers (glquake 483, h2 304,
 * ioq3 <=1000) stay under it. In-game, enabling the whole-geometry lock makes the
 * player character and parts of the world disappear; with locks off it renders
 * fully. The coverage check below reproduces that loss in isolation.
 *
 * NOTES (known issues this test can expose):
 *
 * - Coverage mismatch. The three modes change submission only, so the coverage
 *   line must be identical; a WARNING means the locked path dropped triangles.
 *   On 29.1 + the chunked-lock change both locked modes lost ~6% of the sampled
 *   frame (unlocked 768, modellock 723, perdraw 723), and perdraw locks span only
 *   a few hundred vertices, so the loss is not confined to the >MGL_MAXVERTS
 *   chunk boundary.
 *
 * - Non-zero lock base. Except for the first one, every lock here (and every
 *   per-draw window in reVC) starts at a non-zero first vertex, where the other
 *   games lock from 0. Most of the dropped coverage comes from those locks, so a
 *   lock with first != 0 is worth testing on its own.
 *
 * - Dropped frames. The driver's mid-sequence state-buffer regrow guard can drop
 *   the whole frame being built; a dropped frame is never submitted and times as
 *   the fastest one there is, so a warm-up runs before any measurement. The
 *   counters (g_mglv3d_state_buf_grows / g_mglv3d_frames_dropped) come with the
 *   archive build; they are not importable from the shared library (data
 *   symbols), so this test cannot read them.
 */

#define TimerBase NullDrawTimerBase	/* private name: avoid clashing with MiniGL statics */
#include <exec/types.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include <proto/minigl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Device *TimerBase;
static struct timerequest NullTimerReq;
static unsigned int nullEclockScale;

#define SCRW 640
#define SCRH 480

#define DRAWS_PER_FRAME   500	/* our logs: ~400-1000 */
#define DRAWS_PER_MODEL    12	/* draws sharing one lock, like material ranges */
#define TEX_COUNT          16
#define TEX_SIZE           64
#define MODE_FRAMES        60

/* Draw-size mix (index counts close to the logged 570/984/1944): out of every 20
 * draws, 17 small, 2 medium, 1 large. Each is a triangulated grid of W x H
 * vertices -> 6*(W-1)*(H-1) indices. */
typedef struct { int w, h; } MeshSize;
static const MeshSize s_classes[3] = {
	{ 8, 15 },	/* 120 verts, 588 indices */
	{ 8, 25 },	/* 200 verts, 1008 indices */
	{ 10, 38 },	/* 380 verts, 1998 indices */
};
#define MIX_PERIOD 20

static int
drawClassFor(int d)
{
	int m = d % MIX_PERIOD;
	if(m == MIX_PERIOD - 1)
		return 2;
	if(m >= MIX_PERIOD - 3)
		return 1;
	return 0;
}

static int
timer_init(void)
{
	if(TimerBase != NULL)
		return 1;
	if(OpenDevice((STRPTR)"timer.device", UNIT_ECLOCK, (struct IORequest *)&NullTimerReq, 0) != 0)
		return 0;
	TimerBase = NullTimerReq.tr_node.io_Device;
	return TimerBase != NULL;
}

static unsigned int
now_us(void)
{
	struct EClockVal ev;
	ULONG freq;

	if(TimerBase == NULL)
		return 0;
	freq = ReadEClock(&ev);
	if(freq == 0)
		return 0;
	if(nullEclockScale == 0)
		nullEclockScale = (unsigned int)(((unsigned long long)1000000 << 16) / freq);
	return (unsigned int)(((unsigned long long)ev.ev_lo * nullEclockScale) >> 16);
}

/* Interleaved vertex: position (3 float) + colour (4 ubyte) + uv (2 float). */
typedef struct { float x, y, z; unsigned char r, g, b, a; float u, v; } Vtx;

#define STRIDE ((int)sizeof(Vtx))

static Vtx *vb;
static unsigned short *idx;
static int *vbase;		/* per-draw first vertex */
static int *drawVerts;		/* per-draw vertex count */
static int *idxOff;		/* per-draw offset into idx */
static int *idxNum;		/* per-draw index count */
static int *modelFirst;		/* per-model first draw */
static int *modelDraws;		/* per-model draw count */
static int *modelBase;		/* per-model first vertex */
static int *modelVerts;		/* per-model vertex count */
static int totalVerts;
static int totalIndices;
static int numDraws;
static int numModels;
static unsigned int textures[TEX_COUNT];

/* Triangulate one W x H vertex grid at `base`: two triangles per cell, indices
 * in vertex order so the locked window has long runs. Returns the index count. */
static int
buildDrawIndices(unsigned short *dst, int base, int w, int h)
{
	int r, c, n = 0;
	for(r = 0; r + 1 < h; r++){
		for(c = 0; c + 1 < w; c++){
			int a = base + r*w + c;
			int b = a + 1;
			int cc = a + w;
			int dd = cc + 1;
			dst[n++] = (unsigned short)a;
			dst[n++] = (unsigned short)b;
			dst[n++] = (unsigned short)cc;
			dst[n++] = (unsigned short)b;
			dst[n++] = (unsigned short)dd;
			dst[n++] = (unsigned short)cc;
		}
	}
	return n;
}

static void
setup(void)
{
	int d, k, i, m;
	int vsum = 0, isum = 0;

	numDraws = DRAWS_PER_FRAME;
	numModels = (numDraws + DRAWS_PER_MODEL - 1) / DRAWS_PER_MODEL;

	vbase = (int*)malloc((size_t)numDraws * sizeof(int));
	drawVerts = (int*)malloc((size_t)numDraws * sizeof(int));
	idxOff = (int*)malloc((size_t)numDraws * sizeof(int));
	idxNum = (int*)malloc((size_t)numDraws * sizeof(int));
	modelFirst = (int*)malloc((size_t)numModels * sizeof(int));
	modelDraws = (int*)malloc((size_t)numModels * sizeof(int));
	modelBase = (int*)malloc((size_t)numModels * sizeof(int));
	modelVerts = (int*)malloc((size_t)numModels * sizeof(int));

	for(d = 0; d < numDraws; d++){
		const MeshSize *c = &s_classes[drawClassFor(d)];
		int v = c->w * c->h;
		drawVerts[d] = v;
		vbase[d] = vsum;
		idxOff[d] = isum;
		idxNum[d] = 6*(c->w - 1)*(c->h - 1);
		vsum += v;
		isum += idxNum[d];
	}
	totalVerts = vsum;
	totalIndices = isum;

	/* Models are groups of consecutive draws over a contiguous vertex range. */
	for(m = 0; m < numModels; m++){
		int first = m * DRAWS_PER_MODEL;
		int nd = DRAWS_PER_MODEL;
		int last;
		if(first + nd > numDraws)
			nd = numDraws - first;
		last = first + nd - 1;
		modelFirst[m] = first;
		modelDraws[m] = nd;
		modelBase[m] = vbase[first];
		modelVerts[m] = (vbase[last] + drawVerts[last]) - vbase[first];
	}

	vb = (Vtx*)malloc((size_t)totalVerts * STRIDE);
	idx = (unsigned short*)malloc((size_t)totalIndices * sizeof(unsigned short));

	/* Each draw's grid sits in a 32x32 cell; 20x15 cells tile the frame. */
	for(d = 0; d < numDraws; d++){
		const MeshSize *c = &s_classes[drawClassFor(d)];
		int x0 = (d % 20) * 32;
		int y0 = ((d / 20) % 15) * 32;
		int base = vbase[d];
		int r, col;

		idxNum[d] = buildDrawIndices(idx + idxOff[d], base, c->w, c->h);
		for(r = 0; r < c->h; r++){
			for(col = 0; col < c->w; col++){
				Vtx *p = &vb[base + r*c->w + col];
				p->x = (float)x0 + (float)col * 32.0f / (float)(c->w - 1);
				p->y = (float)y0 + (float)r   * 32.0f / (float)(c->h - 1);
				p->z = 0.0f;
				p->r = p->g = p->b = 255; p->a = 255;
				p->u = (float)col / (float)(c->w - 1);
				p->v = (float)r   / (float)(c->h - 1);
			}
		}
	}

	/* Small procedural textures, bound round-robin between draws (state changes). */
	glGenTextures(TEX_COUNT, textures);
	{
		unsigned char *px = (unsigned char*)malloc(TEX_SIZE*TEX_SIZE*4);
		for(i = 0; i < TEX_COUNT; i++){
			int p;
			for(p = 0; p < TEX_SIZE*TEX_SIZE; p++){
				px[p*4+0] = (unsigned char)((p + i*37) & 0xFF);
				px[p*4+1] = (unsigned char)((p*3 + i*11) & 0xFF);
				px[p*4+2] = (unsigned char)((p*7 + i*5) & 0xFF);
				px[p*4+3] = 255;
			}
			glBindTexture(GL_TEXTURE_2D, textures[i]);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0,
			             GL_RGBA, GL_UNSIGNED_BYTE, px);
		}
		free(px);
	}
}

static void
setup_state(void)
{
	glViewport(0, 0, SCRW, SCRH);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, SCRW, 0, SCRH, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_LIGHTING);
	glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
	glEnable(GL_TEXTURE_2D);
	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, STRIDE, &vb[0].x);
	glEnableClientState(GL_COLOR_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, STRIDE, &vb[0].r);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, STRIDE, &vb[0].u);
}

static void
drawOne(int d)
{
	glBindTexture(GL_TEXTURE_2D, textures[d % TEX_COUNT]);
	glDrawElements(GL_TRIANGLES, idxNum[d], GL_UNSIGNED_SHORT, idx + idxOff[d]);
}

/* Run one whole frame in the given mode; return elapsed us. */
static unsigned int
runFrame(int mode)
{
	unsigned int t0, t1;
	int d, m;

	glClear(GL_COLOR_BUFFER_BIT);

	t0 = now_us();
	if(mode == 0){			/* unlocked */
		for(d = 0; d < numDraws; d++)
			drawOne(d);
	}else if(mode == 1){		/* one lock per model */
		for(m = 0; m < numModels; m++){
			int first = modelFirst[m];
			int e;
			glLockArrays(modelBase[m], modelVerts[m]);
			for(e = first; e < first + modelDraws[m]; e++)
				drawOne(e);
			glUnlockArrays();
		}
	}else{				/* one lock per draw */
		for(d = 0; d < numDraws; d++){
			glLockArrays(vbase[d], drawVerts[d]);
			drawOne(d);
			glUnlockArrays();
		}
	}
	mglSwitchDisplay();	/* submits the frame; glFinish does not */
	t1 = now_us();

	return t1 - t0;
}

/* Draw one frame in `mode` and count non-background samples on a coarse grid.
 * All three modes must produce the same picture (submission differs only), so a
 * mismatch means the locked/chunked path dropped or misplaced triangles. */
static int
frameCoverage(int mode)
{
	static const int gx = 32, gy = 24;
	int x, y, hits = 0;

	runFrame(mode);
	glFinish();
	for(y = 0; y < gy; y++){
		for(x = 0; x < gx; x++){
			unsigned char px[4] = { 0, 0, 0, 0 };
			int sx = (x * SCRW) / gx + SCRW/(2*gx);
			int sy = (y * SCRH) / gy + SCRH/(2*gy);
			glReadPixels(sx, sy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
			if(px[0] > 8 || px[1] > 8 || px[2] > 8)
				hits++;
		}
	}
	return hits;
}

static const char *
modeName(int mode)
{
	return mode == 0 ? "unlocked " : mode == 1 ? "modellock" : "perdraw  ";
}

int
main(void)
{
	int mode, f, m;
	unsigned int t, sum;
	int sumModelVerts = 0;

	freopen("nulldraw_test.log", "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);

	timer_init();

	printf("nulldraw_test: frame glDrawElements benchmark (MiniGL 29.x, real HW)\n");

	if(!MiniGLOpen()){
		printf("nulldraw_test: MiniGLOpen failed\n");
		return 1;
	}
	mglChoosePixelDepth(16);
	mglChooseWindowMode(GL_FALSE);
	if(mglCreateContext(0, 0, SCRW, SCRH) == NULL){
		printf("nulldraw_test: mglCreateContext failed\n");
		MiniGLClose();
		return 1;
	}

	setup();
	setup_state();

	for(m = 0; m < numModels; m++)
		sumModelVerts += modelVerts[m];

	printf("frame: draws=%d  verts=%d  indices=%d  avg indices/draw=%.0f\n",
	       numDraws, totalVerts, totalIndices, (double)totalIndices / numDraws);
	printf("models: %d  draws/model=%d  avg verts/model=%.0f (lock spans >1024)\n",
	       numModels, DRAWS_PER_MODEL, (double)sumModelVerts / numModels);
	printf("%-10s %10s %10s %10s\n", "mode", "ms/frame", "us/draw", "ns/index");

	for(mode = 0; mode < 3; mode++){
		sum = 0;
		for(f = 0; f < 3; f++)		/* reach steady state */
			runFrame(mode);
		for(f = 0; f < MODE_FRAMES; f++)
			sum += runFrame(mode);
		t = sum / (unsigned)MODE_FRAMES;
		printf("%-10s %10u %10u %10u\n", modeName(mode), t / 1000u, t / (unsigned)numDraws,
		       t * 1000u / (unsigned)totalIndices);
	}

	{
		int cov[3];
		for(mode = 0; mode < 3; mode++)
			cov[mode] = frameCoverage(mode);
		printf("coverage: unlocked=%d modellock=%d perdraw=%d (must match)\n",
		       cov[0], cov[1], cov[2]);
		if(cov[0] != cov[1] || cov[0] != cov[2])
			printf("WARNING: modes drew different pixels -- the locked/chunked path drops geometry\n");
	}

	mglDeleteContext();
	MiniGLClose();
	printf("nulldraw_test: done\n");
	return 0;
}
