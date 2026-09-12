// Small s3e subsystems: extensions, config, debug output, timers, sockets.
#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <strings.h>
#include <sys/time.h>
#include <vector>

#include "config.h"
#include "hle.h"
#include "runtime.h"

namespace {
using runtime::S3E_RESULT_ERROR;
using runtime::S3E_RESULT_SUCCESS;

uint32_t ext_hash(const char *name) {
    uint32_t h = 5381;
    for (const char *p = name; *p; ++p) h = h * 33 + static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(*p)));
    return h;
}

// --- extensions -------------------------------------------------------------
// Mirrors the MeeGo loader: s3eEval and s3eOSExec exist, everything else
// (notably s3eThread) is reported missing, which the game handles.
int32_t s3eEvalIsEvaluation() { return 0; }
int32_t s3eEvalUnknown() { return 0; }
int32_t s3eOSExecExecute(const char *url, uint8_t exit_after) {
    logf("[ext] s3eOSExecExecute(%s) ignored", url ? url : "");
    return S3E_RESULT_ERROR;
}
hle::AutoRegister reg_eval0("s3eEval_IsEvaluation", HLE_WRAP(s3eEvalIsEvaluation));
hle::AutoRegister reg_eval1("s3eEval_Unknown", HLE_WRAP(s3eEvalUnknown));
hle::AutoRegister reg_osexec("s3eOSExecExecute", HLE_WRAP(s3eOSExecExecute));

int32_t s3eExtGetHash(uint32_t hash, uint8_t *table, uint32_t size) {
    if (size == 0 && table) return S3E_RESULT_ERROR;
    std::vector<addr_t> funcs;
    const char *name = nullptr;
    if (hash == ext_hash("s3eEval") || hash == ext_hash("s3eExtEval")) {
        name = "s3eEval";
        funcs = {hle::stub_for("s3eEval_IsEvaluation"), hle::stub_for("s3eEval_Unknown")};
    } else if (hash == ext_hash("s3eOSExec") || hash == ext_hash("s3eExtOSExec")) {
        name = "s3eOSExec";
        funcs = {hle::stub_for("s3eOSExecExecute")};
    }
    if (!name) {
        static const char *known[] = {"s3eThread", "s3eVibra", "s3eDialog", "s3eLocation", "s3eSocketOpt",
                                      "s3eLibrary", "s3eDebugHeap", "s3eWebView", "s3eIOSGameCenter"};
        const char *guess = "?";
        for (const char *k : known)
            if (ext_hash(k) == hash) guess = k;
        static std::map<uint32_t, int> seen;
        if (!seen[hash]++) logf("[ext] extension %s (%#x, %u bytes) not available", guess, hash, size);
        return S3E_RESULT_ERROR;
    }
    if (size > funcs.size() * 4) return S3E_RESULT_ERROR;
    if (table) memcpy(table, funcs.data(), size);
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eExtGetHash);

int32_t s3eSocketClose(uint32_t socket) { return S3E_RESULT_ERROR; }
HLE_REGISTER(s3eSocketClose);

// --- config -----------------------------------------------------------------
int32_t s3eConfigGetInt(const char *group, const char *name, int32_t *value) {
    if (!value || !group || !name) return S3E_RESULT_ERROR;
    auto v = config::get(group, name);
    static const bool trace = getenv("NFS_TRACE_CONFIG") != nullptr;
    if (trace) logf("[config] GetInt [%s] %s -> %s", group, name, v ? v->c_str() : "(unset)");
    if (!v) return S3E_RESULT_ERROR;
    const char *p = v->c_str();
    while (*p == ' ' || *p == '\t') ++p;
    if (!strncasecmp(p, "true", 4) || !strncasecmp(p, "on", 2)) *value = 1;
    else if (!strncasecmp(p, "false", 5) || !strncasecmp(p, "off", 3)) *value = 0;
    else {
        char *end;
        long n = strtol(p, &end, 0);
        if (end == p) return S3E_RESULT_ERROR;
        *value = static_cast<int32_t>(n);
    }
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eConfigGetInt);

int32_t s3eConfigGetString(const char *group, const char *name, char *value) {
    if (!value || !group || !name) return S3E_RESULT_ERROR;
    auto v = config::get(group, name);
    static const bool trace = getenv("NFS_TRACE_CONFIG") != nullptr;
    if (trace) logf("[config] GetString [%s] %s -> %s", group, name, v ? v->c_str() : "(unset)");
    if (!v) return S3E_RESULT_ERROR;
    size_t n = std::min<size_t>(v->size(), 255);
    memcpy(value, v->data(), n);
    value[n] = 0;
    return S3E_RESULT_SUCCESS;
}
HLE_REGISTER(s3eConfigGetString);

// --- debug ------------------------------------------------------------------
void s3eDebugOutputString(const char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) --n;
    logf("[game] %.*s", static_cast<int>(n), s);
}
HLE_REGISTER(s3eDebugOutputString);

void s3eDebugPrint(int32_t x, int32_t y, const char *s, int32_t wrap) {
    static int count = 0;
    if (s && count++ < 50) logf("[game:print] %s", s);
}
HLE_REGISTER(s3eDebugPrint);

int32_t s3eDebugGetInt(int32_t property) { return 0; }
HLE_REGISTER(s3eDebugGetInt);

int32_t s3eDebugIsDebuggerPresent() { return 0; }
HLE_REGISTER(s3eDebugIsDebuggerPresent);

int32_t s3eDebugAssertShow(int32_t type, const char *s) {
    logf("[game:assert] %s (lr=%#x)", s ? s : "", Cpu::current().reg(14));
    return 0;  // continue
}
HLE_REGISTER(s3eDebugAssertShow);

int32_t s3eDebugErrorShow(int32_t type, const char *s) {
    logf("[game:error] %s (lr=%#x)", s ? s : "", Cpu::current().reg(14));
    return 0;  // continue
}
HLE_REGISTER(s3eDebugErrorShow);

// --- timers -----------------------------------------------------------------
uint64_t s3eTimerGetMs() { return runtime::now_ms(); }
HLE_REGISTER(s3eTimerGetMs);

uint64_t s3eTimerGetUTC() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    int64_t ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    // Every multiplayer timestamp in the game comes from here, so shifting
    // this clock is all it takes to put a LAN client on the host's time base.
    return static_cast<uint64_t>(ms + runtime::utc_offset_ms);
}
HLE_REGISTER(s3eTimerGetUTC);

}  // namespace
