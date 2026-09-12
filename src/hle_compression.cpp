// s3eCompression: stored / zlib / gzip / LZMA-alone decoding, streaming with a
// guest read callback or one-shot from memory.
#include <algorithm>
#include <lzma.h>
#include <zlib.h>

#include <cstring>
#include <functional>
#include <vector>

#include "hle.h"
#include "memory.h"
#include "runtime.h"

namespace {
using namespace runtime;

enum Type { AUTO = 0, STORED = 1, ZLIB = 2, GZIP = 3, LZMA = 4 };
constexpr int32_t ERR_DATA = 1000;
constexpr int32_t ERR_END = 1002;

using Source = std::function<int32_t(uint8_t *buf, uint32_t n)>;

struct Context {
    bool used = false;
    Type type = STORED;
    Source source;
    std::vector<uint8_t> in;  // pending input bytes
    size_t in_pos = 0;
    bool source_done = false;
    bool stream_end = false;
    int32_t error = 0;
    bool z_init = false;
    z_stream z{};
    bool lz_init = false;
    lzma_stream lz = LZMA_STREAM_INIT;
};

Context g_ctx[4];

// Fill ctx.in so that at least `min` bytes are pending (if the source allows).
void refill(Context &c, size_t min) {
    if (c.in_pos > 0 && c.in_pos == c.in.size()) {
        c.in.clear();
        c.in_pos = 0;
    }
    while (!c.source_done && c.in.size() - c.in_pos < min) {
        uint8_t buf[32768];
        int32_t n = c.source(buf, sizeof(buf));
        if (n <= 0) {
            c.source_done = true;
            break;
        }
        c.in.insert(c.in.end(), buf, buf + n);
        if (static_cast<uint32_t>(n) < sizeof(buf)) c.source_done = true;
    }
}

Type detect(Context &c) {
    refill(c, 13);
    const uint8_t *p = c.in.data() + c.in_pos;
    size_t n = c.in.size() - c.in_pos;
    if (n >= 2 && p[0] == 0x1f && p[1] == 0x8b) return GZIP;
    if (n >= 2 && (p[0] & 0xf) == 8 && (p[0] >> 4) < 8 && ((p[0] << 8) | p[1]) % 31 == 0 && !(p[1] & 0x20))
        return ZLIB;
    if (n >= 13 && p[0] <= 0xe1) {
        uint32_t dict = p[1] | (p[2] << 8) | (p[3] << 16) | (uint32_t(p[4]) << 24);
        if (dict < 0x900001 && !p[9] && !p[10] && !p[11] && !p[12]) return LZMA;
    }
    return STORED;
}

void release(Context &c) {
    if (c.z_init) inflateEnd(&c.z);
    if (c.lz_init) lzma_end(&c.lz);
    c = Context();
}

int free_slot() {
    for (int i = 0; i < 4; ++i)
        if (!g_ctx[i].used) return i;
    set_error(DEV_COMPRESSION, 2);
    return -1;
}

int32_t start(int slot, Type requested, Source source) {
    Context &c = g_ctx[slot];
    c = Context();
    c.used = true;
    c.source = std::move(source);
    Type found = detect(c);
    if (requested != AUTO && requested != found) {
        release(c);
        set_error(DEV_COMPRESSION, ERR_DATA);
        return 0;
    }
    c.type = found;
    if (found == ZLIB || found == GZIP) {
        if (inflateInit2(&c.z, 15 + 32) != Z_OK) {
            release(c);
            set_error(DEV_COMPRESSION, 6);
            return 0;
        }
        c.z_init = true;
    } else if (found == LZMA) {
        if (lzma_alone_decoder(&c.lz, UINT64_MAX) != LZMA_OK) {
            release(c);
            set_error(DEV_COMPRESSION, 6);
            return 0;
        }
        c.lz_init = true;
    }
    return slot + 1;
}

// Returns bytes produced, or -1 on error (error code in c.error).
int64_t produce(Context &c, uint8_t *out, uint32_t cap) {
    if (c.error) return -1;
    if (c.stream_end) return 0;
    uint32_t done = 0;
    switch (c.type) {
    case STORED:
        while (done < cap) {
            refill(c, 1);
            size_t avail = c.in.size() - c.in_pos;
            if (!avail) break;
            size_t k = std::min<size_t>(avail, cap - done);
            memcpy(out + done, c.in.data() + c.in_pos, k);
            c.in_pos += k;
            done += static_cast<uint32_t>(k);
        }
        if (done == 0) {
            c.error = ERR_END;
            return -1;
        }
        return done;
    case ZLIB:
    case GZIP:
        while (done < cap) {
            refill(c, 1);
            c.z.next_in = c.in.data() + c.in_pos;
            c.z.avail_in = static_cast<uInt>(c.in.size() - c.in_pos);
            c.z.next_out = out + done;
            c.z.avail_out = cap - done;
            int r = inflate(&c.z, Z_NO_FLUSH);
            c.in_pos = c.in.size() - c.z.avail_in;
            done = cap - c.z.avail_out;
            if (r == Z_STREAM_END) {
                c.stream_end = true;
                break;
            }
            if (r == Z_DATA_ERROR || r == Z_NEED_DICT) {
                c.error = ERR_DATA;
                return -1;
            }
            if (r == Z_BUF_ERROR && c.source_done && c.in_pos == c.in.size()) {
                if (done) return done;
                c.error = ERR_END;
                return -1;
            }
            if (r != Z_OK && r != Z_BUF_ERROR) {
                c.error = 6;
                return -1;
            }
        }
        return done;
    case LZMA:
        while (done < cap) {
            refill(c, 1);
            c.lz.next_in = c.in.data() + c.in_pos;
            c.lz.avail_in = c.in.size() - c.in_pos;
            c.lz.next_out = out + done;
            c.lz.avail_out = cap - done;
            lzma_ret r = lzma_code(&c.lz, c.source_done ? LZMA_FINISH : LZMA_RUN);
            c.in_pos = c.in.size() - c.lz.avail_in;
            done = cap - static_cast<uint32_t>(c.lz.avail_out);
            if (r == LZMA_STREAM_END) {
                c.stream_end = true;
                break;
            }
            if (r == LZMA_BUF_ERROR && c.source_done && c.in_pos == c.in.size()) {
                // No end marker: the stream ends with the input.
                c.stream_end = true;
                break;
            }
            if (r != LZMA_OK && r != LZMA_BUF_ERROR) {
                c.error = ERR_DATA;
                return -1;
            }
        }
        return done;
    default:
        return -1;
    }
}

Context *get(int32_t handle) {
    if (handle < 1 || handle > 4 || !g_ctx[handle - 1].used) {
        set_error(DEV_COMPRESSION, 1);
        return nullptr;
    }
    return &g_ctx[handle - 1];
}

// --- API ----------------------------------------------------------------------
int32_t s3eCompressionDecompInit(int32_t type, addr_t read_cb, addr_t user) {
    if (type < 0 || type > 4 || !read_cb) {
        set_error(DEV_COMPRESSION, 1);
        return 0;
    }
    int slot = free_slot();
    if (slot < 0) return 0;
    // The callback fills a DecompReq {buffer, size} with guest data; each
    // slot keeps its guest buffers for reuse.
    static addr_t reqs[4], buffers[4];
    if (!reqs[slot]) {
        reqs[slot] = guest::alloc(8);
        buffers[slot] = guest::alloc(32768);
    }
    addr_t req = reqs[slot], buffer = buffers[slot];
    Source src = [read_cb, user, req, buffer](uint8_t *buf, uint32_t n) -> int32_t {
        n = std::min<uint32_t>(n, 32768);
        guest::write32(req, buffer);
        guest::write32(req + 4, n);
        int32_t got = static_cast<int32_t>(Cpu::current().call(read_cb, {req, user}));
        if (got <= 0) return 0;
        got = std::min<int32_t>(got, static_cast<int32_t>(n));
        memcpy(buf, gptr(buffer), got);
        return got;
    };
    return start(slot, static_cast<Type>(type), std::move(src));
}
HLE_REGISTER(s3eCompressionDecompInit);

int32_t s3eCompressionDecompRead(int32_t handle, uint8_t *buf, uint32_t *len) {
    Context *c = get(handle);
    if (!c) return S3E_RESULT_ERROR;
    if (!buf || !len || !*len) {
        set_error(DEV_COMPRESSION, 1);
        return S3E_RESULT_ERROR;
    }
    int64_t n = produce(*c, buf, *len);
    if (n < 0) {
        *len = 0;
        set_error(DEV_COMPRESSION, c->error);
        return S3E_RESULT_ERROR;
    }
    *len = static_cast<uint32_t>(n);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eCompressionDecompRead);

int32_t s3eCompressionDecompFinal(int32_t handle) {
    Context *c = get(handle);
    if (!c) return S3E_RESULT_ERROR;
    release(*c);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eCompressionDecompFinal);

// Five arguments: the type is passed on the stack.
int32_t s3eCompressionDecomp(const uint8_t *src, uint32_t src_len, uint32_t *dst, uint32_t *dst_len, int32_t type) {
    if (!src || !dst || !src_len || type < 0 || type > 4 || (*dst && (!dst_len || !*dst_len))) {
        set_error(DEV_COMPRESSION, 1);
        return S3E_RESULT_ERROR;
    }
    size_t pos = 0;
    Source reader = [&](uint8_t *buf, uint32_t n) -> int32_t {
        uint32_t k = static_cast<uint32_t>(std::min<size_t>(n, src_len - pos));
        memcpy(buf, src + pos, k);
        pos += k;
        return static_cast<int32_t>(k);
    };
    int slot = free_slot();
    if (slot < 0) return S3E_RESULT_ERROR;
    int32_t handle = start(slot, static_cast<Type>(type), reader);
    if (!handle) return S3E_RESULT_ERROR;
    Context &c = g_ctx[handle - 1];

    int32_t result = S3E_RESULT_SUCCESS;
    if (*dst) {
        int64_t n = produce(c, gptr_t<uint8_t>(*dst), *dst_len);
        if (n < 0 && c.error != ERR_END) {
            set_error(DEV_COMPRESSION, c.error);
            *dst_len = 0;
            result = S3E_RESULT_ERROR;
        } else {
            *dst_len = n < 0 ? 0 : static_cast<uint32_t>(n);
        }
    } else {
        uint32_t cap = std::max<uint32_t>(src_len / 2 * 3, 64);
        addr_t out = memory::user_realloc(0, cap);
        uint32_t total = 0;
        while (out) {
            int64_t n = produce(c, gptr_t<uint8_t>(out + total), cap - total);
            if (n < 0) {
                if (c.error != ERR_END) result = S3E_RESULT_ERROR;
                break;
            }
            total += static_cast<uint32_t>(n);
            if (total < cap) break;
            cap = cap / 2 * 3;
            addr_t grown = memory::user_realloc(out, cap);
            if (!grown) {
                set_error(DEV_COMPRESSION, 8);
                result = S3E_RESULT_ERROR;
                break;
            }
            out = grown;
        }
        if (!out) {
            set_error(DEV_COMPRESSION, 8);
            result = S3E_RESULT_ERROR;
        }
        if (result == S3E_RESULT_SUCCESS) {
            addr_t shrunk = memory::user_realloc(out, std::max<uint32_t>(total, 1));
            *dst = shrunk ? shrunk : out;
            if (dst_len) *dst_len = total;
        } else {
            set_error(DEV_COMPRESSION, c.error ? c.error : 8);
            memory::user_free(out);
            *dst = 0;
            if (dst_len) *dst_len = 0;
        }
    }
    release(c);
    return result;
}
HLE_REGISTER(s3eCompressionDecomp);
}  // namespace
