#include "TcpDataManager.h"
#include "Commdef.h"
#include "CallManage.h"
#include "LogManage.h"
#include "AppConfig.h"
#include "QSqLiteHelper.h"
#include <QSettings>
#include "QDebug"
#include <QtEndian>
#include <limits>

// 微秒时间戳 → 日志可读格式："2026-08-03 08:34:23.156"
static QString usToLogStr(qint64 us)
{
	if (us <= 0)
		return QStringLiteral("0");
	return QDateTime::fromMSecsSinceEpoch(us / 1000LL).toString("yyyy-MM-dd hh:mm:ss.zzz");
}

TcpDataManager* TcpDataManager::instance()
{
    static TcpDataManager mgr;
    return &mgr;
}

TcpDataManager::TcpDataManager(QObject *parent) : QObject(parent)
{
    m_processTimer = new QTimer(this);
    m_processTimer->setInterval(250); // 每100ms处理一次缓存帧
    connect(m_processTimer, &QTimer::timeout, this, &TcpDataManager::processPendingFrames);
}

TcpDataManager::~TcpDataManager()
{
    qDeleteAll(m_connections);
}

QList<TcpDataManager::StationCfg> TcpDataManager::loadStationConfigs()
{
    // 站点连接信息存于数据库 tbl_station，ID 为 1-based（站0 = ID 1，站1 = ID 2 …）。
    QList<StationCfg> list;
    QList<QStringList> rows = SQLMgr->selectDataWithSql(
        "SELECT ID, IP, PORT, CAN_IP, CAN_PORT FROM tbl_station ORDER BY ID;", "loadStationConfigs: ");
    for (const QStringList& row : rows) {
        if (row.size() < 3) continue;
        int idOneBased = row.at(0).toInt();
        QString ip = row.at(1);
        int port = row.at(2).toInt();
        if (idOneBased <= 0 || ip.isEmpty() || port == 0) continue;
        StationCfg cfg;
        cfg.id = idOneBased - 1;
        cfg.ip = ip;
        cfg.port = static_cast<quint16>(port);
        cfg.canIp = row.size() > 3 ? row.at(3) : QString();
        cfg.canPort = row.size() > 4 ? static_cast<quint16>(row.at(4).toInt()) : 0;
        cfg.ctypeList = stationCompType(cfg.id,cfg.isAuto);
        list.append(cfg);
    }
    return list;
}

int TcpDataManager::stationCount()
{
    return loadStationConfigs().size();
}

QList<E_CompType> TcpDataManager::stationCompType(int staid, bool& isAuto)
{
    QList<E_CompType> etype;
	for (int ch = 0; ch < CHANNEL_COUNT; ++ch)
		etype.append(E_CompType_auto);

	QList<QStringList> rows = SQLMgr->selectDataWithSql(
		QString("SELECT AUTO FROM tbl_station_comp WHERE STATION_ID=%1;").arg(staid + 1),
		"stationAutoComp: ");
    if (rows.isEmpty() || rows.at(0).isEmpty() || rows.at(0).at(0).toInt() == 0)
    {
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch)
            etype[ch] = E_CompType_Single;
		QList<QStringList> rows = SQLMgr->selectDataWithSql(
			QString("SELECT CHANNEL, THREE_COMP FROM tbl_channel_comp WHERE STATION_ID=%1;").arg(staid + 1),
			"loadCompForStation: ");
		const int chBase = staid * CHANNEL_COUNT;   // 全局通道号基准：站(0-based)*16
		for (const QStringList& row : rows) {
			if (row.size() < 2) continue;
			int ch = row.at(0).toInt() - chBase - 1;   // 全局通道号 -> 该站 0-based 本地通道
			if (ch < 0 || ch >= CHANNEL_COUNT) continue;
			if (row.at(1).toInt() == 0)
				etype[ch] = E_CompType_auto;
			if (row.at(1).toInt() == 1)
				etype[ch] = E_CompType_Three;
			else
				etype[ch] = E_CompType_Single;
		}
        isAuto = false;
    }
    else
    {
        isAuto = true;
    }

    return etype;
}

