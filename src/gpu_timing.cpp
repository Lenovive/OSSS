#include "gpu_timing.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace osss {
namespace {

constexpr DWORD kDoNotFlush = D3D11_ASYNC_GETDATA_DONOTFLUSH;

template <typename T>
HRESULT ReadQuery(
    ID3D11DeviceContext* const context,
    ID3D11Query* const query,
    T& value) noexcept {
    return context->GetData(query, &value, sizeof(value), kDoNotFlush);
}

} // namespace

GpuTimestampCollector::GpuTimestampCollector(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    const std::size_t query_capacity)
    : device_(device),
      context_(context) {
    if (!device_ || !context_ || query_capacity == 0) {
        return;
    }

    try {
        queries_.resize(query_capacity);
        for (auto& query : queries_) {
            D3D11_QUERY_DESC disjoint_description{};
            disjoint_description.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            winrt::check_hresult(device_->CreateQuery(
                &disjoint_description,
                query.disjoint.put()));

            D3D11_QUERY_DESC timestamp_description{};
            timestamp_description.Query = D3D11_QUERY_TIMESTAMP;
            winrt::check_hresult(device_->CreateQuery(
                &timestamp_description,
                query.start.put()));
            winrt::check_hresult(device_->CreateQuery(
                &timestamp_description,
                query.end.put()));
        }
    } catch (...) {
        queries_.clear();
    }
}

void GpuTimestampCollector::Begin() noexcept {
    if (active_query_ != static_cast<std::size_t>(-1) || queries_.empty()) {
        return;
    }
    Poll();
    for (std::size_t index = 0; index < queries_.size(); ++index) {
        auto& query = queries_[index];
        if (query.pending) {
            continue;
        }
        context_->Begin(query.disjoint.get());
        context_->End(query.start.get());
        query.cancelled = false;
        active_query_ = index;
        return;
    }
}

void GpuTimestampCollector::End() noexcept {
    if (active_query_ == static_cast<std::size_t>(-1)) {
        return;
    }
    auto& query = queries_[active_query_];
    context_->End(query.end.get());
    context_->End(query.disjoint.get());
    query.pending = true;
    active_query_ = static_cast<std::size_t>(-1);
}

void GpuTimestampCollector::Cancel() noexcept {
    if (active_query_ == static_cast<std::size_t>(-1)) {
        return;
    }
    auto& query = queries_[active_query_];
    context_->End(query.end.get());
    context_->End(query.disjoint.get());
    query.pending = true;
    query.cancelled = true;
    active_query_ = static_cast<std::size_t>(-1);
}

void GpuTimestampCollector::Poll() noexcept {
    if (queries_.empty()) {
        return;
    }

    for (auto& query : queries_) {
        if (!query.pending) {
            continue;
        }

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        const HRESULT disjoint_result = context_->GetData(
            query.disjoint.get(),
            &disjoint,
            sizeof(disjoint),
            kDoNotFlush);
        if (disjoint_result == S_FALSE) {
            continue;
        }
        if (FAILED(disjoint_result)) {
            query.pending = false;
            continue;
        }

        UINT64 start = 0;
        UINT64 end = 0;
        const HRESULT start_result = ReadQuery(context_, query.start.get(), start);
        const HRESULT end_result = ReadQuery(context_, query.end.get(), end);
        if (start_result == S_FALSE || end_result == S_FALSE) {
            continue;
        }
        if (FAILED(start_result) || FAILED(end_result)) {
            query.pending = false;
            continue;
        }

        query.pending = false;
        if (query.cancelled || disjoint.Disjoint || end < start || disjoint.Frequency == 0) {
            continue;
        }
        const double milliseconds =
            (static_cast<double>(end - start) * 1000.0) /
            static_cast<double>(disjoint.Frequency);
        if (!std::isfinite(milliseconds) || milliseconds < 0.0) {
            continue;
        }
        samples_.push_back(milliseconds);
        while (samples_.size() > kSampleWindow) {
            samples_.pop_front();
        }
    }
}

void GpuTimestampCollector::Reset() noexcept {
    samples_.clear();
}

bool GpuTimestampCollector::Enabled() const noexcept {
    return !queries_.empty();
}

GpuTimingStatistics GpuTimestampCollector::Statistics() const noexcept {
    GpuTimingStatistics statistics{};
    statistics.sample_count = samples_.size();
    if (samples_.empty()) {
        return statistics;
    }

    std::vector<double> ordered(samples_.begin(), samples_.end());
    std::sort(ordered.begin(), ordered.end());
    const auto percentile = [&ordered](const double fraction) {
        const auto rank = static_cast<std::size_t>(std::ceil(
            fraction * static_cast<double>(ordered.size())));
        const std::size_t index = rank == 0 ? 0 : std::min(rank - 1, ordered.size() - 1);
        return ordered[index];
    };
    statistics.p50_milliseconds = percentile(0.50);
    statistics.p95_milliseconds = percentile(0.95);
    return statistics;
}

} // namespace osss
