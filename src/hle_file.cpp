// s3eFile: rom:// (game data, read-only), ram:// (saves), raw:// (host paths)
// and user file systems registered by the game (derbh archives).
#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "hle.h"
#include "runtime.h"

namespace {
using namespace runtime;

constexpr uint32_t kHandleBase = 1000;
constexpr int kMaxFiles = 32;

enum class Drive { None, Rom, Ram, Raw };
enum class Kind { Host, Memory, User };

struct File {
    bool used = false;
    Kind kind = Kind::Host;
    bool text = false;
    bool eof = false;
    bool writable = false;
    int pushback = -1;
    int fd = -1;
    addr_t mem = 0;
    uint32_t mem_size = 0, mem_pos = 0;
    int userfs = -1;
    uint32_t user_handle = 0;
};

struct UserFs {
    uint32_t fn[16];
};

std::recursive_mutex g_mutex;
File g_files[kMaxFiles];
std::vector<UserFs> g_userfs;

enum UserFn { U_OPEN = 0, U_READ = 1, U_SEEK = 3, U_TELL = 4, U_CLOSE = 5, U_LIST_START = 6, U_LIST_NEXT = 7,
              U_LIST_END = 8, U_WRITE = 9 };

// --- paths --------------------------------------------------------------------
struct ParsedPath {
    Drive drive = Drive::None;
    std::string rel;   // normalised, without prefix
    std::string full;  // normalised, with prefix (as given to user file systems)
    bool ok = false;
};

std::string normalise(const std::string &in) {
    std::string s = in;
    for (char &c : s)
        if (c == '\\') c = '/';
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find('/', i);
        if (j == std::string::npos) j = s.size();
        std::string part = s.substr(i, j - i);
        if (part.empty() || part == ".") {
        } else if (part == ".." && !parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else {
            parts.push_back(part);
        }
        i = j + 1;
    }
    std::string out;
    for (size_t k = 0; k < parts.size(); ++k) out += (k ? "/" : "") + parts[k];
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

ParsedPath parse(const char *path) {
    ParsedPath p;
    if (!path) return p;
    std::string s = path;
    static const std::pair<const char *, Drive> prefixes[] = {
        {"rom://", Drive::Rom}, {"ram://", Drive::Ram}, {"raw://", Drive::Raw}};
    for (auto &pr : prefixes)
        if (!strncasecmp(s.c_str(), pr.first, 6)) {
            p.drive = pr.second;
            s = s.substr(6);
            break;
        }
    if (p.drive == Drive::Raw) {
        p.rel = s;
        p.full = std::string("raw://") + s;
        p.ok = true;
        return p;
    }
    p.rel = normalise(s);
    if (p.rel.size() >= 128 || p.rel.compare(0, 3, "../") == 0 || p.rel == "..") {
        set_error(DEV_FILE, 11);
        return p;
    }
    p.full = p.drive == Drive::Rom ? "rom://" + p.rel : p.drive == Drive::Ram ? "ram://" + p.rel : p.rel;
    p.ok = true;
    return p;
}

// Resolve `rel` below `root` matching each component case-insensitively.
// Returns the host path of the longest match; `complete` tells whether all
// components exist.
std::string resolve_host(const std::string &root, const std::string &rel, bool &complete) {
    std::string cur = root;
    complete = true;
    size_t i = 0;
    while (i < rel.size()) {
        size_t j = rel.find('/', i);
        if (j == std::string::npos) j = rel.size();
        std::string part = rel.substr(i, j - i);
        std::string candidate = cur + "/" + part;
        struct stat st;
        if (stat(candidate.c_str(), &st) != 0) {
            bool found = false;
            if (DIR *d = opendir(cur.c_str())) {
                while (dirent *e = readdir(d))
                    if (!strcasecmp(e->d_name, part.c_str())) {
                        candidate = cur + "/" + e->d_name;
                        found = true;
                        break;
                    }
                closedir(d);
            }
            if (!found) {
                complete = false;
                return cur + "/" + rel.substr(i);
            }
        }
        cur = candidate;
        i = j + 1;
    }
    return cur;
}

const std::string &root_of(Drive d) { return d == Drive::Ram ? paths().user : paths().data; }

bool host_is_file(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}
bool host_is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string host_path(Drive d, const ParsedPath &p, bool &exists) {
    if (d == Drive::Raw) {
        exists = access(p.rel.c_str(), F_OK) == 0;
        return p.rel;
    }
    return resolve_host(root_of(d), p.rel, exists);
}

// --- user file systems --------------------------------------------------------
uint32_t ucall(int fs, UserFn fn, std::initializer_list<uint32_t> args) {
    return Cpu::current().call(g_userfs[fs].fn[fn], args);
}

bool user_is_file(int fs, const std::string &full) {
    addr_t path = guest::intern(full);
    uint32_t h = ucall(fs, U_OPEN, {path, guest::intern("rb")});
    if (!h) return false;
    ucall(fs, U_CLOSE, {h});
    return true;
}

bool user_is_dir(int fs, const std::string &full) {
    uint32_t lh = ucall(fs, U_LIST_START, {guest::intern(full)});
    if (!lh) return false;
    addr_t buf = guest::alloc(0x80);
    uint32_t r = ucall(fs, U_LIST_NEXT, {lh, buf, 0x80});
    guest::release(buf);
    ucall(fs, U_LIST_END, {lh});
    return r == 0;
}

// --- handles ------------------------------------------------------------------
File *get(uint32_t handle) {
    uint32_t idx = handle - kHandleBase;
    if (idx >= kMaxFiles || !g_files[idx].used) {
        set_error(DEV_FILE, 1);
        return nullptr;
    }
    return &g_files[idx];
}

int alloc_slot() {
    for (int i = 0; i < kMaxFiles; ++i)
        if (!g_files[i].used) return i;
    return -1;
}

int64_t raw_seek(File &f, int64_t off, int origin) {
    switch (f.kind) {
    case Kind::Host: return lseek(f.fd, off, origin);
    case Kind::Memory: {
        int64_t base = origin == 0 ? 0 : origin == 1 ? f.mem_pos : f.mem_size;
        int64_t pos = std::clamp<int64_t>(base + off, 0, f.mem_size);
        f.mem_pos = static_cast<uint32_t>(pos);
        return pos;
    }
    case Kind::User:
        if (ucall(f.userfs, U_SEEK, {f.user_handle, static_cast<uint32_t>(off), static_cast<uint32_t>(origin)}) != 0)
            return -1;
        return static_cast<int32_t>(ucall(f.userfs, U_TELL, {f.user_handle}));
    }
    return -1;
}

int64_t raw_read(File &f, void *buf, uint32_t n) {
    switch (f.kind) {
    case Kind::Host: {
        int64_t total = 0;
        while (total < n) {
            ssize_t r = read(f.fd, static_cast<char *>(buf) + total, n - total);
            if (r <= 0) break;
            total += r;
        }
        return total;
    }
    case Kind::Memory: {
        uint32_t k = std::min(n, f.mem_size - f.mem_pos);
        memcpy(buf, gptr(f.mem + f.mem_pos), k);
        f.mem_pos += k;
        return k;
    }
    case Kind::User: {
        // The callback needs a guest buffer; host buffers are bounced.
        if (reinterpret_cast<uintptr_t>(buf) >> 32) {
            addr_t bounce = guest::alloc(n);
            int32_t r = static_cast<int32_t>(ucall(f.userfs, U_READ, {bounce, 1, n, f.user_handle}));
            if (r > 0) memcpy(buf, gptr(bounce), r);
            guest::release(bounce);
            return r;
        }
        return static_cast<int32_t>(ucall(f.userfs, U_READ, {gaddr(buf), 1, n, f.user_handle}));
    }
    }
    return 0;
}

// Read with text-mode CRLF -> LF conversion.
int64_t file_read(File &f, char *buf, uint32_t n) {
    if (!f.text) {
        int64_t got = 0;
        if (f.pushback >= 0 && n) {
            buf[got++] = static_cast<char>(f.pushback);
            f.pushback = -1;
        }
        return got + raw_read(f, buf + got, n - static_cast<uint32_t>(got));
    }
    uint32_t out = 0;
    while (out < n) {
        int c;
        if (f.pushback >= 0) {
            c = f.pushback;
            f.pushback = -1;
        } else {
            char ch;
            if (raw_read(f, &ch, 1) != 1) break;
            c = static_cast<unsigned char>(ch);
        }
        if (c == '\r') {
            char next;
            if (raw_read(f, &next, 1) == 1) {
                if (next == '\n') c = '\n';
                else f.pushback = static_cast<unsigned char>(next);
            }
        }
        buf[out++] = static_cast<char>(c);
    }
    return out;
}

// --- API ----------------------------------------------------------------------
uint32_t s3eFileOpen(const char *path, const char *mode) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!mode || !path) {
        set_error(DEV_FILE, 1);
        return 0;
    }
    std::string m = mode;
    bool plus = m.find('+') != std::string::npos;
    char op = static_cast<char>(tolower(m[0]));
    if (op != 'r' && op != 'w' && op != 'a') {
        set_error(DEV_FILE, 1000);
        return 0;
    }
    bool text = m.find('b') == std::string::npos;
    bool write = op != 'r' || plus;
    bool skip_user = m.find('U') != std::string::npos;
    ParsedPath p = parse(path);
    if (!p.ok) return 0;
    int slot = alloc_slot();
    if (slot < 0) {
        set_error(DEV_FILE, 2);
        return 0;
    }
    File f;
    f.text = text;
    f.writable = write;

