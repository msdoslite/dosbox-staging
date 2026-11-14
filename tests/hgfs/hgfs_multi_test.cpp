#include "../../src/hardware/vmware_hgfs.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
using B = std::vector<uint8_t>;
void put(B& b, size_t p, uint32_t v)
{
	if (b.size() < p + 4) {
		b.resize(p + 4);
	}
	for (int i = 0; i < 4; ++i) {
		b[p + i] = v >> (8 * i);
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
	put(b, 6, op);
	return b;
}
B openfile(const std::string& n, int mode = 0)
{
	auto b = req(24);
	put(b, 18, mode);
	put(b, 74, n.size());
	b.resize(90 + n.size());
	std::copy(n.begin(), n.end(), b.begin() + 90);
	return b;
}
B select(const std::string& n)
{
	std::string s = "dosbox.hgfs.select " + n;
	return B(s.begin(), s.end());
}
int main(int argc, char** argv)
{
	assert(argc == 2);
	namespace fs  = std::filesystem;
	fs::path base = argv[1];
	fs::create_directories(base / "a");
	fs::create_directories(base / "b");
	std::ofstream(base / "a/HELLO.TXT") << "AAA";
	std::ofstream(base / "b/HELLO.TXT") << "BBB";
	VmwareHgfs::Init(nullptr);
	assert(VmwareHgfs::AddShare("a", (base / "a").c_str(), true));
	assert(VmwareHgfs::AddShare("b", (base / "b").c_str(), false));
	assert(!VmwareHgfs::AddShare("A", (base / "b").c_str(), true));
	assert(!VmwareHgfs::AddShare("../bad", (base / "a").c_str(), true));
	assert(VmwareHgfs::Request(select("missing"), 1)[0] == '0');
	assert(VmwareHgfs::Request(select("A"), 1)[0] == '1');
	assert(VmwareHgfs::Request(select("b"), 2)[0] == '1');
	assert(VmwareHgfs::Request(select("a"), 2)[0] == '0'); // cannot rebind
	                                                       // live channel
	auto a = VmwareHgfs::Request(openfile("HELLO.TXT"), 1),
	     b = VmwareHgfs::Request(openfile("HELLO.TXT"), 2);
	assert(get(a, 6) == 0 && get(b, 6) == 0);
	auto ha = get(a, 10), hb = get(b, 10);
	auto r = req(25);
	put(r, 10, ha);
	put(r, 22, 3);
	assert(get(VmwareHgfs::Request(r, 2), 6) == 2); // foreign handle
	assert(VmwareHgfs::Request(r, 1).back() == 'A');
	assert(get(VmwareHgfs::Request(openfile("HELLO.TXT", 2), 2), 6) == 8);
	for (auto n : {std::string("..\0b\0HELLO.TXT", 14),
	               std::string("/etc/passwd"),
	               std::string("E:\\HELLO.TXT")}) {
		assert(get(VmwareHgfs::Request(openfile(n), 1), 6) != 0);
	}
	VmwareHgfs::CloseSession(1);
	put(r, 10, hb);
	assert(VmwareHgfs::Request(r, 2).back() == 'B');
	assert(VmwareHgfs::Request(select("a"), 1)[0] == '1');
	a = VmwareHgfs::Request(openfile("HELLO.TXT"), 1);
	assert(get(a, 10) != ha);
	put(r, 10, ha);
	assert(get(VmwareHgfs::Request(r, 1), 6) == 2); // stale handle
	fs::remove(base / "a/HELLO.TXT");
	fs::remove(base / "a");
	assert(get(VmwareHgfs::Request(openfile("HELLO.TXT"), 1), 6) != 0);
	put(r, 10, hb);
	assert(VmwareHgfs::Request(r, 2).back() == 'B');
	VmwareHgfs::Init(nullptr);
	std::cout << "Named-share isolation, readonly, stale/foreign handles and independent root loss passed\n";
}
