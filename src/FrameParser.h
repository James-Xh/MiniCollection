#ifndef FRAMEPARSER_H
#define FRAMEPARSER_H

#include <QObject>
#include <QByteArray>
#include <QVector>
#include "Commdef.h"

// 协议帧解析（从原 WZDisplayChannel 中提取，剔除全部波形显示逻辑）：
//  - 局域网旧协议帧：256样点 x 16通道 x 8字节 + 11字节尾部时间戳（共32779字节）
//  - 云端节点帧：4585字节（CRC32校验 + 500样点 x Z/N/E 24bit）
// 解析结果转发给：
//  - MDataPickder：跨站同步对齐后调用 STALTA 算法并生成事件 mseed
//  - MDataWriter ：站0连续原始数据保存（data/bins）
class FrameParser : public QObject
{
    Q_OBJECT
public:
    explicit FrameParser(QObject* parent = nullptr);
    ~FrameParser();

    // 绑定输出目标（可在线程中使用，通过 invokeMethod 调用其 addData）
    void setPickder(QObject* pickder) { m_pickder = pickder; }
    void setWriter(QObject* writer) { m_writer = writer; }

public slots:
    void onTcpData(int stationId, const QByteArray& data);

private:
    bool parseLanFrame(int stationId, const QByteArray& data);
    bool parseCloudNodeFrame(int stationId, const QByteArray& data);
    void forwardData(int stationId, const QVector<float>& z,
        const QVector<float>& x, const QVector<float>& y,
        const QVector<float>& compFlags, qint64 frameStartUs);

    QObject* m_pickder = nullptr;   // MDataPickder（跨线程 invokeMethod）
    QObject* m_writer = nullptr;    // MDataWriter（跨线程 invokeMethod）

    // 每站 GPS 状态与通道状态缓存
    int m_nGps[MAX_STATION];
    int m_nChanStatus[MAX_STATION][CHANNEL_COUNT];
    int m_nChanType[MAX_STATION][CHANNEL_COUNT];
    qint64 m_nLastDateWarningTime = 0;
};

#endif // FRAMEPARSER_H
