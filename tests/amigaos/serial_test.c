/*
 * serial_test.c - standalone AmigaOS 68k smoke test for serial output.
 *
 * Emits a handful of "SERIALTEST" lines over serial.device (OpenDevice +
 * CMD_WRITE), and also writes a harness-readable log to DH11:serial_test.log.
 * (KPrintF is not linkable with this libnix/mcrt=nix20 setup, so the raw exec
 * serial path is left out; serial.device reaches the same Paula serial port.)
 *
 * The Amiberry B1260 config uses serial_port=TCP://0.0.0.0:1234, so whatever
 * reaches the emulated serial port is delivered to the connected TCP client.
 * Connect a reader first; with no client the emulator drops the bytes.
 *
 * Build (Bebbo toolchain / docker):
 *   m68k-amigaos-gcc -m68040 -m68881 -mhard-float -mcrt=nix20 -O0 \
 *       -o DH11:serial_test serial_test.c
 */
#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/serial.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <string.h>

#ifndef LOGFILE
#define LOGFILE "DH11:serial_test.log"
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

int main(void)
{
	char line[128];
	int i;

	freopen(LOGFILE, "w", stdout);
	setvbuf(stdout, NULL, _IONBF, 0);

	printf("serial_test: start\n");

	serReady = serial_open();
	printf("serial.device open: %s\n", serReady ? "ok" : "FAILED");
	fflush(stdout);

	for (i = 1; i <= 5; i++) {
		sprintf(line, "SERIALTEST line %ld\r\n", (long)i);
		serial_write(line);

		printf("sent %ld\n", (long)i);
		fflush(stdout);
	}

	serial_write("SERIALTEST done\r\n");

	printf("serial_test: done (serial.device %s)\n",
	       serReady ? "used" : "unavailable");
	fflush(stdout);

	serial_close();
	return 0;
}