void TcpDataManager::migrateStationsFromIni()
{
    // tbl_station 已有数据则视为已迁移，不再重复导入（避免覆盖用户在库里的改动）。
    QList<QStringList> rows = SQLMgr->selectDataWithSql(
        "SELECT COUNT(*) FROM tbl_station;", "migrateStationsFromIni: ");
    if (!rows.isEmpty() && !rows.first().isEmpty() && rows.first().first().toInt() > 0)
        return;

    // 旧配置：[Sver1]..[SverN]，IP/Port 必填，CanIP/CanPort 可选（老版本无此项）。
    int migrated = 0;
    for (int i = 1; i <= MAX_STATION; ++i) {
        QString section = QString("Sver%1").arg(i);
        QString ip = APPCfg->getConfig(section, "IP").toString();
        int port = APPCfg->getConfig(section, "Port").toInt();
        if (ip.isEmpty() || port == 0) continue;

        StationCfg cfg;
        cfg.id = i;  // addStation 直接写入 tbl_station.ID，段号 i 即 1-based ID
        cfg.ip = ip;
        cfg.port = static_cast<quint16>(port);
        cfg.canIp = APPCfg->getConfig(section, "CanIP").toString();
        cfg.canPort = static_cast<quint16>(APPCfg->getConfig(section, "CanPort").toInt());
        cfg.isAuto = true;
        // addStation 要求每站 16 条分量记录，迁移时统一初始化为自动模式
        for (int ch = 0; ch < CHANNEL_COUNT; ++ch)
            cfg.ctypeList.append(E_CompType_auto);
        if (addStation(cfg)) ++migrated;
    }
    LOGMgr->addLog("TcpDataManager", QStringLiteral("从ini迁移站点到数据库：%1个").arg(migrated));
}

int TcpDataManager::globalChannel(int idOneBased, int localCh)
{
    // 站 N（1-based）的本地通道 1..16 映射为全局通道 (N-1)*16 + localCh。
    return (idOneBased - 1) * CHANNEL_COUNT + localCh;
}

bool TcpDataManager::addStation(const StationCfg& cfg)
{
//     QString sql = QString(
//         "INSERT OR REPLACE INTO tbl_station (ID, IP, PORT, CAN_IP, CAN_PORT) "
//         "VALUES (%1, '%2', %3, '%4', %5);")
//         .arg(cfg.id)
//         .arg(cfg.ip)
//         .arg(cfg.port)
//         .arg(cfg.canIp)
//         .arg(cfg.canPort);
//     if (!SQLMgr->executeSql(sql))
//         return false;
// 
//     // 同步初始化该站的分量记录：站级 auto 标志 + 16 个通道（全局通道号）。
//     bool isok = SQLMgr->executeSql(QString(
//         "INSERT OR IGNORE INTO tbl_station_comp (STATION_ID, AUTO) VALUES (%1, %2);")
//         .arg(cfg.id).arg(cfg.isAuto ? 1 : 0));
//     if (isok)
//     {
//         for (int ch = 1; ch <= CHANNEL_COUNT; ++ch) {
// 
//             if (SQLMgr)
//             {
//                 SQLMgr->executeSql(QString(
//                     "INSERT OR IGNORE INTO tbl_channel_comp (STATION_ID, CHANNEL, THREE_COMP) "
//                     "VALUES (%1, %2, %3);")
//                     .arg(cfg.id)
//                     .arg(globalChannel(cfg.id, ch))
//                     .arg(cfg.ctypeList.at(ch - 1)));
//             }
//         }
//     }
//     return true;

	if (!SQLMgr)
		return false;

	QString sql = QString(
        "INSERT OR REPLACE INTO tbl_station (ID, IP, PORT, CAN_IP, CAN_PORT) "
        "VALUES (%1, '%2', %3, '%4', %5);")
        .arg(cfg.id)
        .arg(cfg.ip)
        .arg(cfg.port)
        .arg(cfg.canIp)
        .arg(cfg.canPort);
    if (!SQLMgr->executeSql(sql))
        return false;

	bool ok = SQLMgr->executeSql(QString(
		"INSERT OR IGNORE INTO tbl_station_comp "
		"(STATION_ID, AUTO) VALUES (%1, %2);")
		.arg(cfg.id)
		.arg(cfg.isAuto ? 1 : 0));

	for (int ch = 1; ok && ch <= CHANNEL_COUNT; ++ch) {
		if (ch > cfg.ctypeList.size()) {
			ok = false;
			break;
		}

		ok = SQLMgr->executeSql(QString(
			"INSERT OR IGNORE INTO tbl_channel_comp "
			"(STATION_ID, CHANNEL, THREE_COMP) VALUES (%1, %2, %3);")
			.arg(cfg.id)
			.arg(globalChannel(cfg.id, ch))
			.arg(cfg.ctypeList.at(ch - 1)));
	}

	if (ok) {
		return true;
	}

	return false;
}