    // User file systems first (read access only).
    if (p.drive != Drive::Raw && !write && !skip_user) {
        for (int fs = static_cast<int>(g_userfs.size()) - 1; fs >= 0; --fs) {
            addr_t gpath = guest::intern(p.full);
            uint32_t h = ucall(fs, U_OPEN, {gpath, guest::intern("rb")});
            if (h) {
                f.kind = Kind::User;
                f.userfs = fs;
                f.user_handle = h;
                f.used = true;
                g_files[slot] = f;
                return kHandleBase + slot;
            }
        }
    }

    std::string host;
    if (p.drive == Drive::Raw) {
        host = p.rel;
    } else if (write) {
        if (p.drive == Drive::Rom) {
            set_error(DEV_FILE, 1003);
            return 0;
        }
        bool exists_ram;
        host = host_path(Drive::Ram, p, exists_ram);
        if (!exists_ram && p.drive == Drive::None) {
            // Copy-on-write from rom://.
            bool exists_rom;
            std::string rom = host_path(Drive::Rom, p, exists_rom);
            std::string dir = host.substr(0, host.rfind('/'));
            if (exists_rom || host_is_dir(rom.substr(0, rom.rfind('/')))) mkdir(dir.c_str(), 0755);
            if (exists_rom && host_is_file(rom) && op != 'w') {
                int in = open(rom.c_str(), O_RDONLY), outfd = open(host.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                char buf[65536];
                ssize_t r;
                while (in >= 0 && outfd >= 0 && (r = read(in, buf, sizeof(buf))) > 0) (void)!::write(outfd, buf, r);
                if (in >= 0) close(in);
                if (outfd >= 0) close(outfd);
            }
        }
    } else {
        bool exists = false;
        if (p.drive != Drive::Rom) host = host_path(Drive::Ram, p, exists);
        if (!exists || !host_is_file(host)) {
            exists = false;
            if (p.drive != Drive::Ram) host = host_path(Drive::Rom, p, exists);
        }
        if (!exists || !host_is_file(host)) {
            set_error(DEV_FILE, 4);
            return 0;
        }
    }

    int flags = op == 'r' ? (plus ? O_RDWR : O_RDONLY)
                : op == 'w' ? ((plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC)
                            : ((plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND);
    int fd = open(host.c_str(), flags | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(DEV_FILE, errno == ENOENT || errno == ENOTDIR ? 4 : errno == EACCES ? 1003 : 6);
        return 0;
    }
    f.kind = Kind::Host;
    f.fd = fd;
    f.used = true;
    g_files[slot] = f;
    return kHandleBase + slot;
}
HLE_REGISTER(s3eFileOpen);

uint32_t s3eFileOpenFromMemory(addr_t buf, uint32_t size) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!buf) {
        set_error(DEV_FILE, 1);
        return 0;
    }
    int slot = alloc_slot();
    if (slot < 0) {
        set_error(DEV_FILE, 2);
        return 0;
    }
    File f;
    f.kind = Kind::Memory;
    f.mem = buf;
    f.mem_size = size;
    f.writable = true;
    f.used = true;
    g_files[slot] = f;
    return kHandleBase + slot;
}
HLE_REGISTER(s3eFileOpenFromMemory);

int32_t s3eFileClose(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return S3E_RESULT_ERROR;
    if (f->kind == Kind::Host) close(f->fd);
    else if (f->kind == Kind::User) ucall(f->userfs, U_CLOSE, {f->user_handle});
    *f = File();
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eFileClose);

uint32_t s3eFileRead(char *buf, uint32_t elem, uint32_t count, uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return 0;
    if (!buf) {
        set_error(DEV_FILE, 1);
        return 0;
    }
    if (!elem || !count) return 0;
    uint64_t want = uint64_t(elem) * count;
    if (want > 0x7fffffff) want = 0x7fffffff;
    int64_t got = file_read(*f, buf, static_cast<uint32_t>(want));
    if (got < 0) got = 0;
    if (static_cast<uint64_t>(got) < want) {
        f->eof = true;
        set_error(DEV_FILE, 1005);
    }
    return static_cast<uint32_t>(got / elem);
}
HLE_REGISTER(s3eFileRead);

uint32_t s3eFileWrite(const char *buf, uint32_t elem, uint32_t count, uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return 0;
    if (!buf) {
        set_error(DEV_FILE, 1);
        return 0;
    }
    uint64_t n = uint64_t(elem) * count;
    if (!n) return 0;
    f->pushback = -1;
    bool ok = false;
    switch (f->kind) {
    case Kind::Host: {
        uint64_t done = 0;
        while (done < n) {
            ssize_t w = write(f->fd, buf + done, n - done);
            if (w <= 0) break;
            done += w;
        }
        ok = done == n;
        break;
    }
    case Kind::Memory:
        if (f->mem_pos + n <= f->mem_size) {
            memcpy(gptr(f->mem + f->mem_pos), buf, n);
            f->mem_pos += static_cast<uint32_t>(n);
            ok = true;
        }
        break;
    case Kind::User:
        if (!g_userfs[f->userfs].fn[U_WRITE]) {
            set_error(DEV_FILE, 7);
            return 0;
        }
        ok = ucall(f->userfs, U_WRITE, {gaddr(buf), 1, static_cast<uint32_t>(n), f->user_handle}) == n;
        break;
    }
    if (!ok) {
        set_error(DEV_FILE, 12);
        return 0;
    }
    return count;
}
HLE_REGISTER(s3eFileWrite);

int32_t s3eFileGetChar(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return -1;
    char c;
    if (file_read(*f, &c, 1) != 1) {
        f->eof = true;
        set_error(DEV_FILE, 1005);
        return -1;
    }
    return static_cast<unsigned char>(c);
}
HLE_REGISTER(s3eFileGetChar);

int32_t s3eFileSeek(uint32_t handle, int32_t offset, int32_t origin) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return S3E_RESULT_ERROR;
    if (origin < 0 || origin > 2) {
        set_error(DEV_FILE, 1);
        return S3E_RESULT_ERROR;
    }
    if (f->pushback >= 0 && origin == 1) offset -= 1;
    f->pushback = -1;
    if (raw_seek(*f, offset, origin) < 0) {
        set_error(DEV_FILE, 6);
        return S3E_RESULT_ERROR;
    }
    f->eof = false;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eFileSeek);

