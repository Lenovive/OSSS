#pragma once

#include <d3d11.h>
#include <winrt/base.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace osss {

struct GpuTimingStatistics {
    double p50_milliseconds = 0.0;
    double p95_milliseconds = 0.0;
    std::size_t sample_count = 0;
};

// A small non-blocking timestamp-query ring. It never flushes the immediate
// context and never waits for a result; a sample is simply omitted when the
// GPU has not retired it by the time Poll() runs. The rolling window is large
// enough to show a useful tail without turning telemetry into a second queue.
class GpuTimestampCollector {
public:
    GpuTimestampCollector(
        ID3D11Device* device,
        ID3D11DeviceContext* context,
        std::size_t query_capacity = 8);

    GpuTimestampCollector(const GpuTimestampCollector&) = delete;
    GpuTimestampCollector& operator=(const GpuTimestampCollector&) = delete;

    // Begin/End bracket commands on the same immediate context. Begin silently
    // declines to measure when every query slot is still in flight.
    void Begin() noexcept;
    void End() noexcept;
    void Cancel() noexcept;
    void Poll() noexcept;
    void Reset() noexcept;

    [[nodiscard]] bool Enabled() const noexcept;
    [[nodiscard]] GpuTimingStatistics Statistics() const noexcept;

private:
    struct QuerySet {
        winrt::com_ptr<ID3D11Query> disjoint;
        winrt::com_ptr<ID3D11Query> start;
        winrt::com_ptr<ID3D11Query> end;
        bool pending = false;
        bool cancelled = false;
    };

    static constexpr std::size_t kSampleWindow = 120;

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    std::vector<QuerySet> queries_;
    std::deque<double> samples_;
    std::size_t active_query_ = static_cast<std::size_t>(-1);
};

} // namespace osss
