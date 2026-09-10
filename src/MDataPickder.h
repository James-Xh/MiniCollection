#pragma once

#include <QObject>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <QMap>
#include <QSet>
#include <QVector>

#include "picker.h"
//#include "mydll.h"
#include "Commdef.h"

// 保存mseed的最大通道数（按最大站数预分配；实际算法通道数 = 已配置站数 * CHANNEL_COUNT）
#define MSEED_CHAN (CHANNEL_COUNT * MAX_STATION)
typedef struct ChannelData
{
	QVector<double>& getPoints()
	{
		return vecPoints;
	}
	void addPoints(double dPt)
	{
		vecPoints.append(dPt);
	}


	QVector<double>& getPointsX()
	{
		return vecPointsX;
	}
	void addPointsX(double dPt)
	{
		vecPointsX.append(dPt);
	}

	QVector<double>& getPointsY()
	{
		return vecPointsY;
	}
	void addPointsY(double dPt)
	{
		vecPointsY.append(dPt);
	}

	QVector<qint64>& getTimes()
	{
		return vecTimes;
	}
	void addTime(qint64 ntm)
	{
		vecTimes.append(ntm);
	}

	void clearAll()
	{
		vecPoints.clear();
		vecPointsX.clear();
		vecPointsY.clear();
		vecTimes.clear();
	}

	void setThreeComponents(bool isThree) { isHardwareThree = isThree; };
	bool getThreeComponents() { return isHardwareThree; }

	void removeCache()
	{
		int npcount = vecPoints.size();
		if (npcount > X_Axis_COUNT)
			vecPoints.remove(0, npcount - X_Axis_COUNT);

		npcount = vecPointsX.size();
		if (npcount > X_Axis_COUNT)
			vecPointsX.remove(0, npcount - X_Axis_COUNT);

		npcount = vecPointsY.size();
		if (npcount > X_Axis_COUNT)
			vecPointsY.remove(0, npcount - X_Axis_COUNT);

		npcount = vecTimes.size();
		if (npcount > X_Axis_COUNT)
			vecTimes.remove(0, npcount - X_Axis_COUNT);

	}

	void removeXY()
	{
		int npcount = vecPointsX.size();
		if (npcount > 0)
			vecPointsX.clear();
		npcount = vecPointsY.size();
		if (npcount > 0)
			vecPointsY.clear();
	}

	QVector<double> vecPoints;
	QVector<double> vecPointsX;
	QVector<double> vecPointsY;
	QVector<qint64> vecTimes;
	bool isHardwareThree = false;
}CHAN_DATA;

class MDataPickder  : public QObject
{
	Q_OBJECT

public:
	MDataPickder(QObject *parent = nullptr);
	~MDataPickder();

	// 添加一帧数据：frameStartUs 是第一个样点的 Unix 时间戳（微秒）。
	// 数据为 sampleCount*CHANNEL_COUNT 个按样点交错排列的 Z/X/Y 值。
	Q_INVOKABLE void addData(int nstaid, const QVector<float>& zValues,
		const QVector<float>& xValues, const QVector<float>& yValues,
		const QVector<float>& compFlags, qint64 frameStartUs,
		int sampleCount, int sampleRate, quint64 packageNo);

	Q_INVOKABLE void changeCallcValue(int ntag, QString strv);

	Q_INVOKABLE void setValue(int ndmx);

	bool callPickder();

private slots:
	void onCalcTimeout();

	void slot_canRecv(int nstaid, int ntag, QList<QStringList> lstDt);
	void slot_devStatus(int nrd, int nchan, int nstate, int typeBit);
	void slot_gpsStatus(int nstaid, int ngps);
	void slot_staConnectStatus(int nid, bool connected);
	void slot_loginOut();

private:
	struct StationStream {
		QVector<float> z;
		QVector<float> x;
		QVector<float> y;
		QVector<float> comp;
		qint64 firstSampleUs = 0;
		qint64 latestFrameStartUs = 0;
		qint64 lastReceiveMs = 0;
		quint64 latestPackageNo = 0;

		int sampleCount() const { return z.size() / CHANNEL_COUNT; }
		bool isEmpty() const { return z.isEmpty(); }
		void clear()
		{
			z.clear(); x.clear(); y.clear(); comp.clear();
			firstSampleUs = 0;
		}
	};

