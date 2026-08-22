#pragma once

#include <d3dcommon.h>
#include <winrt/base.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace osss {

// Runtime HLSL compilation is the dominant cost of starting OSSS. Every motion
// shader is a different entry point into one ~35 KB translation unit, so a naive
// loop re-parses and re-optimizes that whole unit once per entry point at
// D3DCOMPILE_OPTIMIZATION_LEVEL3. Measured cold on an RTX 5090: ~12 s before the
// overlay could show anything.
//
// Two independent fixes live here, and they compose:
//   * a content-addressed on-disk bytecode cache, so only the first run after a
//     shader edit pays anything at all;
//   * batch compilation across threads, so that first run pays once for the
//     slowest shader rather than once per shader.
//
// The cache key covers the source text, the entry point, the profile, and the
// compile flags. It deliberately does *not* cover the GPU: DXBC is device
// independent, and the driver's own pipeline cache handles the DXBC-to-ISA step.
struct ShaderCompileRequest {
    std::string_view source;
    const char* entry_point = nullptr;
    const char* profile = nullptr;
};

// Where the cached bytecode came from. Startup telemetry reports this so a slow
// cold start is distinguishable from a cache that is silently never hitting.
enum class ShaderCacheOutcome {
    hit,
    compiled,
};

struct ShaderCompileResult {
    winrt::com_ptr<ID3DBlob> bytecode;
    ShaderCacheOutcome outcome = ShaderCacheOutcome::compiled;
};

// Directory holding the cached bytecode, created on demand. Empty when no
// per-user application-data path is available, which disables caching without
// disabling compilation.
[[nodiscard]] std::wstring ShaderCacheDirectory();

// Compile one shader, consulting the on-disk cache first. `error_prefix` is used
// to build the exception message so callers keep their existing diagnostics.
// Throws std::runtime_error on a compilation failure.
[[nodiscard]] ShaderCompileResult CompileShaderCached(
    const ShaderCompileRequest& request,
    std::uint32_t flags,
    const char* error_prefix);

// Compile a batch concurrently and return results in request order.
//
// Concurrency is safe because d3dcompiler_47's D3DCompile is free-threaded; the
// cache reads and writes are content-addressed and each entry is published with
// an atomic rename, so two processes racing on a cold cache both win.
//
// A failure in any request is rethrown after every thread has joined, so a
// partially failed batch never leaves a detached thread touching freed memory.
[[nodiscard]] std::vector<ShaderCompileResult> CompileShadersCached(
    const std::vector<ShaderCompileRequest>& requests,
    std::uint32_t flags,
    const char* error_prefix);

} // namespace osss