int32_t s3eFileTell(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return -1;
    int64_t pos = f->kind == Kind::Memory ? f->mem_pos
                  : f->kind == Kind::Host ? lseek(f->fd, 0, SEEK_CUR)
                                          : static_cast<int32_t>(ucall(f->userfs, U_TELL, {f->user_handle}));
    if (pos >= 0 && f->pushback >= 0) pos -= 1;
    return static_cast<int32_t>(pos);
}
HLE_REGISTER(s3eFileTell);

int32_t s3eFileGetSize(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    if (!f) return -1;
    switch (f->kind) {
    case Kind::Host: {
        struct stat st;
        return fstat(f->fd, &st) == 0 ? static_cast<int32_t>(st.st_size) : -1;
    }
    case Kind::Memory: return static_cast<int32_t>(f->mem_size);
    case Kind::User: {
        uint32_t h = f->user_handle;
        int32_t cur = static_cast<int32_t>(ucall(f->userfs, U_TELL, {h}));
        ucall(f->userfs, U_SEEK, {h, 0, 2});
        int32_t size = static_cast<int32_t>(ucall(f->userfs, U_TELL, {h}));
        ucall(f->userfs, U_SEEK, {h, static_cast<uint32_t>(cur), 0});
        return size;
    }
    }
    return -1;
}
HLE_REGISTER(s3eFileGetSize);