	void appendStationFrame(int stationId, const QVector<float>& zValues,
		const QVector<float>& xValues, const QVector<float>& yValues,
		const QVector<float>& compFlags, qint64 frameStartUs, quint64 packageNo);
	void evaluateSyncCoverage(qint64 nowMs);
	void evaluateProcessingCoverage(qint64 nowMs);
	void evaluatePendingJoinStations(qint64 nowMs);
	void logSyncDiagnostics(qint64 nowMs, bool ready, qint64 overlapUs,
		qint64 latestDiffUs, int problemStation);
	bool computeCoverageOverlap(const QSet<int>& stations, qint64* overlapUs,
		qint64* latestDiffUs, int* problemStation) const;
	void tryBuildAlignedData();
	void commitAlignedBlock(qint64 startUs, int sampleCount, const QList<int>& stations);
	void sendCalcJson(int sampleCount, const QList<int>& stations);
	void trimStationStream(StationStream& stream);
	void removeStationSamples(StationStream& stream, int sampleCount);
	QList<int> participatingStations() const;
	QVector<int> activeGlobalChannels() const;
	void refreshStationTopology();
	void resetCalculationState(bool clearRawStreams);
	void setSyncState(int state, qint64 maxDiffUs, int stationId, const QString& message);
	void clearStationState(int stationId);
	void clearAllState();

	QTimer* m_calcTimer = NULL;

	QMutex m_mutex;

	QMap<int, StationStream> m_stationStreams;
	QSet<int> m_onlineStations;          // 当前在线(或正在送数)的站集合
	QSet<int> m_statusKnownStations;     // 已收到过明确连接状态的站
	QSet<int> m_processingStations;      // 当前算法窗口采用的站集合
	QSet<int> m_pendingJoinStations;     // 已在线、通过3组同步验证后再加入算法的站
	int m_nStationCount = 1;             // 已配置站数（构造时读取）
	int m_syncState = 0;
	int m_syncRecoveryCount = 0;
	bool m_channelThresholdWarning = false;
	bool m_pendingJoinWarning = false;
	bool m_alignmentEstablished = false;
	qint64 m_lastSyncDiffUs = 0;
	qint64 m_syncMismatchStartMs = 0;
	qint64 m_nextSyncDiagMs = 0;         // 同步诊断日志节流
	qint64 m_nextCacheDiagMs = 0;        // 缓存一致性诊断日志节流
	static const qint64 kSamplePeriodUs = 2000;
	static const qint64 kMinOverlapUs = 100000;   // 建立/恢复同步所需的最小公共覆盖时长(100ms)
	static const qint64 kStreamIdleMs = 3000;     // 站点静默判定：超过该时长无数据才视为异常
	static const qint64 kSyncAlarmDelayMs = 1000;
	static const int kRecoveryGroups = 3;

	Event_Para m_demo_eventp;
	int ChNum = MSEED_CHAN;
	float SF = 500.00f;

// 	int* m_StartWz = NULL;
// 	int* m_EndWz = NULL;
	QVector<CHAN_DATA*>		m_vecCacheData;

	int m_nCalcWin = X_Axis_COUNT;
	float* m_fdata[MSEED_CHAN]; // 示例数据
	int m_nArrNum = 10;// m_fdata分配空间 ，m_nArrNum个5秒的数据
	int m_nChanStatus[MSEED_CHAN] = { 0 };
	int m_nGain[MSEED_CHAN] = { 1 };			// 通道增益、放大率
	QMap<int, int> m_stationGps;            // 每站最近一次GPS时钟状态(ngps)
	QSet<int> m_gpsAbnormalStations;        // 时钟异常站(ngps!=5/6)，其数据不参与拾取计算
	QMap<int, qint64> m_nextGpsDropLogMs;   // 时钟异常丢帧日志节流

	int m_nUnclosedPtNum = 0;	// 未闭合状态下添加点数，累加
	qint64 m_lastSavedEventStartMs = -1;
	qint64 m_lastSavedEventEndMs = -1;
	int m_nDmx = 0;

	int m_nCount = 0;
};

