#include "shader_cache.h"

#include <windows.h>

#include <d3dcompiler.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace osss {
namespace {

// Bump when the on-disk record layout changes so old files are ignored rather
// than misparsed. The version is part of the key, so a bump simply misses.
constexpr std::uint32_t kCacheFormatVersion = 1;

// FNV-1a over the whole key. A 64-bit digest over a handful of shaders per
// build has no practical collision risk, and a wrong hit would surface
// immediately as a shader that fails to create rather than as silent corruption.
class Fnv1a {
public:
    void Append(const void* const data, const std::size_t size) noexcept {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t index = 0; index < size; ++index) {
            hash_ ^= static_cast<std::uint64_t>(bytes[index]);
            hash_ *= 0x0000'0100'0000'01B3ULL;
        }
    }

    void Append(const std::string_view text) noexcept {
        Append(text.data(), text.size());
    }

    [[nodiscard]] std::uint64_t Value() const noexcept {
        return hash_;
    }

private:
    std::uint64_t hash_ = 0xCBF2'9CE4'8422'2325ULL;
};

[[nodiscard]] std::wstring HexDigest(const std::uint64_t value) {
    static constexpr wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring text(16, L'0');
    for (int index = 15; index >= 0; --index) {
        text[static_cast<std::size_t>(index)] =
            kDigits[(value >> ((15 - index) * 4)) & 0xFULL];
    }
    return text;
}

// The cache lives beside other per-user application state rather than next to
// the executable: the install directory is frequently read-only, and a failed
// write there would silently cost every start the full compile.
[[nodiscard]] std::wstring ResolveCacheDirectory() {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local_app_data)) ||
        !local_app_data) {
        if (local_app_data) {
            CoTaskMemFree(local_app_data);
        }
        return {};
    }
    std::wstring directory(local_app_data);
    CoTaskMemFree(local_app_data);

    directory += L"\\OSSS";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    directory += L"\\shadercache";
    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return {};
    }
    return directory;
}

[[nodiscard]] std::wstring CachePathFor(
    const ShaderCompileRequest& request,
    const std::uint32_t flags) {
    static const std::wstring directory = ResolveCacheDirectory();
    if (directory.empty()) {
        return {};
    }

    Fnv1a digest;
    digest.Append(&kCacheFormatVersion, sizeof(kCacheFormatVersion));
    digest.Append(&flags, sizeof(flags));
    digest.Append(request.entry_point ? request.entry_point : "");
    digest.Append("\0", 1);
    digest.Append(request.profile ? request.profile : "");
    digest.Append("\0", 1);
    digest.Append(request.source);
    // Length is hashed separately so a source that differs only by a trailing
    // run cannot alias a shorter one under the byte loop above.
    const std::uint64_t length = request.source.size();
    digest.Append(&length, sizeof(length));

    return directory + L"\\" + HexDigest(digest.Value()) + L".cso";
}

[[nodiscard]] winrt::com_ptr<ID3DBlob> ReadCachedBytecode(const std::wstring& path) {
    if (path.empty()) {
        return nullptr;
    }
    const HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return nullptr;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 64LL * 1024LL * 1024LL) {
        CloseHandle(file);
        return nullptr;
    }

    winrt::com_ptr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size.QuadPart), blob.put()))) {
        CloseHandle(file);
        return nullptr;
    }

    auto* cursor = static_cast<unsigned char*>(blob->GetBufferPointer());
    DWORD remaining = static_cast<DWORD>(size.QuadPart);
    while (remaining > 0) {
        DWORD read = 0;
        if (!ReadFile(file, cursor, remaining, &read, nullptr) || read == 0) {
            CloseHandle(file);
            return nullptr;
        }
        cursor += read;
        remaining -= read;
    }
    CloseHandle(file);
    return blob;
}

