#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>
#include "AppConfig.h"

namespace {

constexpr double kAtol = 1e-12;

bool IsClose(double a, double b, double atol = kAtol) {
    return std::fabs(a - b) <= atol;
}

[[maybe_unused]] bool IsConstantChannel(const std::vector<double>& arr, double target) {
    for (double v : arr) {
        if (!IsClose(v, target)) {
            return false;
        }
    }
    return true;
}

double PercentileLinear(std::vector<double> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double position = percentile * static_cast<double>(values.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = static_cast<size_t>(std::ceil(position));
    const double fraction = position - static_cast<double>(lower);
    return values[lower] * (1.0 - fraction) + values[upper] * fraction;
}

double Median(std::vector<double> values) {
    return PercentileLinear(std::move(values), 0.5);
}

std::vector<double> GetChannelSignal(const std::vector<std::vector<double>>& wave_np, int ch) {
    std::vector<double> out;
    out.reserve(wave_np.size());
    for (const auto& row : wave_np) {
        if (ch >= 0 && ch < static_cast<int>(row.size())) {
            out.push_back(row[ch]);
        } else {
            out.push_back(0.0);
        }
    }
    return out;
}

bool HasAbsEqualNonzeroRun(const std::vector<double>& arr, int run_len = 10) {
    if (static_cast<int>(arr.size()) < run_len) {
        return false;
    }

    int cur = 1;
    for (int i = 1; i < static_cast<int>(arr.size()); ++i) {
        if (IsClose(std::fabs(arr[i]), std::fabs(arr[i - 1]))) {
            ++cur;
            if (cur >= run_len && !IsClose(std::fabs(arr[i]), 0.0)) {
                return true;
            }
        } else {
            cur = 1;
        }
    }
    return false;
}

bool HasSamePolarityBetweenMarkers(const std::vector<double>& arr, int rpos, int bpos) {
    if (rpos == 0 || bpos == 0 || arr.empty()) {
        return false;
    }

    int lo = std::max(0, std::min(rpos, bpos));
    int hi = std::min(static_cast<int>(arr.size()) - 1, std::max(rpos, bpos));
    if (hi - lo + 1 < 2) {
        return false;
    }

    int first_sign = 0;
    for (int i = lo; i <= hi; ++i) {
        double v = arr[i];
        if (IsClose(v, 0.0)) {
            continue;
        }

        int sign = (v > 0.0) ? 1 : -1;
        if (first_sign == 0) {
            first_sign = sign;
        } else if (first_sign != sign) {
            return false;
        }
    }

    return first_sign != 0;
}

bool HasSpecialSaturation(const std::vector<double>& arr, int min_count = 10) {
    int pos_count = 0;
    int neg_count = 0;

    for (double v : arr) {
        if (IsClose(v, 131071.0)) {
            ++pos_count;
        }
        if (IsClose(v, -131071.0)) {
            ++neg_count;
        }
    }

    return pos_count >= min_count || neg_count >= min_count;
}

bool HasUniformLocalExtremaBetweenMarkers(const std::vector<double>& arr, int rpos, int bpos) {
    if (rpos == 0 || bpos == 0 || arr.empty()) {
        return false;
    }

    int lo = std::max(0, std::min(rpos, bpos));
    int hi = std::min(static_cast<int>(arr.size()) - 1, std::max(rpos, bpos));
    if (hi - lo + 1 < 3) {
        return false;
    }

    std::vector<double> local_max_vals;
    std::vector<double> local_min_vals;

    for (int i = lo + 1; i <= hi - 1; ++i) {
        const double prev = arr[i - 1];
        const double curr = arr[i];
        const double next = arr[i + 1];

        if (curr > prev && curr > next) {
            local_max_vals.push_back(curr);
        }
        if (curr < prev && curr < next) {
            local_min_vals.push_back(curr);
        }
    }

    auto all_same = [](const std::vector<double>& values) {
        if (values.size() < 2) {
            return false;
        }
        const double first = values.front();
        for (double v : values) {
            if (!IsClose(v, first)) {
                return false;
            }
        }
        return true;
    };

    return all_same(local_max_vals) || all_same(local_min_vals);
}

// 候选规则 7：至少两个固定的激活通道，各自连续精确为 0，且两个通道的
// 连续置零区间重叠至少 run_len 点。固定通道对的要求避免在相邻采样点由
// 不同通道轮流为 0 时产生误判。
bool HasSharedZeroRun(
    const std::vector<std::vector<double>>& wave_np,
    const std::vector<int>& active_channels,
    int run_len = 10) {
    if (run_len <= 0 || static_cast<int>(wave_np.size()) < run_len) {
        return false;
    }

    for (size_t first = 0; first < active_channels.size(); ++first) {
        for (size_t second = first + 1; second < active_channels.size(); ++second) {
            const int first_ch = active_channels[first];
            const int second_ch = active_channels[second];
            int overlap_run = 0;

            for (const auto& row : wave_np) {
                const bool indices_valid =
                    first_ch >= 0 && second_ch >= 0 &&
                    first_ch < static_cast<int>(row.size()) &&
                    second_ch < static_cast<int>(row.size());
                const bool both_zero =
                    indices_valid && IsClose(row[first_ch], 0.0) && IsClose(row[second_ch], 0.0);

                if (both_zero) {
                    ++overlap_run;
                    if (overlap_run >= run_len) {
                        return true;
                    }
                } else {
                    overlap_run = 0;
                }
            }
        }
    }
    return false;
}

// 候选规则 8：用相对差分识别数字毛刺/溢出/阶跃，不使用绝对振幅阈值。
// ratio = 最大相邻点跳变 / 相邻跳变绝对值的 P95。
bool HasExtremeRelativeJump(const std::vector<double>& arr, double ratio_threshold = 50.0) {
    if (arr.size() < 2) {
        return false;
    }
    std::vector<double> abs_diff;
    abs_diff.reserve(arr.size() - 1);
    double max_diff = 0.0;
    for (size_t i = 1; i < arr.size(); ++i) {
        const double diff = std::fabs(arr[i] - arr[i - 1]);
        abs_diff.push_back(diff);
        max_diff = std::max(max_diff, diff);
    }
    const double diff_p95 = std::max(PercentileLinear(std::move(abs_diff), 0.95), kAtol);
    return max_diff / diff_p95 >= ratio_threshold;
}

// 候选规则 9：检测通道是否存在持续约 50 ms 的局部能量波包。
// ratio = 25 点滑动 RMS 的最大值 / 中位数。它抑制单点尖峰，保留持续波包。
bool HasLocalEnergyPacket(
    const std::vector<double>& arr,
    int window_len = 25,
    double ratio_threshold = 2.5) {
    if (static_cast<int>(arr.size()) < window_len || window_len <= 0) {
        return false;
    }
    const double baseline = Median(arr);
    std::vector<double> squared(arr.size(), 0.0);
    for (size_t i = 0; i < arr.size(); ++i) {
        const double centered = arr[i] - baseline;
        squared[i] = centered * centered;
    }

    double window_sum = std::accumulate(squared.begin(), squared.begin() + window_len, 0.0);
    std::vector<double> window_rms;
    window_rms.reserve(arr.size() - window_len + 1);
    window_rms.push_back(std::sqrt(window_sum / static_cast<double>(window_len)));
    for (size_t end = static_cast<size_t>(window_len); end < squared.size(); ++end) {
        window_sum += squared[end] - squared[end - window_len];
        window_rms.push_back(std::sqrt(std::max(0.0, window_sum / static_cast<double>(window_len))));
    }

    const double median_rms = std::max(Median(window_rms), kAtol);
    const double max_rms = *std::max_element(window_rms.begin(), window_rms.end());
    return max_rms / median_rms >= ratio_threshold;
}

}  // namespace

bool is_compliant_event(
    const std::vector<std::vector<double>>& wave_np,
    const std::vector<int>& status,
    const std::vector<int>& rpos,
    const std::vector<int>& bpos,
    const std::vector<int>& offch) {

    if (wave_np.empty() || wave_np.front().empty()) {
        return false;
    }

    int ndetectIndex = 0;
    for (int ch = 0; ch < status.size(); ch++)
    {
        if (status.at(ch) == 1 && rpos.at(ch) != 0 && bpos.at(ch) != 0)
            ndetectIndex++;
    }
    if (AppConfig::Instance()->getConfig("Para", "detect_ch").toInt() > ndetectIndex)
        return false;

    const int n_samples = static_cast<int>(wave_np.size());
    const int n_channels = static_cast<int>(wave_np.front().size());
    if (n_samples == 0 || n_channels == 0) {
        return false;
    }

    // 1a) offch 通道状态不能为 1。
    for (int ch_num_1based : offch) {
        const int ch = ch_num_1based - 1;
        if (ch >= 0 && ch < n_channels && ch < static_cast<int>(status.size())) {
            if (status[ch] == 1) {
                return false;
            }
        }
    }

    // 1b) 暂时禁用：status != 1 的通道不再要求必须是
    // 全 0 / 全 131071 / 全 -131071。
    // 原规则会排除部分有意义的地震波信号，待后续重新设计干扰判据。

    // 激活通道：status == 1
    std::vector<int> active_channels;
    active_channels.reserve(n_channels);
    for (int ch = 0; ch < n_channels && ch < static_cast<int>(status.size()); ++ch) {
        if (status[ch] == 1) {
            active_channels.push_back(ch);
        }
    }

    // 7) 固定的两个激活通道，其连续置零区间重叠至少 10 点。
    if (HasSharedZeroRun(wave_np, active_channels, 10)) {
        return false;
    }

    // 8) 至少 2 个激活通道出现相对常态差分极大的跳变。
    // 实验性规则：强近场事件仍有误判风险，应继续用真震样本校准阈值。
    int extreme_jump_channels = 0;
    for (int ch : active_channels) {
        if (HasExtremeRelativeJump(GetChannelSignal(wave_np, ch), 50.0)) {
            ++extreme_jump_channels;
            if (extreme_jump_channels >= 2) {
                return false;
            }
        }
    }

    // 9) 至少 3 个激活通道应出现持续 50 ms 的显著局部能量波包。
    // 三条新规则中此条误伤风险最高，保留为明确标注的实验性条件。
    int energy_packet_channels = 0;
    for (int ch : active_channels) {
        if (HasLocalEnergyPacket(GetChannelSignal(wave_np, ch), 25, 2.5)) {
            ++energy_packet_channels;
        }
    }
    if (energy_packet_channels < 3) {
        return false;
    }

    // 2) 激活通道中的非零红针位置不能出现 >= 3 次重复。
    std::unordered_map<int, int> rpos_counter;
    for (int ch : active_channels) {
        if (ch >= static_cast<int>(rpos.size())) {
            continue;
        }
        const int pos = rpos[ch];
        if (pos != 0) {
            ++rpos_counter[pos];
        }
    }
    for (const auto& kv : rpos_counter) {
        if (kv.second >= 3) {
            return false;
        }
    }

    // 3-6) 统计激活通道异常数。
    int abnormal_channel_count = 0;
    for (int ch : active_channels) {
        std::vector<double> sig = GetChannelSignal(wave_np, ch);
        const int rpos_v = (ch < static_cast<int>(rpos.size())) ? rpos[ch] : 0;
        const int bpos_v = (ch < static_cast<int>(bpos.size())) ? bpos[ch] : 0;

        const bool abnormal =
            HasAbsEqualNonzeroRun(sig, 10) ||
            HasSamePolarityBetweenMarkers(sig, rpos_v, bpos_v) ||
            HasSpecialSaturation(sig, 10) ||
            HasUniformLocalExtremaBetweenMarkers(sig, rpos_v, bpos_v);

        if (abnormal) {
            ++abnormal_channel_count;
            if (abnormal_channel_count >= 2) {
                return false;
            }
        }
    }

    return true;
}
