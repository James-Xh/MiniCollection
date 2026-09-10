#pragma once
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

// 参数结构体
typedef struct {
    float tri_on;          // 输入的启动振幅比，软件里的“信号开始阈值”
    float tri_off;         // 输入的结束振幅比，软件里的“信号结束阈值”
    int nsta;              // 短窗点数，软件里的“短窗长度”，最大设置为2s*采样率
    int nlta;              // 长窗点数，软件里的“长窗长度”，最大设置为1s*采样率
    int detect_ch;         // 触发所需最小通道数，软件里的“触发达标最小通道数”
    float amp_thre;        // 振幅限制系数，软件里的“强度限制”
    float energy_thre;     // 能量比限制阈值 ，软件里的“能量限制”
    int qualified_ch;      // 质量合格所需最小通道数，软件里的“限制达标最小通道数”
    float SF;              // 采样率， 500Hz
    int intrach_offset;    // 通道内合并阈值点数
    int interch_offset;    // 跨通道关联阈值点数
    int prev_state;        // 输入：上一个状态；输出：当前状态，初始值为0，每次计算会更新，用户不输入
} Event_Para;
// 注意，软件里现有的窗口长度取消掉，不让用户设置了，定死五秒，
// 但是每次计算传递的数据点数是动态的，为5秒*采样率+长窗点数的冗余量（或更多，因为部分状态需要数据累加）

// 存储单个通道候选震相的结构
struct Pick {
    int on;
    int off;
    int ch;
    bool is_qual;
};

// 核心导出函数
// 返回值：当前状态 (0:静默, 1:闭合完成, 2:持续中, 3:异常中断)
int STALTA_Process(float** data, int chNum, int ptNum, Event_Para& para, 
                   std::vector<std::pair<int, int>>& finalPicks, 
                   int& globalStart, int& globalEnd);