uint8_t s3eFileEOF(uint32_t handle) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    File *f = get(handle);
    return !f || f->eof ? 1 : 0;
}
HLE_REGISTER(s3eFileEOF);

// Existence checks in the same order as opening for read.
enum class Found { None, User, Ram, Rom, Raw };

Found find(const ParsedPath &p, bool want_dir, std::string *host_out) {
    if (p.drive == Drive::Raw) {
        bool ok = want_dir ? host_is_dir(p.rel) : host_is_file(p.rel);
        if (ok && host_out) *host_out = p.rel;
        return ok ? Found::Raw : Found::None;
    }
    for (int fs = static_cast<int>(g_userfs.size()) - 1; fs >= 0; --fs)
        if (want_dir ? user_is_dir(fs, p.full) : user_is_file(fs, p.full)) return Found::User;
    for (Drive d : {Drive::Ram, Drive::Rom}) {
        if (p.drive != Drive::None && p.drive != d) continue;
        bool exists;
        std::string h = host_path(d, p, exists);
        if (exists && (want_dir ? host_is_dir(h) : host_is_file(h))) {
            if (host_out) *host_out = h;
            return d == Drive::Ram ? Found::Ram : Found::Rom;
        }
    }
    return Found::None;
}

uint8_t s3eFileCheckExists(const char *path) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ParsedPath p = parse(path);
    return p.ok && find(p, false, nullptr) != Found::None ? 1 : 0;
}
HLE_REGISTER(s3eFileCheckExists);

