#pragma once

#include <string>
#include <vector>

#include "guest.h"

struct S3eImage {
    addr_t base = 0;
    uint32_t image_size = 0;
    uint32_t mem_size = 0;
    addr_t entry = 0;
    std::string config;  // embedded ICF text
    std::vector<std::string> imports;
};

// Load an .s3e file (optionally LZMA "alone" compressed), map it at its link
// address and bind all imports to HLE stubs.
bool s3e_load(const std::string &path, S3eImage &out);
