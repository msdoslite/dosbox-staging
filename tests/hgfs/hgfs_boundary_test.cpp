// SPDX-License-Identifier: GPL-2.0-or-later
#include "../../src/hardware/vmware_hgfs.h"
#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sys/stat.h>
#include <thread>
using B = std::vector<uint8_t>;
extern void put(B&, size_t, uint64_t, size_t = 4);
extern uint32_t get(const B&, size_t);
extern B req(int);
extern void name(B&, size_t, const std::string&);
namespace fs = std::filesystem;
static std::string contents(const fs::path& p)
{
	std::ifstream f(p);
	return {std::istreambuf_iterator<char>(f), {}};
}
void boundary_tests(const char* directory)
{
	const auto base    = fs::path(directory).parent_path();
	const auto root    = base / "boundary-root",
	           outside = base / "boundary-root-other";
	fs::create_directories(root / "SUB");
	fs::create_directories(outside);
	std::ofstream(outside / "SECRET.TXT") << "outside sentinel";
	std::ofstream(root / "SAFE.TXT") << "safe";
	std::ofstream(root / "SUB" / "SAFE.TXT") << "nested";
	fs::create_symlink(outside / "SECRET.TXT", root / "LINK.TXT");
	fs::create_directory_symlink(outside, root / "LINKDIR");
	fs::create_hard_link(outside / "SECRET.TXT", root / "HARD.TXT");
	assert(mkfifo((root / "PIPE").c_str(), 0600) == 0);
	size_t attempts = 0;
	for (bool flat : {false, true}) {
		assert(VmwareHgfs::Init(root.c_str(), true, flat));
		auto path = [&](std::string s) {
			return flat ? s : std::string("SHARED\0", 7) + s;
		};
		auto close_handle = [](uint32_t h) {
			auto q = req(27);
			put(q, 10, h);
			assert(get(VmwareHgfs::Request(q), 6) == 0);
		};
		auto open = [&](const std::string& p, int access, int action) {
			auto q = req(24);
			name(q, 74, path(p));
			put(q, 18, access);
			put(q, 22, action);
			return VmwareHgfs::Request(q);
		};
		std::vector<std::string> bad = {"..",
		                                ".",
		                                "../SECRET.TXT",
		                                "..\\SECRET.TXT",
		                                "/etc/passwd",
		                                "C:SECRET.TXT",
		                                "\\\\host\\share",
		                                ".. ",
		                                "...",
		                                "SUB/../SECRET.TXT",
		                                "SUB\\..\\SECRET.TXT",
		                                "*",
		                                "**..",
		                                "?",
		                                "LINK.TXT",
		                                "HARD.TXT",
		                                "PIPE"};
		auto join = [](std::initializer_list<std::string> parts) {
			std::string r;
			for (auto& p : parts) {
				if (!r.empty()) {
					r += '\0';
				}
				r += p;
			}
			return r;
		};
		bad.push_back(join({"..", "boundary-root-other", "SECRET.TXT"}));
		bad.push_back(join(
		        {"SUB", "..", "..", "boundary-root-other", "SECRET.TXT"}));
		bad.push_back(join({"LINKDIR", "SECRET.TXT"}));
		bad.push_back(std::string("\0SECRET.TXT", 11));
		bad.push_back(std::string("SUB\0\0SECRET.TXT", 15));
		bad.push_back(std::string("SUB\0", 4));
		for (const auto& p : bad) {
			for (int access : {0, 2}) {
				auto r = open(p, access, access ? 4 : 0);
				if (get(r, 6) == 0) {
					std::cerr << "unexpected open flat=" << flat
					          << " access=" << access
					          << " name=";
					for (auto c : p) {
						std::cerr << (c ? c : '|');
					}
					std::cerr << "\n";
				}
				assert(get(r, 6) != 0);
				++attempts;
			}
			for (int op : {28, 31, 32, 33, 34, 35}) {
				auto q = req(op);
				name(q,
				     op == 28   ? 18
				     : op == 32 ? 130
				     : op == 33 ? 30
				                : 26,
				     path(p));
				if (op == 32) {
					put(q, 18, 2, 8);
					put(q, 30, 0, 8);
				}
				auto r = VmwareHgfs::Request(q);
				// Removing a hardlink/symlink cannot follow it,
				// but we reject them too.
				assert(get(r, 6) != 0);
				++attempts;
			}
			for (bool reverse : {false, true}) {
				auto q   = req(36);
				auto src = path(reverse ? "SAFE.TXT" : p),
				     dst = path(reverse ? p : "SAFE.TXT");
				name(q, 26, src);
				name(q, 43 + src.size(), dst);
				assert(get(VmwareHgfs::Request(q), 6) != 0);
				++attempts;
			}
			assert(contents(outside / "SECRET.TXT") == "outside sentinel");
			assert(contents(root / "SAFE.TXT") == "safe");
		}
		// Direct root enumeration exposes ordinary files, never a
		// synthetic SHARED.
		if (flat) {
			auto q = req(28);
			name(q, 18, "");
			auto r = VmwareHgfs::Request(q);
			assert(get(r, 6) == 0);
			auto h     = get(r, 10);
			bool found = false;
			for (unsigned i = 0; i < 10; ++i) {
				q = req(29);
				put(q, 10, h);
				put(q, 14, i);
				r = VmwareHgfs::Request(q);
				assert(get(r, 6) == 0);
				auto len = get(r, 134);
				if (!len) {
					break;
				}
				std::string n(r.begin() + 150, r.begin() + 150 + len);
				assert(n != "SHARED" && n != "HARD.TXT" &&
				       n != "LINK.TXT" && n != "PIPE");
				found |= n == "SAFE.TXT";
			}
			assert(found);
			close_handle(h);
		}
#ifdef __APPLE__
		// A file or directory moved by the host across the boundary
		// must stop working through the old handle, including the
		// similar-prefix sibling.
		auto r = open("SAFE.TXT", 2, 0);
		assert(get(r, 6) == 0);
		auto h = get(r, 10);
		fs::rename(root / "SAFE.TXT", outside / "MOVED.TXT");
		auto q = req(25);
		put(q, 10, h);
		put(q, 14, 0, 8);
		put(q, 22, 4);
		assert(get(VmwareHgfs::Request(q), 6) != 0);
		q = req(26);
		put(q, 10, h);
		put(q, 15, 0, 8);
		put(q, 23, 1);
		q.resize(36, '!');
		assert(get(VmwareHgfs::Request(q), 6) != 0);
		q = req(32);
		name(q, 130, "");
		put(q, 134, 1);
		put(q, 142, h);
		put(q, 18, 2, 8);
		put(q, 30, 0, 8);
		assert(get(VmwareHgfs::Request(q), 6) != 0);
		fs::rename(outside / "MOVED.TXT", root / "SAFE.TXT");
		close_handle(h);
		q = req(28);
		name(q, 18, path("SUB"));
		r = VmwareHgfs::Request(q);
		assert(get(r, 6) == 0);
		h = get(r, 10);
		fs::rename(root / "SUB", outside / "MOVEDDIR");
		q = req(29);
		put(q, 10, h);
		put(q, 14, 0);
		assert(get(VmwareHgfs::Request(q), 6) != 0);
		fs::rename(outside / "MOVEDDIR", root / "SUB");
		close_handle(h);
#endif

		// Race replacing a parent by an outside symlink in both root
		// modes.
		fs::create_directory(root / "RACE");
		std::atomic<bool> stop = false;
		std::thread swapper([&] {
			while (!stop) {
				std::error_code ec;
				fs::rename(root / "RACE", root / "PARK", ec);
				if (ec) {
					continue;
				}
				fs::create_directory_symlink(outside, root / "RACE", ec);
				fs::remove(root / "RACE", ec);
				fs::rename(root / "PARK", root / "RACE", ec);
			}
		});
		for (int i = 0; i < 500; ++i) {
			auto r = open(std::string("RACE\0SECRET.TXT", 15), 2, 4);
			if (get(r, 6) == 0) {
				close_handle(get(r, 10));
			}
		}
		stop = true;
		swapper.join();
		fs::remove_all(root / "RACE");
		fs::remove_all(root / "PARK");
		assert(contents(outside / "SECRET.TXT") == "outside sentinel");
		// Invalid lengths and flags must not turn parsing into a path
		// escape.
		for (uint32_t flag : {2u, 0xffffffffu}) {
			auto q = req(24);
			name(q, 74, path("SAFE.TXT"));
			put(q, 78, flag);
			assert(get(VmwareHgfs::Request(q), 6) != 0);
		}
		std::mt19937 rng(0x48474653);
		for (int i = 0; i < 10000; ++i) {
			B q(rng() % 200);
			for (auto& c : q) {
				c = uint8_t(rng());
			}
			if (q.size() >= 10) {
				q[0] = 'f';
				q[1] = ' ';
				put(q, 6, 24 + rng() % 16);
			}
			auto r = VmwareHgfs::Request(q);
			assert(r.size() <= 61440);
		}
		assert(contents(outside / "SECRET.TXT") == "outside sentinel");
		assert(contents(root / "SAFE.TXT") == "safe");
		VmwareHgfs::Reset();
	}
	VmwareHgfs::Init(nullptr);
	fs::remove_all(root);
	fs::remove_all(outside);
	std::cout << "Boundary tests passed: " << attempts
	          << " denied requests, 20000 malformed packets, both root modes\n";
}
