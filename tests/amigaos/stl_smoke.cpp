/*
 * M0 STL smoke test.
 *
 * Exercises exactly the libstdc++ surface reVC uses: <vector> (PedAttractor),
 * <string>/<unordered_map>/<memory>/<algorithm> (src/extras/ini.h). Built with
 * -fno-exceptions -fno-rtti, so any throwing/typeinfo dependency shows up here.
 */
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct Ped {
	int id;
	explicit Ped(int i) : id(i) {}
};

int main()
{
#ifdef AMIGAOS
	std::freopen("DH11:stl_smoke.log", "w", stdout);
#endif
	int failures = 0;

	std::vector<Ped> peds;
	peds.push_back(Ped(3));
	peds.push_back(Ped(1));
	peds.push_back(Ped(2));
	std::sort(peds.begin(), peds.end(),
	          [](const Ped &a, const Ped &b) { return a.id < b.id; });
	if (peds.size() != 3 || peds[0].id != 1 || peds[2].id != 3) {
		std::printf("FAIL vector/sort\n");
		failures++;
	} else {
		std::printf("ok   vector/sort\n");
	}

	std::string s = "Hello";
	s += ", ";
	s += "AmigaOS";
	if (s.size() != 14 || s.substr(7, 7) != "AmigaOS") {
		std::printf("FAIL string\n");
		failures++;
	} else {
		std::printf("ok   string\n");
	}

	std::unordered_map<std::string, int> map;
	map["one"] = 1;
	map["two"] = 2;
	map["three"] = 3;
	if (map.size() != 3 || map["two"] != 2) {
		std::printf("FAIL unordered_map\n");
		failures++;
	} else {
		std::printf("ok   unordered_map\n");
	}

	std::shared_ptr<std::string> sp = std::make_shared<std::string>("shared");
	std::shared_ptr<std::string> sp2 = sp;
	if (sp.use_count() != 2 || *sp2 != "shared") {
		std::printf("FAIL shared_ptr\n");
		failures++;
	} else {
		std::printf("ok   shared_ptr\n");
	}

	if (failures) {
		std::printf("stl_smoke: %d failure(s)\n", failures);
		return 1;
	}
	std::printf("stl_smoke: all ok\n");
	return 0;
}
