// SPDX-License-Identifier: GPL-2.0-or-later
// HGFS v3 prototype. Wire layout follows VMSMOUNT vmshf.h
// (Ken Kato, Eduardo Casino; GPL-2.0-or-later).
#include "vmware_hgfs.h"
#if !defined(__APPLE__) && !defined(__linux__)
namespace VmwareHgfs {
bool Init(const char*, bool, bool)
{
	return false;
}
void Reset() {}
void CloseSession(uint16_t) {}
bool AddShare(const char*, const char*, bool)
{
	return false;
}
std::vector<uint8_t> Request(const std::vector<uint8_t>&, uint16_t)
{
	return {};
}
std::vector<uint8_t> Request(const std::vector<uint8_t>&)
{
	return {};
}
} // namespace VmwareHgfs
#else
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace VmwareHgfs {
using Bytes = std::vector<uint8_t>;
struct Object {
	int fd                           = -1;
	bool directory                   = false;
	bool root                        = false;
	std::vector<std::string> entries = {};
};
struct State {
	bool flat_root                     = false;
	bool writable                      = false;
	int root_fd                        = -1;
	std::map<uint32_t, Object> objects = {};
	void reset()
	{
		for (auto& [id, o] : objects) {
			close(o.fd);
		}
		objects.clear();
	}
	~State()
	{
		reset();
		if (root_fd >= 0) {
			close(root_fd);
		}
	}
};
static uint32_t next_handle = 1;
static State default_state;
static std::map<std::string, std::unique_ptr<State>> shares;
static std::map<uint16_t, std::unique_ptr<State>> sessions;
static uint64_t get(const Bytes& b, size_t p, size_t n = 4)
{
	if (p > b.size() || n > b.size() - p) {
		throw uint32_t(7);
	}
	uint64_t v = 0;
	for (size_t i = 0; i < n; ++i) {
		v |= uint64_t(b[p + i]) << (8 * i);
	}
	return v;
}
static void put(Bytes& b, size_t p, uint64_t v, size_t n = 4)
{
	if (b.size() < p + n) {
		b.resize(p + n);
	}
	for (size_t i = 0; i < n; ++i) {
		b[p + i] = uint8_t(v >> (8 * i));
	}
}
static uint32_t error()
{
	switch (errno) {
	case ENOENT: return 1;
	case EEXIST: return 4;
	case ENOTDIR: return 5;
	case ENOTEMPTY: return 6;
	case ENOSPC:
	case EDQUOT:
	case EFBIG: return 12;
	case EXDEV: return 16;
	case ENAMETOOLONG: return 14;
	case EBADF: return 2;
	default: return 8;
	}
}
// Private extended attribute: HGFS hidden/system/archive bits, not host flags.
static constexpr auto DosAttrName = "user.dosbox.hgfs.attributes";
static uint8_t dos_attributes(int fd)
{
	uint8_t value = 0;
#ifdef __APPLE__
	auto n = fgetxattr(fd, DosAttrName, &value, 1, 0, 0);
#else
	auto n = fgetxattr(fd, DosAttrName, &value, 1);
#endif
	return n == 1 ? value & 7 : 0;
}
static void set_dos_attributes(int fd, uint8_t value)
{
	if (dos_attributes(fd) == value) {
		return;
	}
#ifdef __APPLE__
	auto result = fsetxattr(fd, DosAttrName, &value, 1, 0, 0);
#else
	auto result = fsetxattr(fd, DosAttrName, &value, 1, 0);
#endif
	if (result) {
		throw error();
	}
}
static std::string upper(std::string s)
{
	for (auto& c : s) {
		if (c >= 'a' && c <= 'z') {
			c -= 32;
		}
	}
	return s;
}
// HGFS names are NUL-separated components, never host path strings.
// Do not interpret DOS wildcards, device names or drive/UNC prefixes here.
static void validate_component(const std::string& name)
{
	if (name.empty() || name == "." || name == ".." || name.back() == '.' ||
	    name.back() == ' ' ||
	    name.find_first_of("/\\:*?\"<>|") != std::string::npos ||
	    std::any_of(name.begin(), name.end(), [](unsigned char c) {
		    return c < 32;
	    })) {
		throw uint32_t(8);
	}
}
// Keep the exported inode pinned; do not silently switch to a replacement
// directory at the same host path. Revoked directory permissions apply even
// to already-open guest files. Close requests still retire handles normally.
static void check_root_available(State& state)
{
	struct stat st = {};
	if (state.root_fd < 0 || fstat(state.root_fd, &st) ||
	    st.st_nlink == 0 || faccessat(state.root_fd, ".", R_OK | X_OK, 0)) {
		throw uint32_t(8);
	}
}
// Revalidate live descriptors as well as names. In particular a host rename
// must not leave a guest handle usable after its file leaves the exported tree.
static void check_confined(State& state, int fd)
{
	struct stat st = {}, root = {};
	if (fstat(fd, &st) || fstat(state.root_fd, &root) || st.st_dev != root.st_dev ||
	    (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
	    (S_ISREG(st.st_mode) && st.st_nlink != 1)) {
		throw uint32_t(8);
	}
#ifdef __APPLE__
	char root_path[MAXPATHLEN], file_path[MAXPATHLEN];
	if (fcntl(state.root_fd, F_GETPATH, root_path) ||
	    fcntl(fd, F_GETPATH, file_path)) {
		throw uint32_t(8);
	}
	const std::string root_name(root_path), file_name(file_path);
	if (file_name != root_name &&
	    file_name.compare(0, root_name.size() + 1, root_name + "/") != 0) {
		throw uint32_t(8);
	}
#endif
}
static std::vector<std::string> entries(State& state, int fd)
{
	check_confined(state, fd);
	int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (copy < 0) {
		throw error();
	}
	DIR* d = fdopendir(copy);
	if (!d) {
		close(copy);
		throw error();
	}
	std::vector<std::string> result;
	while (auto* e = readdir(d)) {
		std::string name = e->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		struct stat st = {};
		if (fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW)) {
			continue;
		}
		if (S_ISDIR(st.st_mode) ||
		    (S_ISREG(st.st_mode) && st.st_nlink == 1)) {
			result.push_back(name);
		}
	}
	closedir(d);
	std::sort(result.begin(), result.end());
	return result;
}
static int open_name(State& state, const Bytes& b, size_t p, bool& virtual_root)
{
	const auto len = get(b, p);
	if (len > 1024 || p + 16 + len > b.size()) {
		throw uint32_t(7);
	}
	if (get(b, p + 4) > 1 || get(b, p + 8) > 2) {
		throw uint32_t(15);
	}
	if (get(b, p + 4) & 1) {
		auto it = state.objects.find(uint32_t(get(b, p + 12)));
		if (it == state.objects.end()) {
			throw uint32_t(2);
		}
		check_confined(state, it->second.fd);
		virtual_root = it->second.root;
		int fd       = dup(it->second.fd);
		if (fd < 0) {
			throw error();
		}
		return fd;
	}
	virtual_root = (len == 0 && !state.flat_root);
	if (len == 0) {
		return dup(state.root_fd);
	}
	std::vector<std::string> parts;
	std::string part;
	for (size_t i = 0; i < len; ++i) {
		char c = char(b[p + 16 + i]);
		if (!c) {
			parts.push_back(part);
			part.clear();
		} else {
			part += c;
		}
	}
	parts.push_back(part);
	if (!state.flat_root && upper(parts.front()) != "SHARED") {
		throw uint32_t(1);
	}
	int fd = dup(state.root_fd);
	if (fd < 0) {
		throw error();
	}
	try {
		for (size_t i = state.flat_root ? 0 : 1; i < parts.size(); ++i) {
			auto name = parts[i];
			validate_component(name);
			// DOS clients request case-insensitive lookup. Reject
			// ambiguous aliases.
			if (get(b, p + 8) != 1) {
				std::string match;
				for (const auto& e : entries(state, fd)) {
					if (upper(e) == upper(name)) {
						if (!match.empty()) {
							throw uint32_t(9);
						}
						match = e;
					}
				}
				if (match.empty()) {
					throw uint32_t(1);
				}
				name = match;
			}
			check_confined(state, fd);
			int child = openat(fd,
			                   name.c_str(),
			                   O_RDONLY | O_NOFOLLOW | O_NONBLOCK |
			                           O_CLOEXEC);
			if (child < 0) {
				throw error();
			}
			close(fd);
			fd = child;
			check_confined(state, fd);
			struct stat st = {};
			if (fstat(fd, &st) ||
			    ((!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
			     (S_ISREG(st.st_mode) && st.st_nlink != 1))) {
				throw uint32_t(8);
			}
		}
	} catch (...) {
		close(fd);
		throw;
	}
	return fd;
}
// Resolve the parent first. Mutations never follow a final symlink and
// never resolve a path relative to the host working directory.
struct Parent {
	int fd = -1;
	std::string name;
	Parent(State& state, const Bytes& b, size_t p)
	{
		auto len = get(b, p);
		if (len > 1024 || p + 16 + len > b.size() ||
		    get(b, p + 4) != 0 || get(b, p + 8) > 2) {
			throw uint32_t(7);
		}
		std::string path(b.begin() + p + 16, b.begin() + p + 16 + len);
		if (!path.empty() && path.front() == '\0') {
			throw uint32_t(8);
		}
		auto slash = path.rfind('\0');
		if (slash == std::string::npos && !state.flat_root) {
			throw uint32_t(8);
		}
		name = slash == std::string::npos ? path : path.substr(slash + 1);
		validate_component(name);
		Bytes parent = b;
		put(parent, p, slash == std::string::npos ? 0 : slash);
		bool root = false;
		fd        = open_name(state, parent, p, root);
		try {
			if (get(b, p + 8) != 1) {
				std::string match;
				for (const auto& e : entries(state, fd)) {
					if (upper(e) == upper(name)) {
						if (!match.empty()) {
							throw uint32_t(9);
						}
						match = e;
					}
				}
				if (!match.empty()) {
					name = match;
				}
			}
			struct stat st = {};
			if (!fstatat(fd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) &&
			    ((!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
			     (S_ISREG(st.st_mode) && st.st_nlink != 1))) {
				throw uint32_t(8);
			}
		} catch (...) {
			close(fd);
			fd = -1;
			throw;
		}
	}
	~Parent()
	{
		if (fd >= 0) {
			close(fd);
		}
	}
	Parent(const Parent&)            = delete;
	Parent& operator=(const Parent&) = delete;
};
static void check_mutable(State& state, int fd)
{
	check_confined(state, fd);
	struct stat st = {}, root = {};
	if (fstat(fd, &st) || fstat(state.root_fd, &root) ||
	    (st.st_dev == root.st_dev && st.st_ino == root.st_ino) ||
	    (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) ||
	    (S_ISREG(st.st_mode) && st.st_nlink > 1)) {
		throw uint32_t(8);
	}
}
static int open_file(State& state, const Bytes& b)
{
	const auto access = get(b, 18), action = get(b, 22);
	if (access > 2 || action > 4) {
		throw uint32_t(15);
	}
	if (!state.writable && (access != 0 || action != 0)) {
		throw uint32_t(8);
	}
	if (action != 0 && access == 0) {
		throw uint32_t(8);
	}
	Parent p(state, b, 74);
	check_confined(state, p.fd);
	int flags = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR;
	if (action >= 2) {
		flags |= O_CREAT;
	}
	if (action == 3) {
		flags |= O_EXCL;
	}
	bool created          = false;
	int fd                = -1;
	const auto safe_flags = O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC;
	if (flags & O_CREAT) {
		fd = openat(p.fd, p.name.c_str(), flags | O_EXCL | safe_flags, 0600);
		created = fd >= 0;
		if (fd < 0 && errno == EEXIST && action != 3) {
			fd = openat(p.fd,
			            p.name.c_str(),
			            (flags & ~(O_CREAT | O_EXCL)) | safe_flags);
		}
	} else {
		fd = openat(p.fd, p.name.c_str(), flags | safe_flags);
	}
	if (fd < 0) {
		throw error();
	}
	try {
		check_confined(state, fd);
		struct stat st = {};
		if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
			throw uint32_t(8);
		}
		if (access) {
			check_mutable(state, fd);
		}
		if (created) {
			const auto mask = get(b, 10, 8);
			if (mask & 64) {
				set_dos_attributes(fd, uint8_t(get(b, 30, 8) & 7));
			}
			if ((mask & 8) && !(get(b, 27, 1) & 2) && fchmod(fd, 0400)) {
				throw error();
			}
		}
		if ((action == 1 || action == 4) && ftruncate(fd, 0)) {
			throw error();
		}
	} catch (...) {
		close(fd);
		throw;
	}
	return fd;
}
static void attr(State& state, Bytes& out, size_t pos, int fd)
{
	check_confined(state, fd);
	struct stat st = {};
	if (fstat(fd, &st)) {
		throw error();
	}
	out.resize(std::max(out.size(), pos + 104), 0);
	put(out, pos, 1 | 2 | 8 | 16 | 32 | 128 | 1024, 8);
	put(out, pos + 8, S_ISDIR(st.st_mode) ? 1 : 0);
	put(out, pos + 12, S_ISREG(st.st_mode) ? uint64_t(st.st_size) : 0, 8);
	const auto filetime = [](time_t t) {
		return uint64_t(int64_t(t) + 11644473600LL) * 10000000ULL;
	};
	put(out, pos + 28, filetime(st.st_atime), 8);
	put(out, pos + 36, filetime(st.st_mtime), 8);
	put(out, pos + 44, filetime(st.st_ctime), 8);
	put(out, pos + 56, dos_attributes(fd), 8);
	out[pos + 53] = (st.st_mode & S_IRUSR ? 4 : 0) |
	                (state.writable && (st.st_mode & S_IWUSR) ? 2 : 0) |
	                (S_ISDIR(st.st_mode) ? 1 : 0);
	out[pos + 54] = 5;
	out[pos + 55] = 5;
}
void Reset()
{
	sessions.clear();
	default_state.reset();
}
void CloseSession(uint16_t session)
{
	sessions.erase(session);
}
bool Init(const char* root, bool allow_write, bool flat)
{
	Reset();
	shares.clear();
	default_state.writable  = allow_write;
	default_state.flat_root = flat;
	if (default_state.root_fd >= 0) {
		close(default_state.root_fd);
	}
	default_state.root_fd = -1;
	if (root && *root) {
		default_state.root_fd = open(root,
		                             O_RDONLY | O_DIRECTORY |
		                                     O_NOFOLLOW | O_CLOEXEC);
	}
	return default_state.root_fd >= 0;
}
static bool valid_share(const std::string& name)
{
	return !name.empty() && name.size() <= 16 &&
	       std::all_of(name.begin(), name.end(), [](unsigned char c) {
		       return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		              (c >= '0' && c <= '9') || c == '_' || c == '-';
	       });
}
bool AddShare(const char* name, const char* root, bool allow_write)
{
	if (!name || !root || !valid_share(name) || shares.size() >= 8 ||
	    shares.count(upper(name))) {
		return false;
	}
	auto share = std::make_unique<State>();
	share->root_fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (share->root_fd < 0) {
		return false;
	}
	share->writable  = allow_write;
	share->flat_root = true;
	shares.emplace(upper(name), std::move(share));
	return true;
}
static Bytes dispatch_request(State& state, const Bytes& in)

{
	Bytes out(10, 0);
	out[0] = '1';
	out[1] = ' ';
	try {
		if (state.root_fd < 0) {
			throw uint32_t(8);
		}
		if (in.size() > 61440 || in.size() < 2 || in[0] != 'f' ||
		    in[1] != ' ') {
			throw uint32_t(7);
		}
		if (in.size() == 2) {
			check_root_available(state);
			return Bytes{'1', ' '};
		}
		put(out, 2, get(in, 2));
		const auto op = get(in, 6);
		if (op != 27 && op != 30) {
			check_root_available(state);
		}
		if (op == 24 || op == 28) {
			if (state.objects.size() >= 256) {
				throw uint32_t(10);
			}

			bool root = false;
			int fd    = op == 24 ? open_file(state, in)
			                     : open_name(state, in, 18, root);
			if (fd < 0) {
				throw error();
			}
			struct stat st = {};
			if (fstat(fd, &st)) {
				const auto code = error();
				close(fd);
				throw code;
			}
			bool dir = S_ISDIR(st.st_mode);
			if ((op == 28 && !dir) || (op == 24 && dir)) {
				close(fd);
				throw uint32_t(op == 28 ? 5 : 8);
			}
			Object o;
			o.fd        = fd;
			o.directory = dir;
			o.root      = root;
			try {
				if (dir) {
					o.entries = root ? std::vector<std::string>{"SHARED"}
					                 : entries(state, fd);
				}
			} catch (...) {
				close(fd);
				throw;
			}
			if (!next_handle) {
				close(fd);
				throw uint32_t(10);
			}
			const auto h = next_handle++;
			state.objects.emplace(h, std::move(o));
			out.resize(op == 24 ? 26 : 22);
			put(out, 10, h);
		} else if (op == 25) {
			const auto h = uint32_t(get(in, 10));
			auto it      = state.objects.find(h);
			if (it == state.objects.end() || it->second.directory) {
				throw uint32_t(2);
			}
			auto offset = get(in, 14, 8);
			auto count  = get(in, 22);
			if (count > 61440 - 22 || offset > INT64_MAX ||
			    count > uint64_t(INT64_MAX) - offset) {
				throw uint32_t(15);
			}
			check_confined(state, it->second.fd);
			out.resize(22 + count);
			auto n = pread(it->second.fd,
			               out.data() + 22,
			               count,
			               off_t(offset));
			if (n < 0) {
				throw error();
			}
			out.resize(22 + size_t(n));
			put(out, 10, uint32_t(n));
		} else if (op == 26) {
			if (!state.writable) {
				throw uint32_t(8);
			}
			auto it = state.objects.find(uint32_t(get(in, 10)));
			if (it == state.objects.end() || it->second.directory) {
				throw uint32_t(2);
			}
			auto offset = get(in, 15, 8), count = get(in, 23);
			auto flags = get(in, 14, 1);
			if (count > 61440 - 35 || 35 + count > in.size() ||
			    offset > INT64_MAX ||
			    count > uint64_t(INT64_MAX) - offset || flags > 1) {
				throw uint32_t(15);
			}
			check_mutable(state, it->second.fd);
			if (flags) {
				struct stat st = {};
				if (fstat(it->second.fd, &st)) {
					throw error();
				}
				offset = uint64_t(st.st_size);
			}
			if (offset > INT64_MAX ||
			    count > uint64_t(INT64_MAX) - offset) {
				throw uint32_t(15);
			}
			size_t done = 0;
			while (done < count) {
				auto n = pwrite(it->second.fd,
				                in.data() + 35 + done,
				                count - done,
				                off_t(offset + done));
				if (n < 0) {
					if (errno == EINTR) {
						continue;
					}
					if (done) {
						break;
					}
					throw error();
				}
				if (n == 0) {
					throw uint32_t(12);
				}
				done += size_t(n);
			}
			out.resize(22);
			put(out, 10, done);
		} else if (op == 27 || op == 30) {
			auto it = state.objects.find(uint32_t(get(in, 10)));
			if (it == state.objects.end()) {
				throw uint32_t(2);
			}
			// Surface delayed host write failures before retiring
			// the handle.
			int saved_error = 0;
			const int fd    = it->second.fd;
			const int flags = fcntl(fd, F_GETFL);
			if (!it->second.directory && flags >= 0 &&
			    (flags & O_ACCMODE) != O_RDONLY && fsync(fd)) {
				saved_error = errno;
			}
			if (close(fd) && !saved_error) {
				saved_error = errno;
			}
			state.objects.erase(it);
			if (saved_error) {
				errno = saved_error;
				throw error();
			}
			out.resize(18);
		} else if (op == 29) {
			auto it = state.objects.find(uint32_t(get(in, 10)));
			if (it == state.objects.end() || !it->second.directory) {
				throw uint32_t(2);
			}
			check_confined(state, it->second.fd);
			const auto index = get(in, 14); // VMSMOUNT tests reply
			                                // length strictly
			                                // greater than its
			                                // packed struct.
			out.resize(152);
			if (index < it->second.entries.size()) {
				const auto& name = it->second.entries[index];
				int fd = it->second.root
				               ? dup(state.root_fd)
				               : openat(it->second.fd,
				                        name.c_str(),
				                        O_RDONLY | O_NOFOLLOW |
				                                O_NONBLOCK |
				                                O_CLOEXEC);
				if (fd < 0) {
					throw error();
				}
				try {
					attr(state, out, 30, fd);
				} catch (...) {
					close(fd);
					throw;
				}
				close(fd);
				put(out, 10, 1, 8);
				put(out, 134, name.size());
				out.resize(151 + name.size());
				std::copy(name.begin(), name.end(), out.begin() + 150);
			}
		} else if (op == 31) {
			bool root = false;
			int fd    = open_name(state, in, 26, root);
			out.resize(139);
			try {
				attr(state, out, 10, fd);
			} catch (...) {
				close(fd);
				throw;
			}
			close(fd);
		} else if (op == 32) {
			if (!state.writable) {
				throw uint32_t(8);
			}
			const auto mask = get(in, 18, 8);
			const auto size = get(in, 30, 8);
			if (mask & ~(uint64_t(1 | 2 | 8 | 16 | 128 | 1024))) {
				throw uint32_t(13);
			}
			bool root = false;
			int fd    = -1;
			if (get(in, 134) & 1) {
				fd = open_name(state, in, 130, root);
			} else {
				Parent p(state, in, 130);
				fd = openat(p.fd,
				            p.name.c_str(),
				            ((mask & 2) ? O_RDWR : O_RDONLY) |
				                    O_NOFOLLOW | O_NONBLOCK |
				                    O_CLOEXEC);
			}
			if (fd < 0) {
				throw error();
			}
			try {
				if (root) {
					throw uint32_t(8);
				}
				check_mutable(state, fd);
				const auto dos_flags = get(in, 74, 8);
				if ((mask & 1024) && dos_flags > 7) {
					throw uint32_t(15);
				}
				if ((mask & 1)) {
					struct stat st = {};
					if (fstat(fd, &st)) {
						throw error();
					}
					if (get(in, 26) !=
					    (S_ISDIR(st.st_mode) ? 1u : 0u)) {
						throw uint32_t(15);
					}
				}
				// macOS requires write permission for extended
				// attributes. Restore it first when DOS removes
				// the read-only attribute.
				if ((mask & 128) && (get(in, 71, 1) & 2)) {
					struct stat st = {};
					if (fstat(fd, &st) ||
					    fchmod(fd, (st.st_mode & 0777) | 0200)) {
						throw error();
					}
				}
				if (mask & 1024) {
					set_dos_attributes(fd, uint8_t(dos_flags));
				}
				if (mask & 2) {
					if (size > INT64_MAX) {
						throw uint32_t(15);
					}
					if (ftruncate(fd, off_t(size))) {
						throw error();
					}
				}
				if (mask & 128) {
					struct stat st = {};
					if (fstat(fd, &st)) {
						throw error();
					}
					auto mode = st.st_mode & 0777;
					mode = (mode & ~0200) |
					       ((get(in, 71, 1) & 2) ? 0200 : 0);
					if (fchmod(fd, mode)) {
						throw error();
					}
				}
				if (mask & (8 | 16)) {
					timespec ts[2] = {
					        {0, UTIME_OMIT},
                                                {0, UTIME_OMIT}
                                        };
					for (int i = 0; i < 2; ++i) {
						if (mask & (i ? 16 : 8)) {
							auto t = get(in,
							             18 + (i ? 36 : 28),
							             8);
							ts[i].tv_sec = int64_t(t / 10000000) -
							               11644473600LL;
							ts[i].tv_nsec = long(t % 10000000) *
							                100;
						}
					}
					if (futimens(fd, ts)) {
						throw error();
					}
				}
			} catch (...) {
				close(fd);
				throw;
			}
			close(fd);
			out.resize(18);
		} else if (op >= 33 && op <= 36) {
			if (!state.writable) {
				throw uint32_t(8);
			}
			Parent p(state, in, op == 33 ? 30 : 26);
			check_confined(state, p.fd);
			int result = 0;
			if (op == 33) {
				result = mkdirat(p.fd, p.name.c_str(), 0700);
			} else if (op == 34 || op == 35) {
				result = unlinkat(p.fd,
				                  p.name.c_str(),
				                  op == 35 ? AT_REMOVEDIR : 0);
			} else {
				auto source_len = get(in, 26);
				Parent dst(state, in, 26 + 17 + source_len);
				check_confined(state, p.fd);
				check_confined(state, dst.fd);
#ifdef __APPLE__
				result = renameatx_np(p.fd,
				                      p.name.c_str(),
				                      dst.fd,
				                      dst.name.c_str(),
				                      RENAME_EXCL);
#else
				result = renameat2(p.fd,
				                   p.name.c_str(),
				                   dst.fd,
				                   dst.name.c_str(),
				                   RENAME_NOREPLACE);
#endif
			}
			if (result) {
				throw error();
			}
			out.resize(18);
		} else if (op == 37) {
			struct statvfs st = {};
			if (fstatvfs(state.root_fd, &st)) {
				throw error();
			}
			out.resize(34);
			put(out,
			    10,
			    state.writable ? uint64_t(st.f_bavail) * st.f_frsize : 0,
			    8);
			put(out, 18, uint64_t(st.f_blocks) * st.f_frsize, 8);
		} else {
			throw uint32_t(13);
		}
	} catch (uint32_t code) {
		out.resize(10);
		put(out, 6, code);
	} catch (...) {
		out.resize(10);
		put(out, 6, 10);
	}
	return out;
}
// A channel is bound once. File handles and reset/close belong to that channel,
// never to another mounted drive, even when both export the same host folder.
Bytes Request(const Bytes& in, uint16_t session)
{
	static const std::string select = "dosbox.hgfs.select ";
	if (in.size() >= select.size() &&
	    std::equal(select.begin(), select.end(), in.begin())) {
		const std::string name(in.begin() + select.size(), in.end());
		if (!session || sessions.count(session) || !valid_share(name) ||
		    !shares.count(upper(name)) || sessions.size() >= 32) {
			return {'0', ' '};
		}
		const auto& source = *shares.at(upper(name));
		auto target        = std::make_unique<State>();
		target->root_fd    = dup(source.root_fd);
		if (target->root_fd < 0) {
			return {'0', ' '};
		}
		target->writable  = source.writable;
		target->flat_root = true;
		sessions.emplace(session, std::move(target));
		return {'1', ' '};
	}
	if (!session) {
		return dispatch_request(default_state, in);
	} else {
		if (!sessions.count(session)) {
			if (sessions.size() >= 32) {
				return {'0', ' '};
			}
			auto target       = std::make_unique<State>();
			target->root_fd   = default_state.root_fd >= 0
			                          ? dup(default_state.root_fd)
			                          : -1;
			target->writable  = default_state.writable;
			target->flat_root = default_state.flat_root;
			sessions.emplace(session, std::move(target));
		}
	}
	return dispatch_request(*sessions.at(session), in);
}
Bytes Request(const Bytes& in)
{
	return Request(in, 0);
}
} // namespace VmwareHgfs

#endif