bool TcpDataManager::delStation(int idOneBased)
{
    // 删除该站在三张表中的记录（站级标志 + 16 个通道）。
    SQLMgr->executeSql(QString("DELETE FROM tbl_channel_comp WHERE STATION_ID = %1;").arg(idOneBased));
    SQLMgr->executeSql(QString("DELETE FROM tbl_station_comp WHERE STATION_ID = %1;").arg(idOneBased));
    if (!SQLMgr->executeSql(QString("DELETE FROM tbl_station WHERE ID = %1;").arg(idOneBased)))
        return false;

    // STATION_ID / CHANNEL 仅作顺序编号，删除后把编号更大的站顺次前移一位，
    // 保证 STATION_ID 从 1 起连续、CHANNEL 从 1 起连续（每站 16 通道）。
    // 升序处理：每次前移的目标编号都已被上一步腾空，不会与主键冲突。
    QList<QStringList> rows = SQLMgr->selectDataWithSql(
        QString("SELECT ID FROM tbl_station WHERE ID > %1 ORDER BY ID ASC;").arg(idOneBased),
        "delStation renumber: ");
    for (const QStringList& row : rows) {
        if (row.isEmpty()) continue;
        int oldId = row.at(0).toInt();
        int newId = oldId - 1;
        SQLMgr->executeSql(QString("UPDATE tbl_station SET ID = %1 WHERE ID = %2;").arg(newId).arg(oldId));
        SQLMgr->executeSql(QString("UPDATE tbl_station_comp SET STATION_ID = %1 WHERE STATION_ID = %2;").arg(newId).arg(oldId));
        // 通道号整体下移一站（16 个），STATION_ID 同步前移。
        SQLMgr->executeSql(QString(
            "UPDATE tbl_channel_comp SET STATION_ID = %1, CHANNEL = CHANNEL - %2 WHERE STATION_ID = %3;")
            .arg(newId).arg(CHANNEL_COUNT).arg(oldId));
    }
    return true;
}

void TcpDataManager::syncCompTables()
{
    // 遍历 tbl_station，为每个已配置站补齐分量记录：站级 auto 标志 + 16 个通道（全局通道号）。
    // 使用 INSERT OR IGNORE，只填充缺失项，不覆盖用户已有设置。
    QList<QStringList> rows = SQLMgr->selectDataWithSql(
        "SELECT ID FROM tbl_station ORDER BY ID;", "syncCompTables: ");
    int stations = 0, channels = 0;
    for (const QStringList& row : rows) {
        if (row.isEmpty()) continue;
        int idOneBased = row.at(0).toInt();
        if (idOneBased <= 0) continue;

        SQLMgr->executeSql(QString(
            "INSERT OR IGNORE INTO tbl_station_comp (STATION_ID, AUTO) VALUES (%1, 0);")
            .arg(idOneBased));
        for (int ch = 1; ch <= CHANNEL_COUNT; ++ch) {
            SQLMgr->executeSql(QString(
                "INSERT OR IGNORE INTO tbl_channel_comp (STATION_ID, CHANNEL, THREE_COMP) "
                "VALUES (%1, %2, 0);")
                .arg(idOneBased)
                .arg(globalChannel(idOneBased, ch)));
            ++channels;
        }
        ++stations;
    }
    LOGMgr->addLog("TcpDataManager", QStringLiteral("补齐分量记录：站%1个，通道%2个").arg(stations).arg(channels));
}

void TcpDataManager::initFromConfig()
{
    const QList<StationCfg> configs = loadStationConfigs();
    for (const StationCfg &cfg : configs) {
        TcpConnection *conn = new TcpConnection(cfg.id, cfg.ip, cfg.port, this);
        connect(conn, &TcpConnection::frameReady, this, &TcpDataManager::onFrameReceived);
        connect(conn, &TcpConnection::connectionChanged, this, &TcpDataManager::onConnectionStatus);
        m_connections[cfg.id] = conn;
        //conn->start();
        LOGMgr->addLog("TcpDataManager", QStringLiteral("添加站点%1 %2:%3").arg(cfg.id).arg(cfg.ip).arg(cfg.port));
    }
}

