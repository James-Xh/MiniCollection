#ifndef TCPDATAMANAGER_H
#define TCPDATAMANAGER_H

#include <QObject>
#include <QMap>
#include <QQueue>
#include <QTimer>
#include <QList>
#include <QString>
#include "TcpConnection.h"

enum E_CompType
{
    E_CompType_auto = 0,
    E_CompType_Three,      //三分量
    E_CompType_Single    //单分量
};

class TcpDataManager : public QObject
{
    Q_OBJECT
public:
    static TcpDataManager* instance();
    void initFromConfig();               // 从配置读取多IP、端口
    void startAll();                     // 启动所有站点连接（精简版：无登录流程）
    void stopAll();                      // 停止所有站点连接

    // 站点配置（0-based 站号），供 Manager / Picker 共用，避免站号、站数各算各的
    struct StationCfg {
        int id; QString ip; quint16 port; QString canIp; quint16 canPort; bool isAuto = false; QList<E_CompType> ctypeList;
    };
    static QList<StationCfg> loadStationConfigs();  // 读 tbl_station，id = ID-1
    static int stationCount();                      // 已配置的有效站点数

    static QList<E_CompType> stationCompType(int staid, bool &isAuto);

    // 一次性迁移：tbl_station 为空时，把旧 [SverN] ini 配置导入数据库。
    static void migrateStationsFromIni();

    // 站点配置增删（写入数据库 tbl_station）。strID 为 1-based 站号。
    // addStation/delStation 会同步维护该站在 tbl_station_comp / tbl_channel_comp 中的
    // 16 个通道记录：站 N（1-based）对应全局通道号 (N-1)*16+1 .. N*16。
    static bool addStation(const StationCfg& cfg);
    static bool delStation(int idOneBased);

    // 全局通道号：站 idOneBased（1-based）的本地通道 localCh（1..16）-> 全局通道号。
    static int globalChannel(int idOneBased, int localCh);

    // 数据补齐：为 tbl_station 中已存在、但缺少分量记录的站，补齐 tbl_station_comp
    // 及其 16 个 tbl_channel_comp 通道记录（全局通道号）。仅填充缺失项，不覆盖已有数据。
    static void syncCompTables();

private slots:
    void onFrameReceived(int stationId, const QByteArray &frame);
    void onConnectionStatus(int stationId, bool connected);
    void processPendingFrames();         // 定时处理排序后的帧
private:
    TcpDataManager(QObject *parent = nullptr);
    ~TcpDataManager();

    // 每个站点独立的按时间戳排序缓存 (QMap自动排序)
    struct StationBuffer {
        QMap<qint64, QByteArray> frames;   // key=包首时间戳(us), value=完整帧
        qint64 lastProcessedTime = 0;      // 上次已处理的最大时间戳(us)
    };
    QMap<int, StationBuffer> m_buffers;
    QMap<int, TcpConnection*> m_connections;
    QTimer *m_processTimer;

    qint64 extractTimestamp(const QByteArray &frame); // 提取并统一为包首时间戳(us)
};

#endif // TCPDATAMANAGER_H
