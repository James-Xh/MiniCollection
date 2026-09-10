#include "picker.h"

// 滑动窗口平方均值计算 (保持不变)
void compute_moving_avg_sq(float* data, int n, int window, std::vector<float>& out) {
    out.assign(std::max(n, 0), 0.0f);
    if (!data || n <= 0 || window <= 0 || window > n) return;
    double current_sum = 0.0;
    for (int i = 0; i < window; ++i) current_sum += (double)data[i] * data[i];
    out[window - 1] = (float)(current_sum / window);
    for (int i = window; i < n; ++i) {
        current_sum += (double)data[i] * data[i] - (double)data[i - window] * data[i - window];
        out[i] = (float)(current_sum / window);
    }
}

// 合并逻辑 (保持不变)
std::vector<std::pair<int, int>> merge_picks(std::vector<std::pair<int, int>>& segs, int offset) {
    if (segs.empty()) return {};
    std::sort(segs.begin(), segs.end());
    std::vector<std::pair<int, int>> merged;
    int curr_s = segs[0].first; int curr_e = segs[0].second;
    for (size_t i = 1; i < segs.size(); ++i) {
        if (segs[i].first - curr_e <= offset) {
            curr_e = (segs[i].second > curr_e) ? segs[i].second : curr_e;
        } else {
            merged.push_back({curr_s, curr_e});
            curr_s = segs[i].first; curr_e = segs[i].second;
        }
    }
    merged.push_back({curr_s, curr_e});
    return merged;
}

