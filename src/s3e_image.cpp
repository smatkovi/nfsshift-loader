#include "s3e_image.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <lzma.h>

#include "cpu.h"

namespace {
constexpr uint32_t S3E_MAGIC = 0x55334558;

struct Header {
    uint32_t magic, version, flags;
    uint32_t fixup_offset, fixup_size;
    uint32_t image_offset, image_size, mem_size;
    uint32_t sig_offset, sig_size;
    uint32_t entry;
    uint32_t config_offset, config_size;
    uint32_t base;
    uint32_t extra_offset, extra_size;
};

uint32_t rd32(const std::vector<uint8_t> &d, size_t off) {
    uint32_t v;
    memcpy(&v, d.data() + off, 4);
    return v;
}
uint16_t rd16(const std::vector<uint8_t> &d, size_t off) {
    uint16_t v;
    memcpy(&v, d.data() + off, 2);
    return v;
}

bool lzma_alone_decode(const std::vector<uint8_t> &in, std::vector<uint8_t> &out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) return false;
    out.resize(in.size() * 4);
    strm.next_in = in.data();
    strm.avail_in = in.size();
    strm.next_out = out.data();
    strm.avail_out = out.size();
    for (;;) {
        lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK && r != LZMA_BUF_ERROR) {
            lzma_end(&strm);
            return false;
        }
        if (strm.avail_out == 0) {
            size_t done = out.size();
            out.resize(out.size() * 2);
            strm.next_out = out.data() + done;
            strm.avail_out = out.size() - done;
        } else if (r == LZMA_BUF_ERROR) {
            // Streams without an end marker end when the input is consumed.
            break;
        }
    }
    out.resize(strm.total_out);
    lzma_end(&strm);
    return true;
}
}  // namespace

bool s3e_load(const std::string &path, S3eImage &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        logf("[s3e] cannot open %s", path.c_str());
        return false;
    }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() >= 4 && rd32(d, 0) != S3E_MAGIC) {
        std::vector<uint8_t> raw;
        if (!lzma_alone_decode(d, raw) || raw.size() < 0x48 || rd32(raw, 0) != S3E_MAGIC) {
            logf("[s3e] %s is not a (compressed) s3e image", path.c_str());
            return false;
        }
        d.swap(raw);
    }

    Header h;
    memcpy(&h, d.data(), sizeof(h));
    if (static_cast<uint64_t>(h.image_offset) + h.image_size > d.size() ||
        static_cast<uint64_t>(h.fixup_offset) + h.fixup_size > d.size() || h.mem_size < h.image_size) {
        logf("[s3e] corrupt header");
        return false;
    }
    logf("[s3e] version %#x base %#x image %#x mem %#x entry +%#x", h.version, h.base, h.image_size, h.mem_size,
         h.entry);

    // The image is linked to this address and cannot move: the relocation
    // section (type 1 below) is not applied, and the LAN patch tables in
    // src/mp/patches.c address the image directly. So a collision here is the
    // end -- but it must say so on screen: returning false used to end the app
    // without a word, which on a phone looks exactly like "it does not start".
    if (!guest::map_fixed(h.base, h.mem_size))
        fatal("the game image needs %#x-%#x, but that address range is already in use "
              "(the log lists what is there)",
              h.base, h.base + h.mem_size);
    memcpy(gptr(h.base), d.data() + h.image_offset, h.image_size);
    out.base = h.base;
    out.image_size = h.image_size;
    out.mem_size = h.mem_size;
    out.entry = h.base + h.entry;
    if (h.config_size && static_cast<uint64_t>(h.config_offset) + h.config_size <= d.size())
        out.config.assign(reinterpret_cast<const char *>(d.data() + h.config_offset),
                          strnlen(reinterpret_cast<const char *>(d.data() + h.config_offset), h.config_size));

    hle::map_stub_page();

    std::vector<std::string> names;
    size_t p = h.fixup_offset, end = h.fixup_offset + h.fixup_size;
    while (p + 8 <= end) {
        uint32_t type = rd32(d, p), size = rd32(d, p + 4);
        if (size < 8 || p + size > end) {
            logf("[s3e] bad fixup section at %#zx", p);
            return false;
        }
        switch (type) {
        case 0: {  // symbol names
            uint16_t count = rd16(d, p + 8);
            size_t q = p + 10;
            for (uint16_t i = 0; i < count; ++i) {
                const char *s = reinterpret_cast<const char *>(d.data() + q);
                size_t n = strnlen(s, end - q);
                names.emplace_back(s, n);
                q += n + 1;
            }
            break;
        }
        case 1:  // relocations: image is loaded at its link address, nothing to do
            break;
        case 2:    // ARM BL imports
        case 3:    // Thumb BL imports
        case 4: {  // data (address) imports
            uint32_t count = rd32(d, p + 8);
            for (uint32_t i = 0; i < count; ++i) {
                size_t e = p + 12 + 6 * i;
                uint32_t off = (static_cast<uint32_t>(rd16(d, e)) << 16) | rd16(d, e + 2);
                uint16_t idx = rd16(d, e + 4);
                if (idx >= names.size() || off + 4 > h.mem_size) {
                    logf("[s3e] bad import entry");
                    return false;
                }
                addr_t site = h.base + off;
                addr_t target = hle::stub_for(names[idx].c_str());
                if (type == 2) {
                    int64_t delta = static_cast<int64_t>(target) - (static_cast<int64_t>(site) + 8);
                    uint32_t insn = guest::read32(site);
                    guest::write32(site, (insn & 0xff000000) | ((static_cast<uint32_t>(delta) >> 2) & 0xffffff));
                } else if (type == 4) {
                    guest::write32(site, target);
                } else {
                    fatal("[s3e] thumb imports are not supported (%s)", names[idx].c_str());
                }
            }
            break;
        }
        default:
            logf("[s3e] unknown fixup section type %u", type);
            return false;
        }
        p += size;
    }
    out.imports = names;
    logf("[s3e] bound %zu imports", names.size());
    return true;
}
