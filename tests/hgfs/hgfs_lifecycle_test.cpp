// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../src/hardware/vmware_hgfs.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
using B = std::vector<uint8_t>;
extern void put(B&, size_t, uint64_t, size_t = 4);
extern uint32_t get(const B&, size_t);
extern B req(int);
extern void name(B&, size_t, const std::string&);
void lifecycle_tests(const char* directory)
{
	namespace fs    = std::filesystem;
	const auto root = fs::path(directory).parent_path() / "lifecycle";
	fs::create_directory(root);
	std::ofstream(root / "FILE.TXT") << "ABC";
	assert(VmwareHgfs::Init(root.c_str(), true, true));
	auto q = req(24);
	name(q, 74, "FILE.TXT");
	put(q, 18, 2);
	auto r = VmwareHgfs::Request(q);
	assert(get(r, 6) == 0);
	const auto h = get(r, 10);
	assert(chmod(root.c_str(), 0) == 0);
	q = req(25);
	put(q, 10, h);
	put(q, 14, 0, 8);
	put(q, 22, 3);
	assert(get(VmwareHgfs::Request(q), 6) == 8);
	auto w = req(26);
	put(w, 10, h);
	put(w, 15, 0, 8);
	put(w, 23, 1);
	w.resize(36, 'X');
	assert(get(VmwareHgfs::Request(w), 6) == 8);
	assert(chmod(root.c_str(), 0700) == 0);
	r = VmwareHgfs::Request(q);
	assert(get(r, 6) == 0 && std::string(r.begin() + 22, r.end()) == "ABC");
	fs::remove_all(root);
	assert(get(VmwareHgfs::Request(q), 6) == 8);
	fs::create_directory(root);
	std::ofstream(root / "FILE.TXT") << "NEW";
	assert(get(VmwareHgfs::Request(q), 6) == 8);
	auto close = req(27);
	put(close, 10, h);
	assert(get(VmwareHgfs::Request(close), 6) == 0);
	assert(VmwareHgfs::Init(root.c_str(), true, true));
	auto open = req(24);
	name(open, 74, "FILE.TXT");
	r = VmwareHgfs::Request(open);
	assert(get(r, 6) == 0);
	put(q, 10, get(r, 10));
	r = VmwareHgfs::Request(q);
	assert(std::string(r.begin() + 22, r.end()) == "NEW");
	VmwareHgfs::Init(nullptr);
	fs::remove_all(root);
	assert(!VmwareHgfs::Init(root.c_str(), true, true));
	assert(get(VmwareHgfs::Request(open), 6) == 8);
	std::cout << "Permission, deleted-root, reinit lifecycle tests passed\n";
}