int STALTA_Process(float** data, int chNum, int ptNum, Event_Para& para, 
                   std::vector<std::pair<int, int>>& finalPicks, 
                   int& globalStart, int& globalEnd) 
{
    finalPicks.assign(std::max(chNum, 0), {-1, -1});
    globalStart = -1;
    globalEnd = -1;

    // 配置缺失时 toInt() 会返回 0。window == 0 会导致 out[-1]
    // 和除零，因此在进入算法前统一拒绝非法参数。
    if (!data || chNum <= 0 || ptNum <= 0 ||
        para.nsta <= 0 || para.nlta <= 0 ||
        para.nsta > ptNum || para.nlta > ptNum ||
        para.nsta > para.nlta ||
        para.detect_ch <= 0 || para.detect_ch > chNum ||
        para.qualified_ch <= 0 || para.qualified_ch > chNum) {
        para.prev_state = 0;
        return 0;
    }
    for (int ic = 0; ic < chNum; ++ic) {
        if (!data[ic]) {
            para.prev_state = 0;
            return 0;
        }
    }

    int warmup_pts = (int)para.nlta;

    double r_on_user = (double)para.tri_on * para.tri_on;
    double r_off_user = (double)para.tri_off * para.tri_off;
    const double triDenominator = para.nsta * r_on_user + (para.nlta - para.nsta);
    if (!std::isfinite(triDenominator) || std::abs(triDenominator) < 1e-12) {
        para.prev_state = 0;
        return 0;
    }
    double tri_on_internal = (para.nlta * r_on_user) / triDenominator;
    double tri_off_internal = r_off_user; 

    std::vector<Pick> allCandidates;

    // 1. 逐通道提取
    for (int ic = 0; ic < chNum; ++ic) {
        std::vector<float> sta, lta;
        compute_moving_avg_sq(data[ic], ptNum, para.nsta, sta);
        compute_moving_avg_sq(data[ic], ptNum, para.nlta, lta);
        std::vector<std::pair<int, int>> raw_segs;
        int ptr = warmup_pts;
        while (ptr < ptNum) {
            int ps = -1;
            for (int i = ptr; i < ptNum; ++i) {
                if (lta[i] > 0 && (sta[i] / lta[i]) > tri_on_internal) { ps = i; break; }
            }
            if (ps == -1) break;
            float lta_frozen = lta[ps];
            int pe = ptNum - 1;
            for (int i = ps + 1; i < ptNum; ++i) {
                if (lta_frozen > 0 && (sta[i] / lta_frozen) < tri_off_internal) { pe = i; break; }
            }
            raw_segs.push_back({ps, pe});
            ptr = pe + 1;
        }
        std::vector<std::pair<int, int>> merged = merge_picks(raw_segs, para.intrach_offset);
        for (auto& m : merged) allCandidates.push_back({m.first, m.second, ic, false});
    }

    // 2. 链式关联
    std::sort(allCandidates.begin(), allCandidates.end(), [](const Pick& a, const Pick& b) { return a.on < b.on; });
    std::vector<Pick> bestChain;
    for (int i = 0; i < (int)allCandidates.size(); ++i) {
        std::vector<Pick> tempChain = {allCandidates[i]};
        std::vector<bool> chUsed(chNum, false); chUsed[allCandidates[i].ch] = true;
        int lastOn = allCandidates[i].on;
        for (int j = i + 1; j < (int)allCandidates.size(); ++j) {
            if (!chUsed[allCandidates[j].ch] && allCandidates[j].on - lastOn <= para.interch_offset) {
                tempChain.push_back(allCandidates[j]);
                chUsed[allCandidates[j].ch] = true;
                lastOn = allCandidates[j].on;
            }
        }
        if ((int)tempChain.size() >= para.detect_ch) { bestChain = tempChain; break; }
    }

    if (bestChain.empty()) {
        para.prev_state = (para.prev_state == 2) ? 3 : 0;
        return para.prev_state;
    }

    // 3. 确定全局边界 (关键修复)
    int minOn = bestChain[0].on;
    int maxOn = bestChain[0].on;
    std::vector<int> offsets;
    for (auto& p : bestChain) {
        offsets.push_back(p.off);
        if (p.on < minOn) minOn = p.on;
        if (p.on > maxOn) maxOn = p.on;
    }

    double sum = std::accumulate(offsets.begin(), offsets.end(), 0.0);
    double mean = sum / offsets.size();
    double sq_sum = 0;
    for (int o : offsets) sq_sum += (o - mean) * (o - mean);
    double stdev = std::sqrt(sq_sum / offsets.size());
    double off_threshold = std::max(mean + 1.5 * stdev, (double)maxOn + 2.0 * para.nsta);

    int maxOffFiltered = 0;
    for (int o : offsets) {
        if (o <= off_threshold && o > maxOffFiltered) maxOffFiltered = o;
    }
    if (maxOffFiltered <= 0) maxOffFiltered = *std::max_element(offsets.begin(), offsets.end());

    // 4. 状态决策
    int localGlobalEnd = maxOffFiltered;
    bool is_ongoing = localGlobalEnd >= (ptNum - (int)(0.2 * para.SF));

    if (is_ongoing) {
        para.prev_state = 2;
        return 2;
    } else {
        // 信号已闭合，进行质量核验
        int qualChanCnt = 0;
        for (auto& cand : bestChain) {
            int ic = cand.ch;
            std::vector<float> sta_sq, lta_sq;
            compute_moving_avg_sq(data[ic], ptNum, para.nsta, sta_sq);
            compute_moving_avg_sq(data[ic], ptNum, para.nlta, lta_sq);

            double cf_sum = 0, cf_max = 0, ic_abs_max = 0;
            for (int i = warmup_pts; i < ptNum; ++i) {
                float cf = (lta_sq[i] > 0) ? (sta_sq[i] / lta_sq[i]) : 0.0f;
                cf_sum += cf;
                if (cf > cf_max) cf_max = cf;
                if (std::abs(data[ic][i]) > ic_abs_max) ic_abs_max = std::abs(data[ic][i]);
            }
            double energy = (cf_max > 0) ? ((cf_sum / (ptNum - warmup_pts)) / cf_max) : 1.0;

            if (ic_abs_max > para.amp_thre * 1310.72 && energy < para.energy_thre) qualChanCnt++;
            finalPicks[ic] = {cand.on, std::min(cand.off, localGlobalEnd)};
        }

        if (qualChanCnt >= para.qualified_ch) {
            globalStart = minOn;
            globalEnd = localGlobalEnd;
            para.prev_state = 1; return 1;
        } else {
            int failState = (para.prev_state == 2) ? 3 : 0;
            para.prev_state = failState;
            finalPicks.assign(chNum, {-1, -1});
            return failState;
        }
    }
}