qint64 TcpDataManager::extractTimestamp(const QByteArray &frame)
{
	if (frame.size() < 11) {
		LOGMgr->addLog("TcpDataManager", QStringLiteral("收到过短数据帧: %1字节").arg(frame.size()));
		return 0;
	}

    // 根据用户代码，时间戳在帧尾偏移 dataSize-11 的位置
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(frame.constData());
    int tailOffset = frame.size() - 11;
    quint16 minutes = raw[tailOffset + 2] | ((raw[tailOffset + 3] & 0x0F) << 8);
    int hour = minutes / 60;
    int minute = minutes % 60;
    quint8 second = raw[tailOffset + 4];
    quint8 msecLow = raw[tailOffset + 5];
    quint8 msecHigh = raw[tailOffset + 6];
    quint16 msec = (msecHigh << 8) | msecLow;
    quint8 day = raw[tailOffset + 7];
    quint8 month = raw[tailOffset + 8];
    quint16 year = raw[tailOffset + 9] | (raw[tailOffset + 10] << 8);
    QDateTime dt(QDate(year, month, day), QTime(hour, minute, second, msec));
    if (!dt.isValid())
        return 0;
    return dt.toMSecsSinceEpoch() * 1000LL;
}

void TcpDataManager::onFrameReceived(int stationId, const QByteArray& frame)
{
    qint64 ts = extractTimestamp(frame);
    StationBuffer& buf = m_buffers[stationId];
    if (ts <= 0) {
        LOGMgr->addLog("TcpDataManager",
            QStringLiteral("站%1收到无效包首时间，已丢弃").arg(stationId));
        return;
    }

	// 重复或迟到帧属于异常时间情况，发布版也需要记录；"收到数据包"为逐帧明细，仅调试版（full_log）记录。
	if (ts <= buf.lastProcessedTime || buf.frames.contains(ts)) {
		LOGMgr->addLog("TcpDataManager",
			QStringLiteral("站%1收到重复或迟到帧，time=%2").arg(stationId).arg(usToLogStr(ts)));
	}
	if (AppConfig::Instance()->getConfig("Config", "full_log").toInt())
	{
		LOGMgr->addLog("TcpDataManager", QStringLiteral("站%1收到结束时间为time=%2的数据包").arg(stationId).arg(usToLogStr(ts)));
	}
    buf.frames.insert(ts, frame);  // QMap自动按微秒时间戳排序

    emit CallManage::getInstance()->sig_tcpData(stationId, frame);

	// 可选：限制缓存大小，防止内存无限增长（如保留最近1000帧）
	while (buf.frames.size() > 2000) {
		auto it = buf.frames.begin();
		buf.frames.erase(it);
	}
}

void TcpDataManager::processPendingFrames()
{
    // 跨站按包首时间公平派发，避免一个站的积压帧先占满下游队列。
    while (true) {
        int selectedStation = -1;
        qint64 selectedTime = std::numeric_limits<qint64>::max();

        for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
            StationBuffer& buf = it.value();
            while (!buf.frames.isEmpty() && buf.frames.firstKey() <= buf.lastProcessedTime)
                buf.frames.erase(buf.frames.begin());
            if (!buf.frames.isEmpty() && buf.frames.firstKey() < selectedTime) {
                selectedTime = buf.frames.firstKey();
                selectedStation = it.key();
            }
        }

        if (selectedStation < 0)
            break;

        StationBuffer& selected = m_buffers[selectedStation];
        QByteArray frame = selected.frames.take(selectedTime);
        selected.lastProcessedTime = selectedTime;
       // emit CallManage::getInstance()->sig_tcpData(selectedStation, frame);
    }
}

void TcpDataManager::startAll()
{
    m_processTimer->start();

    const QList<StationCfg> configs = loadStationConfigs();
    for (const StationCfg& cfg : configs) {
        if(m_connections[cfg.id])
            m_connections[cfg.id]->start();
    }
}


void TcpDataManager::stopAll()
{
	m_processTimer->stop();

	const QList<StationCfg> configs = loadStationConfigs();
	for (const StationCfg& cfg : configs) {
		if (m_connections[cfg.id])
			m_connections[cfg.id]->stop();
	}

	for (auto it = m_buffers.begin(); it != m_buffers.end(); ++it) {
		int stationId = it.key();
		StationBuffer& buf = it.value();
		if (buf.frames.isEmpty()) continue;
        buf.lastProcessedTime = 0;
        buf.frames.clear();
	}
}

void TcpDataManager::onConnectionStatus(int stationId, bool connected)
{
	if (!connected) {
		auto it = m_buffers.find(stationId);
		if (it != m_buffers.end()) {
			it->frames.clear();
			it->lastProcessedTime = 0;
		}
	}
    emit CallManage::getInstance()->sig_staConnectStatus(stationId, connected);
}
