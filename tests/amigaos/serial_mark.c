/*
 * serial_mark.c - tiny AmigaOS 68k progress-sentinel helper for the target
 * test harness (tests/amigaos/run_target.py).
 *
 * Joins its argv into one line, writes it to serial.device unit 0 (same
 * OpenDevice + CMD_WRITE path as serial_test.c) and appends it to
 * DH11:target_serial.log. The serial port is best-effort: if it cannot be
 * opened the marker is still recorded in the log file and the program exits 0,
 * so a missing/held serial port never aborts a run.
 *
 * When the marker is exactly "TARGET:ALLDONE" it also creates
 * DH11:target_done.txt, which is the harness's primary completion signal
 * (independent of AmigaDOS `Echo >file` syntax).
 *
 * KPrintF does not link under -mcrt=nix20, so the raw serial.device path is
 * used instead.
 *
 * Build (Bebbo toolchain / docker):
 *   m68k-amigaos-gcc -m68040 -m68881 -mhard-float -mcrt=nix20 -O0 \
 *       -o DH11:serial_mark serial_mark.c
 */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/serial.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#ifndef SERIAL_LOG
#define SERIAL_LOG "DH11:target_serial.log"
#endif

#ifndef DONE_FILE
#define DONE_FILE "DH11:target_done.txt"
#endif

static struct MsgPort *serPort;
static struct IOExtSer *serReq;
static int serReady;

static int serial_open(void)
{
	serPort = CreateMsgPort();
	if (!serPort)
		return 0;

	serReq = (struct IOExtSer *)CreateIORequest(serPort,
	                                            sizeof(struct IOExtSer));
	if (!serReq) {
		DeleteMsgPort(serPort);
		serPort = 0;
		return 0;
	}

	if (OpenDevice((STRPTR)"serial.device", 0,
	               (struct IORequest *)serReq, 0) != 0) {
		DeleteIORequest((struct IORequest *)serReq);
		DeleteMsgPort(serPort);
		serReq = 0;
		serPort = 0;
		return 0;
	}

	serReq->io_SerFlags = 0;
	serReq->io_Baud = 9600;
	serReq->io_ReadLen = 8;
	serReq->io_WriteLen = 8;
	serReq->io_StopBits = 1;
	serReq->IOSer.io_Command = SDCMD_SETPARAMS;
	DoIO((struct IORequest *)serReq);

	return 1;
}

static void serial_write(const char *s)
{
	if (!serReady)
		return;
	serReq->IOSer.io_Command = CMD_WRITE;
	serReq->IOSer.io_Data = (APTR)s;
	serReq->IOSer.io_Length = (ULONG)strlen(s);
	DoIO((struct IORequest *)serReq);
}

static void serial_close(void)
{
	if (serReady)
		CloseDevice((struct IORequest *)serReq);
	if (serReq)
		DeleteIORequest((struct IORequest *)serReq);
	if (serPort)
		DeleteMsgPort(serPort);
	serReq = 0;
	serPort = 0;
	serReady = 0;
}

static void append_log(const char *line)
{
	FILE *f = fopen(SERIAL_LOG, "a");
	if (!f)
		f = fopen(SERIAL_LOG, "w");
	if (!f)
		return;
	fputs(line, f);
	fputs("\n", f);
	fclose(f);
}

static void write_done(void)
{
	FILE *f = fopen(DONE_FILE, "w");
	if (!f)
		return;
	fputs("TARGET_DONE\n", f);
	fclose(f);
}

int main(int argc, char **argv)
{
	char line[256];
	int i, n = 0;

	line[0] = 0;
	for (i = 1; i < argc; i++) {
		size_t len = strlen(line);
		if (i > 1 && len + 1 < sizeof(line)) {
			line[len++] = ' ';
			line[len] = 0;
		}
		strncat(line, argv[i], sizeof(line) - strlen(line) - 1);
	}
	n = (int)strlen(line);

	append_log(line);

	serReady = serial_open();
	if (serReady) {
		/* n <= sizeof(line)-1 == 255, so wire (258 bytes) always fits. */
		char wire[sizeof(line) + 2];
		memcpy(wire, line, (size_t)n);
		wire[n] = '\r';
		wire[n + 1] = '\n';
		wire[n + 2] = 0;
		serial_write(wire);
		serial_close();
	}

	if (strcmp(line, "TARGET:ALLDONE") == 0)
		write_done();

	return 0;
}