int32_t s3eFileMakeDirectory(const char *path) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ParsedPath p = parse(path);
    if (!p.ok) return S3E_RESULT_ERROR;
    if (p.drive == Drive::Rom) {
        set_error(DEV_FILE, 1003);
        return S3E_RESULT_ERROR;
    }
    bool exists;
    std::string host = p.drive == Drive::Raw ? p.rel : host_path(Drive::Ram, p, exists);
    if (mkdir(host.c_str(), 0770) != 0) {
        set_error(DEV_FILE, errno == EEXIST ? 1001 : errno == ENOENT ? 4 : 9);
        return S3E_RESULT_ERROR;
    }
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eFileMakeDirectory);

int32_t s3eFileGetError() { return take_error(DEV_FILE); }
HLE_REGISTER(s3eFileGetError);

int64_t s3eFileGetFileInt(const char *path, int32_t prop) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ParsedPath p = parse(path);
    if (!p.ok) {
        set_error(DEV_FILE, 1);
        return -1;
    }
    std::string host;
    switch (prop) {
    case 0: return find(p, false, nullptr) != Found::None ? 1 : 0;
    case 1: return find(p, true, nullptr) != Found::None ? 1 : 0;
    case 2: return find(p, false, nullptr) == Found::User ? 1 : 0;
    case 3: {
        Found fnd = find(p, false, nullptr);
        if (fnd == Found::None) {
            set_error(DEV_FILE, 4);
            return -1;
        }
        return fnd == Found::Ram || fnd == Found::Raw || (fnd == Found::Rom && p.drive == Drive::None) ? 1 : 0;
    }
    case 4: {
        Found fnd = find(p, false, &host);
        if (fnd == Found::None) {
            set_error(DEV_FILE, 4);
            return -1;
        }
        if (fnd == Found::User) {
            uint32_t h = s3eFileOpen(path, "rb");
            int32_t size = h ? s3eFileGetSize(h) : -1;
            if (h) s3eFileClose(h);
            return size;
        }
        struct stat st;
        return stat(host.c_str(), &st) == 0 ? st.st_size : -1;
    }
    case 5: {
        Found fnd = find(p, false, &host);
        if (fnd == Found::None) return -1;
        if (fnd == Found::User) return 0;
        struct stat st;
        return stat(host.c_str(), &st) == 0 ? int64_t(st.st_mtime) * 1000 : -1;
    }
    case 6:
    case 8: {
        struct statvfs vfs;
        if (statvfs(paths().user.c_str(), &vfs) != 0) return 0;
        return prop == 6 ? int64_t(vfs.f_bavail) * vfs.f_frsize : int64_t(vfs.f_blocks) * vfs.f_frsize;
    }
    default: set_error(DEV_FILE, 1); return -1;
    }
}
HLE_REGISTER(s3eFileGetFileInt);

int32_t s3eFileAddUserFileSys(const uint32_t *fs) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    if (!fs) {
        set_error(DEV_FILE, 1);
        return S3E_RESULT_ERROR;
    }
    for (int i = 0; i <= 8; ++i)
        if (!fs[i]) {
            set_error(DEV_FILE, 1);
            return S3E_RESULT_ERROR;
        }
    if (g_userfs.size() >= 4) {
        set_error(DEV_FILE, 2);
        return S3E_RESULT_ERROR;
    }
    UserFs u;
    memcpy(u.fn, fs, sizeof(u.fn));
    g_userfs.push_back(u);
    logf("[file] user file system %zu registered (open=%#x)", g_userfs.size() - 1, fs[0]);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eFileAddUserFileSys);
}  // namespace

namespace files {
bool host_path_for_read(const char *path, std::string &out) {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    ParsedPath p = parse(path);
    if (!p.ok) return false;
    Found f = find(p, false, &out);
    return f == Found::Ram || f == Found::Rom || f == Found::Raw;
}
}  // namespace files
