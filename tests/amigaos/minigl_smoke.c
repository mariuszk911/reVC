/*
 * PiStorm3D / minigl.library bring-up smoke test (M0 "glhello").
 *
 * Opens the shared minigl.library through the PiStorm3D dispatch import
 * library, creates a context, draws a clear + a flat-shaded triangle, then
 * tears everything down. Run on a real PiStorm + RTG Amiga:
 *
 *   minigl_smoke > ram:minigl_smoke.log
 *
 * GL types/enums and the dispatch wrappers (gl and mgl, MiniGLOpen/
 * MiniGLClose) come from the minigl.library 29.x SDK
 * (minigl-shared-library/include). See tests/amigaos/Makefile: target
 * `minigl-smoke`.
 */

#include <stdio.h>

#include <proto/minigl.h>

int
main(void)
{
	GLint maxTexSize = 0;
	const GLubyte *ver, *ren;

	if (!MiniGLOpen()) {
		printf("MiniGLOpen failed\n");
		return 1;
	}

	mglChoosePixelDepth(16);
	mglChooseWindowMode(GL_FALSE);

	if (!mglCreateContext(0, 0, 320, 240)) {
		printf("mglCreateContext failed\n");
		MiniGLClose();
		return 1;
	}

	ver = glGetString(GL_VERSION);
	ren = glGetString(GL_RENDERER);
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
	printf("MiniGL version : %s\n", ver ? (const char *)ver : "(null)");
	printf("Renderer       : %s\n", ren ? (const char *)ren : "(null)");
	printf("Max texture    : %d\n", (int)maxTexSize);

	glViewport(0, 0, 320, 240);
	glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0.0, 320.0, 240.0, 0.0, -1.0, 1.0);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glBegin(GL_TRIANGLES);
	glColor4ub(255, 0, 0, 255);
	glVertex3f(160.0f, 60.0f, 0.0f);
	glColor4ub(0, 255, 0, 255);
	glVertex3f(60.0f, 180.0f, 0.0f);
	glColor4ub(0, 0, 255, 255);
	glVertex3f(260.0f, 180.0f, 0.0f);
	glEnd();

	mglSwitchDisplay();

	printf("triangle presented; exiting\n");

	mglDeleteContext();
	MiniGLClose();

	return 0;
}
