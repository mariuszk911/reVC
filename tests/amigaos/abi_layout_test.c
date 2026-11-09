/*
 * H10: AmigaOS exec/dos ABI layout guard (target-only, 68k).
 *
 * The port pokes exec structures by field (amigaos.cpp: FindTask(NULL),
 * tc_SPUpper/tc_SPLower, ReplyMsg((struct Message *)...), CloseLibrary(...)).
 * A toolchain/header change that silently alters these layouts would corrupt
 * memory at runtime, so pin the documented AmigaOS ABI sizes/offsets at compile
 * time with _Static_assert, and re-check them at runtime for the log.
 *
 * Built only into build/amigaos/test (DH11:); not part of the host suite (the exec
 * headers do not exist on Linux). Writes DH11:abi_layout.log.
 *
 * Values are the documented AmigaOS 68k ABI (and what this Bebbo toolchain's
 * headers define); update them only with a real ABI change, never to make the
 * test pass.
 */
#include <exec/types.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <stdio.h>

#define OFF(t, f) ((long)((char *)&((t *)0)->f - (char *)0))

_Static_assert(sizeof(struct Node)        == 14,  "struct Node ABI");
_Static_assert(sizeof(struct MinNode)     == 8,   "struct MinNode ABI");
_Static_assert(sizeof(struct Message)     == 20,  "struct Message ABI");
_Static_assert(sizeof(struct MsgPort)     == 34,  "struct MsgPort ABI");
_Static_assert(sizeof(struct Library)     == 34,  "struct Library ABI");
_Static_assert(sizeof(struct Task)        == 92,  "struct Task ABI");
_Static_assert(sizeof(struct MemList)     == 24,  "struct MemList ABI");
_Static_assert(sizeof(struct MemEntry)    == 8,   "struct MemEntry ABI");
_Static_assert(sizeof(struct IOStdReq)    == 48,  "struct IOStdReq ABI");
_Static_assert(sizeof(struct FileInfoBlock) == 260, "struct FileInfoBlock ABI");
_Static_assert(sizeof(struct DateStamp)   == 12,  "struct DateStamp ABI");

_Static_assert(OFF(struct Task, tc_Node)      == 0,  "Task.tc_Node");
_Static_assert(OFF(struct Task, tc_SigWait)   == 22, "Task.tc_SigWait");
_Static_assert(OFF(struct Task, tc_SPUpper)   == 62, "Task.tc_SPUpper");
_Static_assert(OFF(struct Task, tc_MemEntry)  == 74, "Task.tc_MemEntry");
_Static_assert(OFF(struct Task, tc_UserData)  == 88, "Task.tc_UserData");

_Static_assert(OFF(struct MsgPort, mp_SigBit)  == 15, "MsgPort.mp_SigBit");
_Static_assert(OFF(struct MsgPort, mp_SigTask) == 16, "MsgPort.mp_SigTask");
_Static_assert(OFF(struct MsgPort, mp_MsgList) == 20, "MsgPort.mp_MsgList");

_Static_assert(OFF(struct Message, mn_Node)      == 0,  "Message.mn_Node");
_Static_assert(OFF(struct Message, mn_ReplyPort) == 14, "Message.mn_ReplyPort");
_Static_assert(OFF(struct Message, mn_Length)    == 18, "Message.mn_Length");

_Static_assert(OFF(struct Library, lib_Version)  == 20, "Library.lib_Version");
_Static_assert(OFF(struct Library, lib_IdString) == 24, "Library.lib_IdString");

int main(void)
{
	int fails = 0;

	freopen("DH11:abi_layout.log", "w", stdout);
	printf("AmigaOS ABI layout test\n");

	/* The _Static_asserts above are the real guard; mirror them at runtime so
	 * the harness gets a terminal line and the expected values are recorded. */
	{
		struct { const char *what; long got; long want; } checks[] = {
			{ "sizeof(Node)",        (long)sizeof(struct Node),        14  },
			{ "sizeof(MinNode)",     (long)sizeof(struct MinNode),     8   },
			{ "sizeof(Message)",     (long)sizeof(struct Message),     20  },
			{ "sizeof(MsgPort)",     (long)sizeof(struct MsgPort),     34  },
			{ "sizeof(Library)",     (long)sizeof(struct Library),     34  },
			{ "sizeof(Task)",        (long)sizeof(struct Task),        92  },
			{ "sizeof(MemList)",     (long)sizeof(struct MemList),     24  },
			{ "sizeof(MemEntry)",    (long)sizeof(struct MemEntry),    8   },
			{ "sizeof(IOStdReq)",    (long)sizeof(struct IOStdReq),    48  },
			{ "sizeof(FileInfoBlock)",(long)sizeof(struct FileInfoBlock),260 },
			{ "sizeof(DateStamp)",   (long)sizeof(struct DateStamp),   12  },
			{ "Task.tc_SigWait",     OFF(struct Task, tc_SigWait),     22  },
			{ "Task.tc_SPUpper",     OFF(struct Task, tc_SPUpper),     62  },
			{ "Task.tc_MemEntry",    OFF(struct Task, tc_MemEntry),    74  },
			{ "Task.tc_UserData",    OFF(struct Task, tc_UserData),    88  },
			{ "MsgPort.mp_SigBit",   OFF(struct MsgPort, mp_SigBit),   15  },
			{ "MsgPort.mp_SigTask",  OFF(struct MsgPort, mp_SigTask),  16  },
			{ "Message.mn_Length",   OFF(struct Message, mn_Length),   18  },
			{ "Library.lib_Version", OFF(struct Library, lib_Version), 20  },
			{ "Library.lib_IdString",OFF(struct Library, lib_IdString),24  },
		};
		int i, n = (int)(sizeof(checks) / sizeof(checks[0]));
		for (i = 0; i < n; i++) {
			if (checks[i].got != checks[i].want) {
				printf("FAIL %-24s got %ld want %ld\n",
				       checks[i].what, checks[i].got, checks[i].want);
				fails++;
			}
		}
		printf("checked %d sizes/offsets\n", n);
	}

	if (fails) {
		printf("abi_layout_test: %d failure(s)\n", fails);
		return 1;
	}
	printf("abi_layout_test: all ok\n");
	return 0;
}
