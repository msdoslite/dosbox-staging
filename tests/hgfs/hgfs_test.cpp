#include "../../src/hardware/vmware_hgfs.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/resource.h>
#include <thread>
using B = std::vector<uint8_t>;
void put(B& b, size_t p, uint64_t v, size_t n = 4)
{
	if (b.size() < p + n) {
		b.resize(p + n);
	}
	for (size_t i = 0; i < n; ++i) {
		b[p + i] = uint8_t(v >> (8 * i));
	}
}
uint32_t get(const B& b, size_t p)
{
	return b.at(p) | (uint32_t(b.at(p + 1)) << 8) |
	       (uint32_t(b.at(p + 2)) << 16) | (uint32_t(b.at(p + 3)) << 24);
}
B req(int op)
{
	B b(10);
	b[0] = 'f';
	b[1] = ' ';
	put(b, 2, 123);
	put(b, 6, op);
	return b;
}
void name(B& b, size_t p, const std::string& s)
{
	put(b, p, s.size());
	b.resize(p + 17 + s.size());
	std::copy(s.begin(), s.end(), b.begin() + p + 16);
}
void boundary_tests(const char* directory);
void lifecycle_tests(const char* directory);
int main(int argc, char** argv)
{
	assert(argc == 2);
	std::filesystem::create_directories(argv[1]);
	std::ofstream(std::string(argv[1]) + "/HELLO.TXT") << "Hello HGFS\r\n";
	assert(VmwareHgfs::Init(argv[1]));
	auto b = req(28);
	name(b, 18, "");
	auto r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 0);
	auto h = get(r, 10);
	b      = req(29);
	put(b, 10, h);
	put(b, 14, 0);
	r = VmwareHgfs::Request(b);
	assert(get(r, 134) == 6);
	assert(std::string(r.begin() + 150, r.begin() + 156) == "SHARED");
	b = req(29);
	put(b, 10, h);
	put(b, 14, 1);
	r = VmwareHgfs::Request(b);
	assert(r.size() > 151 && get(r, 134) == 0);
	b = req(24);
	name(b, 74, std::string("SHARED\0hello.txt", 16));
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 0);
	h = get(r, 10);
	b = req(25);
	put(b, 10, h);
	put(b, 14, 0, 8);
	put(b, 22, 100);
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 0);
	assert(std::string(r.begin() + 22, r.end()) == "Hello HGFS\r\n");
	put(b, 14, 100, 8);
	r = VmwareHgfs::Request(b);
	assert(get(r, 10) == 0);
	b = req(26);
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 8);
	b = req(24);
	name(b, 74, std::string("SHARED\0..\0secret", 16));
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) != 0);
	std::filesystem::remove(std::string(argv[1]) + "/LINK");
	std::filesystem::create_symlink("/etc/passwd", std::string(argv[1]) + "/LINK");
	b = req(24);
	name(b, 74, std::string("SHARED\0LINK", 11));
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) != 0);
	for (int op = 24; op <= 39; ++op) {
		b = req(op);
		r = VmwareHgfs::Request(b);
		assert(get(r, 6) != 0 || op == 37);
	}
	VmwareHgfs::Reset();
	b = req(25);
	put(b, 10, h);
	put(b, 14, 0, 8);
	put(b, 22, 10);
	assert(get(VmwareHgfs::Request(b), 6) == 2);
	assert(VmwareHgfs::Init(argv[1], true));
	auto path = [](const char* leaf) {
		return std::string("SHARED\0", 7) + leaf;
	};
	b = req(24);
	name(b, 74, path("WRITE.TXT"));
	put(b, 18, 2);
	put(b, 22, 4);
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 0);
	h = get(r, 10);
	b = req(26);
	put(b, 10, h);
	put(b, 15, 0, 8);
	put(b, 23, 6);
	b.resize(41);
	std::copy_n("abcdef", 6, b.begin() + 35);
	r = VmwareHgfs::Request(b);
	assert(get(r, 6) == 0 && get(r, 10) == 6);
	b = req(32);
	put(b, 18, 2, 8);
	put(b, 30, 3, 8);
	name(b, 130, "");
	put(b, 134, 1);
	put(b, 142, h);
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(25);
	put(b, 10, h);
	put(b, 14, 0, 8);
	put(b, 22, 20);
	r = VmwareHgfs::Request(b);
	assert(std::string(r.begin() + 22, r.end()) == "abc");
	b = req(27);
	put(b, 10, h);
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(36);
	name(b, 26, path("WRITE.TXT"));
	name(b, 43 + path("WRITE.TXT").size(), path("RENAMED.TXT"));
	std::filesystem::remove(std::string(argv[1]) + "/RENAMED.TXT");
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(34);
	name(b, 26, path("RENAMED.TXT"));
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(33);
	name(b, 30, path("NEWDIR"));
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(35);
	name(b, 26, path("NEWDIR"));
	assert(get(VmwareHgfs::Request(b), 6) == 0);
	b = req(24);
	name(b, 74, path("LINK"));
	put(b, 18, 2);
	put(b, 22, 4);
	assert(get(VmwareHgfs::Request(b), 6) != 0);
	b = req(24);
	name(b, 74, path("..\0ESCAPE"));
	put(b, 18, 2);
	put(b, 22, 4);
	assert(get(VmwareHgfs::Request(b), 6) != 0);
	// A writable alias must not truncate a hard-linked file outside the share.
	auto outside = std::filesystem::path(argv[1]).parent_path() /
	               "outside-hgfs.txt";
	std::ofstream(outside) << "keep";
	auto hard = std::filesystem::path(argv[1]) / "HARD.TXT";
	std::filesystem::remove(hard);
	std::filesystem::create_hard_link(outside, hard);
	b = req(24);
	name(b, 74, path("HARD.TXT"));
	put(b, 18, 2);
	put(b, 22, 4);
	assert(get(VmwareHgfs::Request(b), 6) != 0 &&
	       std::filesystem::file_size(outside) == 4);
	std::filesystem::remove(hard);
	std::filesystem::remove(outside);

	auto request_ok = [](const B& packet) {
		auto result = VmwareHgfs::Request(packet);
		if (get(result, 6)) {
			std::cerr << "Failed op " << get(packet, 6)
			          << " status " << get(result, 6) << "\n";
		}
		assert(get(result, 6) == 0);
		return result;
	};
	auto close_file = [&](uint32_t handle) {
		auto q = req(27);
		put(q, 10, handle);
		request_ok(q);
	};
	auto open_file = [&](const char* leaf, int access, int action) {
		auto q = req(24);
		name(q, 74, path(leaf));
		put(q, 18, access);
		put(q, 22, action);
		return VmwareHgfs::Request(q);
	};
	// Permission toggling must work after the file itself becomes read-only.
	r = open_file("ATTR.TXT", 2, 4);
	assert(get(r, 6) == 0);
	close_file(get(r, 10));
	b = req(32);
	name(b, 130, path("ATTR.TXT"));
	put(b, 18, 128 | 1024 | 16, 8);
	put(b, 71, 4, 1);
	put(b, 74, 7, 8);
	put(b, 54, (11644473600ULL + 946684800) * 10000000, 8);
	request_ok(b);
	request_ok(b); // Setting the same read-only attributes is idempotent.
	assert(get(open_file("ATTR.TXT", 2, 0), 6) == 8);
	auto q = req(31);
	name(q, 26, path("ATTR.TXT"));
	r = request_ok(q);
	assert(r.at(63) == 4 && get(r, 66) == 7);
	assert(std::filesystem::file_size(std::filesystem::path(argv[1]) /
	                                  "ATTR.TXT") == 0);
	put(b, 71, 6, 1);
	put(b, 74, 0, 8);
	request_ok(b);
	r = open_file("ATTR.TXT", 2, 0);
	assert(get(r, 6) == 0);
	h = get(r, 10);
	// Append ignores a caller's offset, sparse seek and truncate/extend
	// round-trip.
	b = req(26);
	put(b, 10, h);
	put(b, 15, 4096, 8);
	put(b, 23, 1);
	b.resize(36);
	b[35] = 'X';
	request_ok(b);
	put(b, 14, 1, 1);
	put(b, 15, 0, 8);
	b[35] = 'Y';
	request_ok(b);
	b = req(25);
	put(b, 10, h);
	put(b, 14, 4095, 8);
	put(b, 22, 3);
	r = request_ok(b);
	assert(r.at(22) == 0 && r.at(23) == 'X' && r.at(24) == 'Y');
	close_file(h);
	assert(get(open_file("ATTR.TXT", 2, 3), 6) == 4);
	assert(get(open_file("MISSING.TXT", 2, 1), 6) == 1);
	// Failed rename must preserve both original files.
	r = open_file("OTHER.TXT", 2, 4);
	assert(get(r, 6) == 0);
	close_file(get(r, 10));
	b = req(36);
	name(b, 26, path("ATTR.TXT"));
	name(b, 43 + path("ATTR.TXT").size(), path("OTHER.TXT"));
	assert(get(VmwareHgfs::Request(b), 6) == 4);
	assert(std::filesystem::file_size(std::filesystem::path(argv[1]) /
	                                  "ATTR.TXT") == 4098);
	// Read-only mode rejects every mutation even for existing open handles.
	assert(VmwareHgfs::Init(argv[1], false));
	for (int op = 32; op <= 36; ++op) {
		b = req(op);
		assert(get(VmwareHgfs::Request(b), 6) == 8);
	}
	assert(get(open_file("ATTR.TXT", 2, 4), 6) == 8);
	assert(VmwareHgfs::Init(argv[1], true));
	// Every truncation of a valid write packet is rejected, not executed.
	r = open_file("OTHER.TXT", 2, 0);
	h = get(r, 10);
	b = req(26);
	put(b, 10, h);
	put(b, 15, 0, 8);
	put(b, 23, 4);
	b.resize(39, 'z');
	for (size_t n = 3; n < b.size(); ++n) {
		B truncated(b.begin(), b.begin() + n);
		assert(get(VmwareHgfs::Request(truncated), 6) != 0);
	}
	assert(std::filesystem::file_size(std::filesystem::path(argv[1]) /
	                                  "OTHER.TXT") == 0);
	close_file(h);
	b = req(32);
	name(b, 130, "");
	put(b, 18, 128, 8);
	put(b, 71, 0, 1);
	assert(get(VmwareHgfs::Request(b), 6) != 0);
	// Mid-path symlink and special files cannot be used for creation or
	// reading.
	auto dirlink = std::filesystem::path(argv[1]) / "DIRLINK";
	std::filesystem::remove(dirlink);
	std::filesystem::create_directory_symlink("..", dirlink);
	b = req(24);
	name(b, 74, std::string("SHARED\0DIRLINK\0ESCAPE", 21));
	put(b, 18, 2);
	put(b, 22, 4);
	assert(get(VmwareHgfs::Request(b), 6) != 0);

	// Race a host symlink replacement against guest create/truncate.
	// Outside data must remain intact, whichever directory lookup wins.
	const auto base        = std::filesystem::path(argv[1]);
	const auto outside_dir = base.parent_path() / "race-outside";
	std::filesystem::create_directories(outside_dir);
	std::ofstream(outside_dir / "VICTIM") << "keep outside";
	std::filesystem::create_directory(base / "RACE");
	std::atomic<bool> stop = false;
	std::thread swapper([&] {
		while (!stop) {
			std::error_code ec;
			std::filesystem::rename(base / "RACE", base / "PARKED", ec);
			if (ec) {
				continue;
			}
			std::filesystem::create_directory_symlink(outside_dir,
			                                          base / "RACE",
			                                          ec);
			std::filesystem::remove(base / "RACE", ec);
			std::filesystem::rename(base / "PARKED", base / "RACE", ec);
		}
	});
	for (int i = 0; i < 500; ++i) {
		b = req(24);
		name(b, 74, std::string("SHARED\0RACE\0VICTIM", 18));
		put(b, 18, 2);
		put(b, 22, 4);
		r = VmwareHgfs::Request(b);
		if (get(r, 6) == 0) {
			close_file(get(r, 10));
		}
	}
	stop = true;
	swapper.join();
	std::ifstream outside_read(outside_dir / "VICTIM");
	std::string content;
	std::getline(outside_read, content);
	assert(content == "keep outside");
	std::filesystem::remove_all(base / "RACE");
	std::filesystem::remove_all(base / "PARKED");
	std::filesystem::remove_all(outside_dir);
	// Exercise a real host write failure without filling the user's disk.
	r = open_file("OTHER.TXT", 2, 0);
	assert(get(r, 6) == 0);
	h = get(r, 10);
	struct rlimit old_limit, limited;
	assert(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
	limited          = old_limit;
	limited.rlim_cur = 0;
	auto old_signal  = std::signal(SIGXFSZ, SIG_IGN);
	assert(setrlimit(RLIMIT_FSIZE, &limited) == 0);
	b = req(26);
	put(b, 10, h);
	put(b, 15, 0, 8);
	put(b, 23, 1);
	b.resize(36, 'x');
	auto failure = VmwareHgfs::Request(b);
	assert(setrlimit(RLIMIT_FSIZE, &old_limit) == 0);
	std::signal(SIGXFSZ, old_signal);
	assert(get(failure, 6) == 12);
	close_file(h);
	b = req(24);
	b.resize(61441);
	assert(get(VmwareHgfs::Request(b), 6) == 7);
	r = open_file("OTHER.TXT", 0, 0);
	h = get(r, 10);
	b = req(25);
	put(b, 10, h);
	put(b, 14, 0, 8);
	put(b, 22, 61440);
	assert(get(VmwareHgfs::Request(b), 6) == 15);
	close_file(h);
	// Exhaustion is bounded; reset releases every handle and the server
	// recovers.
	VmwareHgfs::Reset();
	for (int i = 0; i < 256; ++i) {
		assert(get(open_file("OTHER.TXT", 0, 0), 6) == 0);
	}
	assert(get(open_file("OTHER.TXT", 0, 0), 6) == 10);
	VmwareHgfs::Reset();
	r = open_file("OTHER.TXT", 0, 0);
	assert(get(r, 6) == 0);
	close_file(get(r, 10));
	for (auto leaf : {"ATTR.TXT", "OTHER.TXT", "DIRLINK"}) {
		std::filesystem::remove(std::filesystem::path(argv[1]) / leaf);
	}
	VmwareHgfs::Reset();
	boundary_tests(argv[1]);
	lifecycle_tests(argv[1]);
	std::cout << "HGFS read/write/truncate/rename/directory/bounds tests passed\n";
}