// Written to a unique temporary name and renamed into place. Two OSSS processes
// starting together therefore never observe a half-written .cso, and the loser
// of the race simply overwrites an identical file.
void WriteCachedBytecode(const std::wstring& path, ID3DBlob* const bytecode) {
    if (path.empty() || !bytecode) {
        return;
    }
    const std::wstring temporary =
        path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    const auto* cursor = static_cast<const unsigned char*>(bytecode->GetBufferPointer());
    DWORD remaining = static_cast<DWORD>(bytecode->GetBufferSize());
    bool wrote_everything = true;
    while (remaining > 0) {
        DWORD written = 0;
        if (!WriteFile(file, cursor, remaining, &written, nullptr) || written == 0) {
            wrote_everything = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    CloseHandle(file);

    if (!wrote_everything ||
        !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
    }
}

[[nodiscard]] winrt::com_ptr<ID3DBlob> CompileOne(
    const ShaderCompileRequest& request,
    const std::uint32_t flags,
    const char* const error_prefix) {
    winrt::com_ptr<ID3DBlob> bytecode;
    winrt::com_ptr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        request.source.data(),
        request.source.size(),
        nullptr,
        nullptr,
        nullptr,
        request.entry_point,
        request.profile,
        flags,
        0,
        bytecode.put(),
        errors.put());
    if (FAILED(result)) {
        std::string message = error_prefix ? error_prefix : "Shader compilation failed for ";
        message.append(request.entry_point ? request.entry_point : "<unknown entry point>");
        if (errors) {
            message.append(": ");
            message.append(
                static_cast<const char*>(errors->GetBufferPointer()),
                errors->GetBufferSize());
        }
        throw std::runtime_error(message);
    }
    return bytecode;
}

} // namespace

std::wstring ShaderCacheDirectory() {
    return ResolveCacheDirectory();
}

ShaderCompileResult CompileShaderCached(
    const ShaderCompileRequest& request,
    const std::uint32_t flags,
    const char* const error_prefix) {
    const std::wstring path = CachePathFor(request, flags);
    if (auto cached = ReadCachedBytecode(path)) {
        return ShaderCompileResult{std::move(cached), ShaderCacheOutcome::hit};
    }

    auto bytecode = CompileOne(request, flags, error_prefix);
    WriteCachedBytecode(path, bytecode.get());
    return ShaderCompileResult{std::move(bytecode), ShaderCacheOutcome::compiled};
}

std::vector<ShaderCompileResult> CompileShadersCached(
    const std::vector<ShaderCompileRequest>& requests,
    const std::uint32_t flags,
    const char* const error_prefix) {
    std::vector<ShaderCompileResult> results(requests.size());
    if (requests.empty()) {
        return results;
    }
    if (requests.size() == 1) {
        results[0] = CompileShaderCached(requests[0], flags, error_prefix);
        return results;
    }

    std::vector<std::exception_ptr> failures(requests.size());

    // One worker per shader, capped so a large batch on a small machine does not
    // oversubscribe. The compile is CPU bound and each one is seconds long, so
    // there is nothing to gain from a finer-grained pool.
    const unsigned int hardware = std::max(1U, std::thread::hardware_concurrency());
    const std::size_t worker_count =
        std::min<std::size_t>(requests.size(), std::max<std::size_t>(1, hardware));

    std::mutex next_mutex;
    std::size_t next_index = 0;
    const auto run = [&]() noexcept {
        for (;;) {
            std::size_t index = 0;
            {
                const std::lock_guard<std::mutex> lock(next_mutex);
                if (next_index >= requests.size()) {
                    return;
                }
                index = next_index++;
            }
            try {
                results[index] = CompileShaderCached(requests[index], flags, error_prefix);
            } catch (...) {
                failures[index] = std::current_exception();
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count - 1);
    for (std::size_t worker = 1; worker < worker_count; ++worker) {
        workers.emplace_back(run);
    }
    run();
    for (auto& worker : workers) {
        worker.join();
    }

    // Rethrow only after every worker has joined, and report the first failure
    // in request order so the message does not depend on thread scheduling.
    for (const auto& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    return results;
}

} // namespace osss
