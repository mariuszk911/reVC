/*
 * M0 C++ runtime smoke test.
 *
 * RTCW is C and never exercised libnix's C++ runtime; reVC has many global
 * class instances (FrontEndMenuManager, TheText, SampleManager). This checks
 * that global constructors run before main, that new/delete work, and that
 * virtual dispatch and static storage behave.
 */
#include <cstdio>

static int g_ctorRuns;
static int g_dtorRuns;
static bool g_ctorBeforeMain;

struct Global {
	int value;
	Global() : value(42) { g_ctorRuns++; }
	virtual ~Global() { g_dtorRuns++; }
	virtual int get() const { return value; }
};

static Global g_global;

struct CtorProbe {
	CtorProbe() { g_ctorBeforeMain = (g_ctorRuns == 1); }
};
static CtorProbe g_probe;

struct Derived : Global {
	int get() const override { return value * 2; }
};

int main()
{
#ifdef AMIGAOS
	std::freopen("DH11:runtime_smoke.log", "w", stdout);
#endif
	int failures = 0;

	if (!g_ctorBeforeMain || g_global.value != 42 || g_ctorRuns != 1) {
		std::printf("FAIL global_ctor (runs=%d before_main=%d)\n",
		            g_ctorRuns, (int)g_ctorBeforeMain);
		failures++;
	} else {
		std::printf("ok   global_ctor\n");
	}

	Global *d = new Derived();
	if (d->get() != 84) {
		std::printf("FAIL new/virtual\n");
		failures++;
	} else {
		std::printf("ok   new/virtual\n");
	}
	delete d;

	Global *arr = new Global[4];
	bool arrOk = true;
	for (int i = 0; i < 4; i++)
		if (arr[i].value != 42)
			arrOk = false;
	delete[] arr;
	if (!arrOk) {
		std::printf("FAIL new[]/delete[]\n");
		failures++;
	} else {
		std::printf("ok   new[]/delete[]\n");
	}

	if (failures) {
		std::printf("runtime_smoke: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("runtime_smoke: all ok\n");
	return 0;
}
