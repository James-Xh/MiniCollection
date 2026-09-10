#include "MDataPickder.h"
#include "libmseed.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDebug>
#include <cstring>
#include "AppConfig.h"
#include "CallManage.h"
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <limits>
#include "HttpMgr.h"
#include "TcpDataManager.h"

const qint64 MDataPickder::kSamplePeriodUs;
const qint64 MDataPickder::kMinOverlapUs;
const qint64 MDataPickder::kStreamIdleMs;
const qint64 MDataPickder::kSyncAlarmDelayMs;
const int MDataPickder::kRecoveryGroups;

MDataPickder::MDataPickder(QObject *parent)
	: QObject(parent)
	, m_calcTimer(new QTimer(this))
{
	connect(m_calcTimer, &QTimer::timeout, this, &MDataPickder::onCalcTimeout);
	m_calcTimer->setInterval(1000); // 默认1秒调用算法一次
	m_calcTimer->start();

	for (int i = 0; i < MSEED_CHAN; i++)
	{
		// 预分配空间
		m_fdata[i] = new float[X_Axis_COUNT* m_nArrNum];
		memset(m_fdata[i], 0, sizeof(float)*X_Axis_COUNT * m_nArrNum);

		m_vecCacheData << new CHAN_DATA;

		m_nGain[i] = 1;
	}

	//m_StartWz = new int[ChNum];
	//m_EndWz = new int[ChNum];

	connect(CallManage::getInstance(), &CallManage::sig_canRecv, this, &MDataPickder::slot_canRecv, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_devStatus, this, &MDataPickder::slot_devStatus, Qt::QueuedConnection);

	connect(CallManage::getInstance(), &CallManage::sig_gpsStatus, this, &MDataPickder::slot_gpsStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_staConnectStatus, this, &MDataPickder::slot_staConnectStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_loginOutClear, this, &MDataPickder::slot_loginOut, Qt::QueuedConnection);

	// 读取已配置的站数，决定算法实际通道数(ChNum = m_nStationCount * CHANNEL_COUNT)
	m_nStationCount = TcpDataManager::stationCount();
	if (m_nStationCount <= 0)
		m_nStationCount = 1;
	if (m_nStationCount > MAX_STATION)
		m_nStationCount = MAX_STATION;

	//m_demo_eventp.Method = 0;
	m_demo_eventp.tri_on = AppConfig::Instance()->getConfig("Para", "tri_on").toFloat();
	m_demo_eventp.tri_off = AppConfig::Instance()->getConfig("Para", "tri_off").toFloat();
	m_demo_eventp.nsta = AppConfig::Instance()->getConfig("Para", "nsta").toInt();
	m_demo_eventp.nlta = AppConfig::Instance()->getConfig("Para", "nlta").toInt();

	m_demo_eventp.detect_ch = AppConfig::Instance()->getConfig("Para", "detect_ch").toInt();
	m_demo_eventp.qualified_ch = AppConfig::Instance()->getConfig("Para", "qualified_ch").toInt();
	m_demo_eventp.energy_thre = AppConfig::Instance()->getConfig("Para", "energy_thre").toFloat();
	m_demo_eventp.amp_thre = AppConfig::Instance()->getConfig("Para", "amp_thre").toFloat();
	m_demo_eventp.intrach_offset = AppConfig::Instance()->getConfig("Para", "intrach").toInt();
	m_demo_eventp.interch_offset = AppConfig::Instance()->getConfig("Para", "interch").toInt();
	m_demo_eventp.SF = 500;

	m_nCalcWin = 2500;// AppConfig::Instance()->getConfig("Para", "calc_win_len").toInt();
	m_nCalcWin += m_demo_eventp.nlta;
}

MDataPickder::~MDataPickder()
{
	for (int i = 0; i < MSEED_CHAN; i++)
	{
		delete []m_fdata[i];

		delete m_vecCacheData[i];
	}
	m_vecCacheData.clear();

	//delete []m_StartWz;
	//delete[]m_EndWz;

}

Q_INVOKABLE void MDataPickder::addData(int nstaid, const QVector<float>& zValues,
	const QVector<float>& xValues, const QVector<float>& yValues,
	const QVector<float>& compFlags, qint64 frameStartUs,
	int declaredSampleCount, int sampleRate, quint64 packageNo)
{
	if (zValues.isEmpty() || nstaid < 0 || nstaid >= m_nStationCount)
		return;

	const int sampleCount = zValues.size() / CHANNEL_COUNT;
	if (sampleRate != 500 || frameStartUs <= 0 || declaredSampleCount != sampleCount ||
		zValues.size() % CHANNEL_COUNT != 0 || sampleCount <= 0 || sampleCount > 500 ||
		xValues.size() != zValues.size() || yValues.size() != zValues.size() ||
		compFlags.size() != CHANNEL_COUNT)
	{
		qWarning() << "MDataPickder: invalid frame" << nstaid << frameStartUs
			<< declaredSampleCount << sampleRate
			<< zValues.size() << xValues.size() << yValues.size();
		QMutexLocker locker(&m_mutex);
		setSyncState(2, 0, nstaid,
			QStringLiteral("站%1拾取数据格式或采样率异常").arg(nstaid + 1));
		return;
	}

	QMutexLocker locker(&m_mutex);
	// 时钟异常站：所有通道数据不进入拾取计算，等待时钟恢复（slot_gpsStatus 中重新放行）。
	if (m_gpsAbnormalStations.contains(nstaid)) {
		const qint64 gateNowMs = QDateTime::currentMSecsSinceEpoch();
		if (gateNowMs >= m_nextGpsDropLogMs.value(nstaid, 0)) {
			m_nextGpsDropLogMs[nstaid] = gateNowMs + 60000;
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("站%1时钟异常，丢弃一帧数据（不参与拾取计算）").arg(nstaid + 1));
		}
		return;
	}
	if (m_statusKnownStations.contains(nstaid) && !m_onlineStations.contains(nstaid))
		return; // 断线后仍在队列中的旧帧不能重新激活该站
	if (!m_statusKnownStations.contains(nstaid))
		m_onlineStations.insert(nstaid); // 尚未收到状态信号时以首帧作为在线依据
	refreshStationTopology();
	const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
	appendStationFrame(nstaid, zValues, xValues, yValues, compFlags,
		frameStartUs, packageNo);
	evaluateSyncCoverage(nowMs);
	tryBuildAlignedData();
}

void MDataPickder::appendStationFrame(int stationId, const QVector<float>& zValues,
	const QVector<float>& xValues, const QVector<float>& yValues,
	const QVector<float>& compFlags, qint64 frameStartUs, quint64 packageNo)
{
	StationStream& stream = m_stationStreams[stationId];
	const int incomingSamples = zValues.size() / CHANNEL_COUNT;
	int firstIncomingSample = 0;
	if (packageNo > 0 && stream.latestPackageNo > 0 &&
		packageNo != stream.latestPackageNo + 1) {
		emit CallManage::getInstance()->sig_addLog("PickerSync",
			QStringLiteral("站%1包号不连续：%2 -> %3")
				.arg(stationId + 1).arg(stream.latestPackageNo).arg(packageNo));
	}

	if (stream.isEmpty()) {
		stream.firstSampleUs = frameStartUs;
	}
	else {
		const qint64 expectedStartUs = stream.firstSampleUs +
			static_cast<qint64>(stream.sampleCount()) * kSamplePeriodUs;
		const qint64 deltaUs = frameStartUs - expectedStartUs;

		if (deltaUs >= kSamplePeriodUs) {
			stream.clear();
			stream.firstSampleUs = frameStartUs;
			m_syncRecoveryCount = 0;
			m_syncMismatchStartMs = 0;
			setSyncState(0, deltaUs, stationId,
				QStringLiteral("站%1数据不连续，等待重新同步").arg(stationId + 1));
		}
		else if (deltaUs < 0) {
			firstIncomingSample = static_cast<int>(
				(-deltaUs + kSamplePeriodUs - 1) / kSamplePeriodUs);
			if (firstIncomingSample >= incomingSamples)
				return; // 整包都已存在
		}
	}

	const int firstValue = firstIncomingSample * CHANNEL_COUNT;
	stream.z.reserve(stream.z.size() + zValues.size() - firstValue);
	stream.x.reserve(stream.x.size() + xValues.size() - firstValue);
	stream.y.reserve(stream.y.size() + yValues.size() - firstValue);
	for (int i = firstValue; i < zValues.size(); ++i) {
		stream.z.append(zValues.at(i));
		stream.x.append(xValues.at(i));
		stream.y.append(yValues.at(i));
	}
	stream.comp = compFlags;
	stream.latestFrameStartUs = frameStartUs;
	stream.latestPackageNo = packageNo;
	stream.lastReceiveMs = QDateTime::currentMSecsSinceEpoch();
	trimStationStream(stream);
}

// 计算给定站集合缓冲区时间覆盖区间的公共交集长度(微秒)。
// 返回 true 表示所有站均有有效缓冲；overlapUs 为交集长度(可能为负)；
// latestDiffUs 为各站最新包首时间差(仅用于状态展示)；problemStation 为限制交集末尾的站，
// 若某站缺少有效数据则为其站号并返回 false。
bool MDataPickder::computeCoverageOverlap(const QSet<int>& stations, qint64* overlapUs,
	qint64* latestDiffUs, int* problemStation) const
{
	*overlapUs = -1;
	*latestDiffUs = 0;
	*problemStation = -1;
	if (stations.isEmpty())
		return false;

	qint64 overlapStartUs = (std::numeric_limits<qint64>::min)();
	qint64 minEndUs = (std::numeric_limits<qint64>::max)();
	qint64 minLatestUs = (std::numeric_limits<qint64>::max)();
	qint64 maxLatestUs = (std::numeric_limits<qint64>::min)();
	for (int sid : stations) {
		const auto streamIt = m_stationStreams.constFind(sid);
		if (streamIt == m_stationStreams.cend() || streamIt->isEmpty() || streamIt->firstSampleUs <= 0) {
			*problemStation = sid;
			return false;
		}
		const qint64 startUs = streamIt->firstSampleUs;
		const qint64 endUs = startUs + static_cast<qint64>(streamIt->sampleCount()) * kSamplePeriodUs;
		minLatestUs = qMin(minLatestUs, startUs);
		maxLatestUs = qMax(maxLatestUs, startUs);
		if (startUs > overlapStartUs)
			overlapStartUs = startUs;
		if (endUs < minEndUs) {
			minEndUs = endUs;
			*problemStation = sid;
		}
	}
	*overlapUs = minEndUs - overlapStartUs;
	*latestDiffUs = maxLatestUs - minLatestUs;
	return true;
}

// 统一入口：评估在算站健康度与待加入站验证。
// 判据由旧的"包首时间戳差<=容差"改为"缓冲区时间覆盖交集"：
// 包首相位取决于各站起播时刻，天然可相差0~一个包长，不能作为同步依据；
// 样本级对齐精度仍由 tryBuildAlignedData 的 <2ms 校验保证。
void MDataPickder::evaluateSyncCoverage(qint64 nowMs)
{
	if (!m_processingStations.isEmpty())
		evaluateProcessingCoverage(nowMs);
	if (!m_pendingJoinStations.isEmpty() && m_syncState == 1)
		evaluatePendingJoinStations(nowMs);
}

void MDataPickder::evaluateProcessingCoverage(qint64 nowMs)
{
	qint64 overlapUs = -1;
	qint64 latestDiffUs = 0;
	int problemStation = -1;
	const bool ready = computeCoverageOverlap(m_processingStations, &overlapUs, &latestDiffUs, &problemStation);

	if (ready && overlapUs > 0) {
		m_lastSyncDiffUs = latestDiffUs;
		m_syncMismatchStartMs = 0;
		if (m_syncState != 1) {
			++m_syncRecoveryCount;
			if (m_syncRecoveryCount >= kRecoveryGroups) {
				m_syncRecoveryCount = 0;
				resetCalculationState(false);
				setSyncState(1, m_lastSyncDiffUs, -1, QStringLiteral("拾取数据时间同步已恢复"));
			}
			else {
				setSyncState(0, m_lastSyncDiffUs, problemStation,
					QStringLiteral("拾取数据同步验证 %1/%2").arg(m_syncRecoveryCount).arg(kRecoveryGroups));
			}
		}
		logSyncDiagnostics(nowMs, ready, overlapUs, latestDiffUs, problemStation);
		return;
	}

	if (!ready) {
		// problemStation 无有效缓冲：属"数据暂缺"而非"失步"。
		// 关键：不重置恢复计数！否则"健康/暂缺"交替评估会让计数永远到不了3，状态2无法自愈。
		const auto streamIt = m_stationStreams.constFind(problemStation);
		const qint64 lastMs = (streamIt != m_stationStreams.cend()) ? streamIt->lastReceiveMs : 0;
		// 该站从未送数或缓冲刚被消费完/正在重连：等待，不算异常。
		if (lastMs == 0 || nowMs - lastMs < kStreamIdleMs) {
			logSyncDiagnostics(nowMs, ready, overlapUs, latestDiffUs, problemStation);
			return;
		}
	}
	else {
		// 数据齐全但没有共同覆盖，才是真正的失步证据。
		m_syncRecoveryCount = 0;
	}

	// 数据缺失时放宽确认时长(3s)，覆盖不足时按常规时长(1s)确认。
	if (m_syncMismatchStartMs <= 0)
		m_syncMismatchStartMs = nowMs;
	const qint64 mismatchMs = nowMs - m_syncMismatchStartMs;
	const qint64 alarmDelayMs = ready ? kSyncAlarmDelayMs : kStreamIdleMs;
	if (mismatchMs < alarmDelayMs) {
		if (m_syncState == 1)
			return; // 宽限期内保持计算，避免单次评估抖动打断拾取
		setSyncState(0, latestDiffUs, problemStation,
			QStringLiteral("站间共同数据覆盖不足，拾取计算等待同步"));
		logSyncDiagnostics(nowMs, ready, overlapUs, latestDiffUs, problemStation);
		return;
	}
	setSyncState(2, latestDiffUs, problemStation,
		QStringLiteral("站%1持续找不到与其它站的共同数据覆盖，拾取计算已暂停；最新包差%2ms")
			.arg(problemStation + 1).arg(latestDiffUs / 1000.0, 0, 'f', 1));
	logSyncDiagnostics(nowMs, ready, overlapUs, latestDiffUs, problemStation);
}

// 节流输出同步诊断：状态非"同步正常"时每2秒一条，包含各站缓冲状态与设备时钟偏差，
// 用于从日志直接定位"哪一站缺数据/哪一站时间戳与其它站存在固定偏差"。
void MDataPickder::logSyncDiagnostics(qint64 nowMs, bool ready, qint64 overlapUs,
	qint64 latestDiffUs, int problemStation)
{
	const bool fullLog = AppConfig::Instance()->getConfig("Config", "full_log").toInt();
	if (m_syncState == 1 && !fullLog)
		return;
	if (nowMs < m_nextSyncDiagMs)
		return;
	m_nextSyncDiagMs = nowMs + 2000;

	QString detail;
	const QList<int> stations = participatingStations();
	for (int sid : stations) {
		const auto it = m_stationStreams.constFind(sid);
		if (it == m_stationStreams.cend()) {
			detail += QStringLiteral("[站%1:无数据]").arg(sid + 1);
			continue;
		}
		detail += QStringLiteral("[站%1:样点%2,包首%3ms,设备-本机时差%4ms]")
			.arg(sid + 1)
			.arg(it->sampleCount())
			.arg(it->latestFrameStartUs / 1000)
			.arg(it->latestFrameStartUs / 1000 - nowMs);
	}
	emit CallManage::getInstance()->sig_addLog("PickerSync",
		QStringLiteral("同步诊断：状态%1，就绪%2，覆盖%3ms，包首差%4ms，关注站%5，恢复计数%6 %7")
			.arg(m_syncState)
			.arg(ready ? 1 : 0)
			.arg(overlapUs / 1000.0, 0, 'f', 1)
			.arg(latestDiffUs / 1000.0, 0, 'f', 1)
			.arg(problemStation + 1)
			.arg(m_syncRecoveryCount)
			.arg(detail));
}

// 待加入站验证：与在算站的共同覆盖连续 kRecoveryGroups 次达到 kMinOverlapUs 后加入。
void MDataPickder::evaluatePendingJoinStations(qint64 nowMs)
{
	Q_UNUSED(nowMs);
	QSet<int> jointStations = m_processingStations;
	jointStations.unite(m_pendingJoinStations);
	qint64 overlapUs = -1;
	qint64 latestDiffUs = 0;
	int problemStation = -1;
	const bool ready = computeCoverageOverlap(jointStations, &overlapUs, &latestDiffUs, &problemStation);
	if (!ready || overlapUs < kMinOverlapUs) {
		if (!m_pendingJoinWarning) {
			m_pendingJoinWarning = true;
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("新上线站与在算站暂无足够的共同数据覆盖，暂不加入拾取计算；最新包差%1ms")
					.arg(latestDiffUs / 1000.0, 0, 'f', 1));
		}
		return;
	}
	++m_syncRecoveryCount;
	if (m_syncRecoveryCount >= kRecoveryGroups) {
		m_processingStations.unite(m_pendingJoinStations);
		m_pendingJoinStations.clear();
		m_pendingJoinWarning = false;
		m_syncRecoveryCount = 0;
		resetCalculationState(false);
		emit CallManage::getInstance()->sig_pickerSyncStatus(1, latestDiffUs, -1,
			QStringLiteral("新上线站已通过同步验证并加入拾取计算"));
		emit CallManage::getInstance()->sig_addLog("PickerSync",
			QStringLiteral("新上线站已通过同步验证并加入拾取计算"));
	}
}

void MDataPickder::tryBuildAlignedData()
{
	if (m_syncState != 1 || m_processingStations.isEmpty())
		return;
	const int onlineChannelCount = m_processingStations.size() * CHANNEL_COUNT;
	if (m_demo_eventp.detect_ch > onlineChannelCount ||
		m_demo_eventp.qualified_ch > onlineChannelCount) {
		if (!m_channelThresholdWarning) {
			m_channelThresholdWarning = true;
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("在线通道数%1不足以满足拾取阈值，计算暂停").arg(onlineChannelCount));
		}
		return;
	}
	m_channelThresholdWarning = false;

	const QList<int> stations = participatingStations();
	while (!stations.isEmpty()) {
		qint64 initialMinStartUs = (std::numeric_limits<qint64>::max)();
		qint64 targetStartUs = (std::numeric_limits<qint64>::min)();
		for (int sid : stations) {
			const StationStream& stream = m_stationStreams[sid];
			if (stream.isEmpty())
				return;
			initialMinStartUs = qMin(initialMinStartUs, stream.firstSampleUs);
			targetStartUs = qMax(targetStartUs, stream.firstSampleUs);
		}

		// 首次建轴严格舍弃共同起点前的样点；建轴后保持已验证的<2ms相位，避免每包重复丢点。
		if (!m_alignmentEstablished ||
			targetStartUs - initialMinStartUs >= kSamplePeriodUs) {
			for (int sid : stations) {
				StationStream& stream = m_stationStreams[sid];
				const qint64 beforeStartUs = stream.firstSampleUs;
				const int beforeSamples = stream.sampleCount();
				const qint64 leadUs = targetStartUs - stream.firstSampleUs;
				const int drop = leadUs > 0
					? static_cast<int>((leadUs + kSamplePeriodUs - 1) / kSamplePeriodUs) : 0;
				if (drop >= stream.sampleCount()) {
					emit CallManage::getInstance()->sig_addLog("PickerSync",
						QStringLiteral("对齐丢弃整站数据：站%1，包号%2，firstSampleUs=%3，latestFrameStartUs=%4，样点数=%5，计划丢弃=%6，对齐起始=%7")
							.arg(sid + 1).arg(stream.latestPackageNo)
							.arg(beforeStartUs).arg(stream.latestFrameStartUs)
							.arg(beforeSamples).arg(drop).arg(targetStartUs));
					removeStationSamples(stream, stream.sampleCount());
					return;
				}
				if (drop > 0) {
					emit CallManage::getInstance()->sig_addLog("PickerSync",
						QStringLiteral("对齐丢弃前导样点：站%1，包号%2，firstSampleUs=%3->%4，latestFrameStartUs=%5，原样点数=%6，实际丢弃=%7，对齐起始=%8")
							.arg(sid + 1).arg(stream.latestPackageNo)
							.arg(beforeStartUs).arg(beforeStartUs + static_cast<qint64>(drop) * kSamplePeriodUs)
							.arg(stream.latestFrameStartUs).arg(beforeSamples).arg(drop).arg(targetStartUs));
				}
				removeStationSamples(stream, drop);
			}
		}

		qint64 minStartUs = (std::numeric_limits<qint64>::max)();
		qint64 maxStartUs = (std::numeric_limits<qint64>::min)();
		int commonSamples = (std::numeric_limits<int>::max)();
		for (int sid : stations) {
			const StationStream& stream = m_stationStreams[sid];
			minStartUs = qMin(minStartUs, stream.firstSampleUs);
			maxStartUs = qMax(maxStartUs, stream.firstSampleUs);
			commonSamples = qMin(commonSamples, stream.sampleCount());
		}
		if (maxStartUs - minStartUs >= kSamplePeriodUs || commonSamples <= 0)
			return;

		m_alignmentEstablished = true;
		const int blockSamples = qMin(commonSamples, 500);
		if (AppConfig::Instance()->getConfig("Config", "full_log").toInt())
		{
			for (int sid : stations) {
			const StationStream& stream = m_stationStreams[sid];
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("提交对齐块：站%1，alignedStartUs=%2，packageNo=%3，firstSampleUs=%4，latestFrameStartUs=%5，缓存样点数=%6，提交样点数=%7")
					.arg(sid + 1).arg(maxStartUs).arg(stream.latestPackageNo)
					.arg(stream.firstSampleUs).arg(stream.latestFrameStartUs)
					.arg(stream.sampleCount()).arg(blockSamples));
			}
		}
		commitAlignedBlock(maxStartUs, blockSamples, stations);
	}
}

void MDataPickder::commitAlignedBlock(qint64 startUs, int sampleCount, const QList<int>& stations)
{
	if (sampleCount <= 0)
		return;

	sendCalcJson(sampleCount, stations);
	const int maxCachePoints = X_Axis_COUNT * m_nArrNum;
	for (int sta : stations) {
		StationStream& stream = m_stationStreams[sta];
		// 每站用各自缓冲区真实起始时间打时间戳，站间<2ms的残差不再被统一时间标签掩盖。
		const qint64 staStartUs = stream.firstSampleUs;
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
			const int globalChannel = sta * CHANNEL_COUNT + ch;
			CHAN_DATA* channelData = m_vecCacheData.value(globalChannel, nullptr);
			if (!channelData)
				continue;
			for (int sample = 0; sample < sampleCount; ++sample) {
				const int sourceIndex = sample * CHANNEL_COUNT + ch;
				channelData->addPoints(stream.z.at(sourceIndex));
				channelData->addPointsX(stream.x.at(sourceIndex));
				channelData->addPointsY(stream.y.at(sourceIndex));
				channelData->addTime((staStartUs + static_cast<qint64>(sample) * kSamplePeriodUs) / 1000LL);
			}
			channelData->setThreeComponents(stream.comp.value(ch) > 0.5f);
			const int overflow = channelData->getPoints().size() - maxCachePoints;
			if (overflow > 0) {
				channelData->getPoints().remove(0, overflow);
				channelData->getPointsX().remove(0, qMin(overflow, channelData->getPointsX().size()));
				channelData->getPointsY().remove(0, qMin(overflow, channelData->getPointsY().size()));
				channelData->getTimes().remove(0, qMin(overflow, channelData->getTimes().size()));
			}
		}
	}
	for (int sta : stations)
	{
		StationStream& stream = m_stationStreams[sta];
		const int beforeSamples = stream.sampleCount();
		removeStationSamples(stream, sampleCount);
		// 逐块消费明细，日志量大，仅调试版（full_log）记录。
		if (AppConfig::Instance()->getConfig("Config", "full_log").toInt())
		{
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("消费对齐块：站%1，alignedStartUs=%2，packageNo=%3，消费样点数=%4，剩余样点数=%5，剩余firstSampleUs=%6")
					.arg(sta + 1).arg(startUs).arg(stream.latestPackageNo)
					.arg(qMin(sampleCount, beforeSamples)).arg(stream.sampleCount())
					.arg(stream.firstSampleUs));
		}
	}
}

void MDataPickder::sendCalcJson(int sampleCount, const QList<int>& stations)
{
	QJsonArray arrAll;
	for (int sta : stations) {
		const StationStream& stream = m_stationStreams[sta];
		// 每站使用各自缓冲区的真实起始时间，避免把<2ms的站间相位残差伪装成已对齐。
		const QDateTime dt = QDateTime::fromMSecsSinceEpoch(stream.firstSampleUs / 1000LL);
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
			QJsonArray samples;
			for (int sample = 0; sample < sampleCount; ++sample)
				samples.append(stream.z.at(sample * CHANNEL_COUNT + ch));

			const int globalChannel = sta * CHANNEL_COUNT + ch;
			QJsonObject object;
			object.insert("net", QString::number(sta + 1));
			object.insert("sta", QString::number(sta + 1));
			object.insert("loc", "00");
			object.insert("chan", QString::number(globalChannel + 1));
			object.insert("sampleRate", 500);
			object.insert("startTime", dt.toString("yyyy-MM-dd hh:mm:ss.zzz"));
			object.insert("samples", samples);
			arrAll.append(object);
		}
	}
	if (!arrAll.isEmpty())
		HttpMgr::Instance()->calc_json(arrAll);
}

void MDataPickder::trimStationStream(StationStream& stream)
{
	const int overflow = stream.sampleCount() - X_Axis_COUNT * m_nArrNum;
	if (overflow > 0)
		removeStationSamples(stream, overflow);
}

void MDataPickder::removeStationSamples(StationStream& stream, int sampleCount)
{
	const int removeSamples = qBound(0, sampleCount, stream.sampleCount());
	if (removeSamples <= 0)
		return;
	const int removeValues = removeSamples * CHANNEL_COUNT;
	stream.z.remove(0, qMin(removeValues, stream.z.size()));
	stream.x.remove(0, qMin(removeValues, stream.x.size()));
	stream.y.remove(0, qMin(removeValues, stream.y.size()));
	stream.firstSampleUs += static_cast<qint64>(removeSamples) * kSamplePeriodUs;
	if (stream.z.isEmpty())
		stream.firstSampleUs = 0;
}

QList<int> MDataPickder::participatingStations() const
{
	QList<int> stations = m_processingStations.values();
	std::sort(stations.begin(), stations.end());
	return stations;
}

QVector<int> MDataPickder::activeGlobalChannels() const
{
	QVector<int> channels;
	for (int station : participatingStations())
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch)
			channels.append(station * CHANNEL_COUNT + ch);
	return channels;
}

void MDataPickder::refreshStationTopology()
{
	QSet<int> validStations;
	for (int station : m_onlineStations)
		if (station >= 0 && station < m_nStationCount)
			validStations.insert(station);
	QSet<int> knownStations = m_processingStations;
	knownStations.unite(m_pendingJoinStations);
	if (validStations == knownStations)
		return;

	QSet<int> removedStations = knownStations;
	removedStations.subtract(validStations);
	QSet<int> addedStations = validStations;
	addedStations.subtract(knownStations);
	bool removedProcessingStation = false;
	for (int station : removedStations)
		removedProcessingStation = removedProcessingStation || m_processingStations.contains(station);
	for (int station : removedStations) {
		m_processingStations.remove(station);
		m_pendingJoinStations.remove(station);
	}
	if (m_processingStations.isEmpty())
		m_processingStations = validStations;
	else
		m_pendingJoinStations.unite(addedStations);
	m_syncRecoveryCount = 0;
	m_syncMismatchStartMs = 0;
	m_pendingJoinWarning = false;
	if (removedProcessingStation || m_syncState != 1) {
		resetCalculationState(false);
		setSyncState(0, 0, -1, QStringLiteral("在线站集合变化，等待拾取数据重新同步"));
	}
	else if (!addedStations.isEmpty()) {
		emit CallManage::getInstance()->sig_addLog("PickerSync",
			QStringLiteral("检测到新上线站，现有站继续计算，新站进入同步验证"));
	}
}

void MDataPickder::resetCalculationState(bool clearRawStreams)
{
	for (CHAN_DATA* data : m_vecCacheData)
		if (data) data->clearAll();
	if (clearRawStreams)
		m_stationStreams.clear();
	m_demo_eventp.prev_state = 0;
	m_nUnclosedPtNum = 0;
	m_lastSavedEventStartMs = -1;
	m_lastSavedEventEndMs = -1;
	m_channelThresholdWarning = false;
	m_alignmentEstablished = false;
}

void MDataPickder::setSyncState(int state, qint64 maxDiffUs, int stationId, const QString& message)
{
	m_lastSyncDiffUs = maxDiffUs;
	if (m_syncState == state)
		return;
	m_syncState = state;
	if (state == 2)
		resetCalculationState(false);
	emit CallManage::getInstance()->sig_pickerSyncStatus(state, maxDiffUs, stationId, message);
	emit CallManage::getInstance()->sig_addLog("PickerSync", message);
}

void MDataPickder::clearStationState(int stationId)
{
	m_stationStreams.remove(stationId);
}

void MDataPickder::clearAllState()
{
	m_onlineStations.clear();
	m_statusKnownStations.clear();
	m_processingStations.clear();
	m_pendingJoinStations.clear();
	m_syncRecoveryCount = 0;
	m_syncMismatchStartMs = 0;
	m_pendingJoinWarning = false;
	m_syncState = 0;
	resetCalculationState(true);
}

void MDataPickder::slot_staConnectStatus(int nid, bool connected)
{
	QMutexLocker locker(&m_mutex);
	if (nid < 0 || nid >= m_nStationCount)
		return;
	m_statusKnownStations.insert(nid);
	if (connected)
		m_onlineStations.insert(nid);
	else {
		m_onlineStations.remove(nid);
		clearStationState(nid);
	}
	refreshStationTopology();
	tryBuildAlignedData();
}

void MDataPickder::slot_loginOut()
{
	QMutexLocker locker(&m_mutex);
	clearAllState();
}

Q_INVOKABLE void MDataPickder::changeCallcValue(int ntag, QString strv)
{
	QMutexLocker locker(&m_mutex);

	QStringList slist = { "tri_on","tri_off","nsta","nlta","calc_win_len",
		"detect_ch",	//5
		" ",
		"qualified_ch", //7
		"energy_thre","amp_thre","intrach" ,"interch" };
	if (ntag >= 0 && ntag < slist.size())
	{
		AppConfig::Instance()->setConfig("Para", slist.at(ntag), strv);
	}

	// 信号开始阈值:
	if (0 == ntag)
	{
		m_demo_eventp.tri_on = strv.toDouble();
	}
	// 信号结束阈值::
	else if (1 == ntag)
	{
		m_demo_eventp.tri_off = strv.toDouble();
	}
	// 短窗长度(样点数)::
	else if (2 == ntag)
	{
		m_demo_eventp.nsta = strv.toInt();
	}
	// 长窗长度(样点数)::
	else if (3 == ntag)
	{
		m_demo_eventp.nlta = strv.toInt();
	}
	// 计算窗口长度(样点数)::
	else if (4 == ntag)
	{
		m_nCalcWin = strv.toInt();// +m_demo_eventp.nlta;
	}
	// 触发达标的最小通道数::
	else if (5 == ntag)
	{
		m_demo_eventp.detect_ch = strv.toInt();
	}
	// 振幅达标的最小通道数::
	//else if (6 == ntag)
	//{
	//	m_demo_eventp.M2 = strv.toInt();
	//}
	// 限制达标最小通道数:
	else if (7 == ntag)
	{
		m_demo_eventp.qualified_ch = strv.toInt();
	}
	// 能量限制:
	else if (8 == ntag)
	{
		m_demo_eventp.energy_thre = strv.toDouble();
	}
	// 强度限制:
	else if (9 == ntag)
	{
		m_demo_eventp.amp_thre = strv.toDouble();
	}
	// 单通道相位间隔点数:
	else if (10 == ntag)
	{
		m_demo_eventp.intrach_offset = strv.toDouble();
	}
	// 多通道相位间隔点数:
	else if (11 == ntag)
	{
		m_demo_eventp.interch_offset = strv.toDouble();
	}
}

Q_INVOKABLE void MDataPickder::setValue(int ndmx)
{
	m_nDmx = ndmx;
}

bool MDataPickder::callPickder()
{
// 	if (m_syncState != 1)
// 		return false;
	const QVector<int> activeChannels = activeGlobalChannels();
	if (activeChannels.isEmpty())
		return false;
	ChNum = activeChannels.size();
	if (m_demo_eventp.detect_ch > ChNum || m_demo_eventp.qualified_ch > ChNum)
		return false;

	// m_fdata 在构造函数中按该容量固定分配，算法窗口不能超过它。
	const int bufferCapacity = X_Axis_COUNT * m_nArrNum;
	if (bufferCapacity <= 0)
		return false;

	// 使用 64 位计算，避免异常配置值相加时 int 溢出。
	const qint64 requestedCalcWin = static_cast<qint64>(m_nCalcWin)
		+ static_cast<qint64>(m_demo_eventp.nlta)
		+ static_cast<qint64>(m_nUnclosedPtNum);
	const int nCalcWin = static_cast<int>(
		qBound<qint64>(1, requestedCalcWin, static_cast<qint64>(bufferCapacity)));

	if (requestedCalcWin != nCalcWin) {
// 		LOGMgr->addLog("MDataPickder",
// 			QString("计算窗口%1超出缓冲区范围，已限制为%2")
// 				.arg(requestedCalcWin).arg(nCalcWin));
	}

	QVector<double>& vecpPts0 = m_vecCacheData[activeChannels.first()]->getPoints();
	int npt0 = vecpPts0.size();

	// 诊断：各活动通道缓存长度必须一致，否则同一索引在各站对应的绝对时间不同，
	// 保存的mseed中各站波形会整体错位。出现差异立即记录告警。
	{
		int minPts = npt0;
		int maxPts = npt0;
		int minCh = activeChannels.first();
		int maxCh = activeChannels.first();
		for (int ch : activeChannels) {
			CHAN_DATA* cd = m_vecCacheData.value(ch, nullptr);
			if (!cd)
				continue;
			const int sz = cd->getPoints().size();
			if (sz < minPts) { minPts = sz; minCh = ch; }
			if (sz > maxPts) { maxPts = sz; maxCh = ch; }
		}
		if (maxPts - minPts > 0 && npt0 >= nCalcWin + 512 * 2) {
			const qint64 nowMs2 = QDateTime::currentMSecsSinceEpoch();
			if (nowMs2 >= m_nextCacheDiagMs) {
				m_nextCacheDiagMs = nowMs2 + 5000;
				emit CallManage::getInstance()->sig_addLog("PickerSync",
					QStringLiteral("缓存长度不一致：通道%1(%2点) 与 通道%3(%4点)，各站波形可能存在索引错位！")
						.arg(minCh + 1).arg(minPts).arg(maxCh + 1).arg(maxPts));
			}
		}
	}

	// 计算点X_Axis_COUNT, 前后1秒钟的数据
	if (npt0 < nCalcWin + 512 * 2)
	{
		// 诊断（5秒节流）：缓存累积进度，用于定位"拾取不启动"问题
		const qint64 diagNowMs = QDateTime::currentMSecsSinceEpoch();
		if (diagNowMs >= m_nextCacheDiagMs) {
			m_nextCacheDiagMs = diagNowMs + 5000;
			emit CallManage::getInstance()->sig_addLog("PickerSync",
				QStringLiteral("拾取等待缓存：首通道缓存%1点，需要%2点，活动通道%3，算法通道数%4")
					.arg(npt0).arg(nCalcWin + 512 * 2).arg(activeChannels.size()).arg(ChNum));
		}
		return false;
	}

	{
		for (int i = 0; i < MSEED_CHAN; i++)
		{
			memset(m_fdata[i], 0, sizeof(float)*X_Axis_COUNT * m_nArrNum);
		}

		int nstartIndex = -1;

		// 计算逻辑
		for (int j = 0; j < ChNum; ++j)
		{
			const int globalChannel = activeChannels.at(j);
			QVector<double>& vecpPts = m_vecCacheData[globalChannel]->getPoints();
			int npcount = vecpPts.size();

			int nidx = 0;
			for (int m = 512; m < npcount; m++)
			{
				if (nstartIndex < 0)
					nstartIndex = m;

				// nCalcWin 已被限制在 bufferCapacity 内，这里再做一层写入保护。
				if (nidx >= nCalcWin || nidx >= bufferCapacity)
					break;
				m_fdata[j][nidx] = vecpPts.at(m);

				//lstv << QString("%1|%2|%3").arg(j).arg(nidx).arg(QString::number(m_fdata[j][nidx]));
				nidx++;
			}
		}





		// 打开文件
// 		QFile file("D:/work/new/work/tmp/test.txt");
// 		if (!file.open(QIODevice::Append | QIODevice::Text)) {
// 			qCritical() << "无法打开文件写入";
// 			return -1;
// 		}
// 
// 		QTextStream out(&file);
// 		out.setCodec("UTF-8");
// 
// 		out << QStringLiteral("信号开始阈值 = ") << m_demo_eventp.tri_on << "\n";
// 		out << QStringLiteral("信号结束阈值 = ") << m_demo_eventp.tri_off << "\n";
// 		out << QStringLiteral("短窗点数 = ") << m_demo_eventp.nsta << "\n";
// 		out << QStringLiteral("长窗点数 = ") << m_demo_eventp.nlta << "\n";
// 		out << QStringLiteral("触发达标最小通道数 = ") << m_demo_eventp.detect_ch << "\n";
// 		out << QStringLiteral("强度限制 = ") << m_demo_eventp.amp_thre << "\n";
// 		out << QStringLiteral("能量限制 = ") << m_demo_eventp.energy_thre << "\n";
// 		out << QStringLiteral("限制达标最小通道数 = ") << m_demo_eventp.qualified_ch << "\n";
// 		out << QStringLiteral("采样率 = ") << m_demo_eventp.SF << "\n";
// 		out << QStringLiteral("通道内合并阈值点数 = ") << m_demo_eventp.intrach_offset << "\n";
// 		out << QStringLiteral("跨通道关联阈值点数 = ") << m_demo_eventp.interch_offset << "\n";
// 		out << QStringLiteral("上一个状态（当前状态） = ") << m_demo_eventp.prev_state << "\n";
// 
// 
// 		out << QStringLiteral("ChNum = ") << ChNum << "\n";
// 		out << QStringLiteral("nCalcWin = ") << nCalcWin << "\n";
// 
// 		for (int i = 0; i < m_fdata.size(); ++i) {
// 			// 注意：m_fdata[i] 必须已经被分配并赋值
// 			if (m_fdata[i] != nullptr) {
// 				out << QString("[%1] ").arg(*m_fdata[i]);
// 			}
// 			else {
// 				out << QString("m_fdata[%1] = (null)").arg(i) << "\n";
// 			}
// 		}
// 
// 		out << "\n";
// 
// 		file.close();
// 		qDebug() << "写入完成";


		//memset(m_StartWz, 0, sizeof(int) * ChNum);
		//memset(m_EndWz, 0, sizeof(int) * ChNum);

		// 默认删除1秒的数据
		int nrmOldNum = 512;
		//Event_info(m_demo_eventp, m_fdata, ChNum, nCalcWin, SF);
		//LOGMgr->addLog("Event_picker", QString("开始时间，ChNum:%1, PointNum:%2, SF:%3").arg(ChNum).arg(PointNum).arg(SF));
		//int inspire = Event_picker(m_demo_eventp, m_fdata, ChNum, nCalcWin, SF, m_StartWz, m_EndWz);

		if (AppConfig::Instance()->getConfig("Config", "full_log").toInt())
		{
			const QList<int> calcStations = participatingStations();
			for (int station : calcStations)
			{
				const int firstGlobalChannel = station * CHANNEL_COUNT;
				const CHAN_DATA* stationData = m_vecCacheData.value(firstGlobalChannel, nullptr);
				QString calcStartText = QStringLiteral("无有效时间");
				QString calcEndText = QStringLiteral("无有效时间");

				if (stationData && nstartIndex >= 0)
				{
					const QVector<qint64>& calcTimes = stationData->vecTimes;
					if (!calcTimes.isEmpty())
					{
						const int calcStartIndex = qBound(0, nstartIndex, calcTimes.size() - 1);
						const int calcEndIndex = qBound(0, nstartIndex + nCalcWin - 1,
							calcTimes.size() - 1);
						calcStartText = QDateTime::fromMSecsSinceEpoch(calcTimes.at(calcStartIndex))
							.toString("yyyy-MM-dd hh:mm:ss.zzz");
						calcEndText = QDateTime::fromMSecsSinceEpoch(calcTimes.at(calcEndIndex))
							.toString("yyyy-MM-dd hh:mm:ss.zzz");
					}
				}

				emit CallManage::getInstance()->sig_addLog("STALTA_Process ",
					QStringLiteral("站点:%1 通道:%2-%3 通道总数:%4 计算开始时间:%5 计算结束时间:%6")
					.arg(station + 1)
					.arg(firstGlobalChannel + 1)
					.arg(firstGlobalChannel + CHANNEL_COUNT)
					.arg(CHANNEL_COUNT)
					.arg(calcStartText)
					.arg(calcEndText));
			}
		}

		std::vector<std::pair<int, int>> finalPicks;
		int globalStart;
		int globalEnd;
		int inspire = STALTA_Process(m_fdata, ChNum, nCalcWin, m_demo_eventp, finalPicks, globalStart, globalEnd);
		if (m_nCount >= 5)
		{
			emit CallManage::getInstance()->sig_addLog("STALTA_Process ", QString("1# ChNum:%1 nCalcWin:%2 Return:%3").arg(ChNum).arg(nCalcWin).arg(inspire));
			m_nCount = 0;
		}
		m_nCount++;
		//LOGMgr->addLog("Event_picker", QString("结束时间 :%1-%2").arg(inspire).arg(k));
		if (inspire == 3)	// 异常
		{
			m_nUnclosedPtNum = 0;
			nrmOldNum = nCalcWin;
		}
		else if (inspire == 2)	// 有不闭合事件
		{
			m_nUnclosedPtNum += 512;
			//emit CallManage::getInstance()->sig_addLog("STALTA_Process ", QString("2# UnclosedPtNum:%1").arg(m_nUnclosedPtNum));
			nrmOldNum = 0;
			if (m_nUnclosedPtNum > 5000)
			{
				nrmOldNum = nCalcWin;
				m_nUnclosedPtNum = 0;
			}
			
		}
		else if (inspire == 1)	// 有至少一个闭合的事件 
		{
			m_nUnclosedPtNum = 0;
			// 取出红针最小点， 蓝针最大点
			int nstart = 9999999;
			int nend = 0;
			for (int i = 0; i < finalPicks.size(); i++)
			{
				int ntmps = finalPicks.at(i).first;
				int ntmpe = finalPicks.at(i).second;
				if (ntmps > 0)
				{
					if (ntmps < nstart)
						nstart = ntmps;
				}
				if (ntmpe > 0)
				{
					if (ntmpe > nend)
						nend = ntmpe;
				}
			}
			//emit CallManage::getInstance()->sig_addLog("STALTA_Process ", QString("3# globalStart:%1 globalEnd:%2 startEnd[%3-%4]")
			//	.arg(globalStart).arg(globalEnd).arg(nstart).arg(nend));
			//QStringList strtmp;
			//printf("The data has event\n");

			// 取出红针最小点， 蓝针最大点
			//for (int i = 0; i < ChNum; i++)
			//{
			//	if (m_StartWz[i] > 0)
			//	{
			//		if (m_StartWz[i] < nstart)
			//			nstart = m_StartWz[i];
			//	}
			//	if (m_EndWz[i] > 0)
			//	{
			//		if (m_EndWz[i] > nend)
			//			nend = m_EndWz[i];
			//	}
			//	//strtmp << QString("%1,%2").arg(m_StartWz[i]).arg(m_EndWz[i]);
			//}
			//strtmp << QString("MinMax[%1,%2] nstartIndex[%3]").arg(nstart).arg(nend).arg(nstartIndex);
			if (nstart == 9999999)
				nstart = 0;

			int saveStart = nstart + nstartIndex - 500;
			if (saveStart < 0)
				saveStart = 0;

			// remove(0, count) does not include the item at count.
			nrmOldNum = nend + nstartIndex + 1;

			int saveEnd = nrmOldNum + 500;
			if (saveEnd > npt0)
				saveEnd = npt0;

			// Deduplicate one physical event detected in adjacent 512-point windows.
			qint64 eventStartMs = -1;
			qint64 eventEndMs = -1;
			const QVector<qint64>& eventTimes =
				m_vecCacheData[activeChannels.first()]->getTimes();
			if (!eventTimes.isEmpty())
			{
				const int eventStartIndex = qBound(0, nstart + nstartIndex,
					eventTimes.size() - 1);
				const int eventEndIndex = qBound(0, nend + nstartIndex,
					eventTimes.size() - 1);
				eventStartMs = eventTimes.at(eventStartIndex);
				eventEndMs = eventTimes.at(eventEndIndex);
			}

			constexpr qint64 kSameEventGapMs = 1500;
			const bool duplicateEvent =
				m_lastSavedEventEndMs >= 0 && eventStartMs >= 0 &&
				eventStartMs <= m_lastSavedEventEndMs + kSameEventGapMs &&
				eventEndMs >= m_lastSavedEventStartMs - kSameEventGapMs;

			if (duplicateEvent)
			{
				m_lastSavedEventStartMs = qMin(m_lastSavedEventStartMs, eventStartMs);
				m_lastSavedEventEndMs = qMax(m_lastSavedEventEndMs, eventEndMs);
				qInfo() << "Skip duplicated picker event:"
					<< eventStartMs << eventEndMs;
			}
			else
			{

			// 诊断：输出事件红针位置各站的绝对时刻。若各站时刻差异明显(远大于2ms采样周期)，
			// 说明对应站的设备时间戳与采样内容存在固定偏移(设备时钟/GPS问题)，而非对齐逻辑问题。
			{
				QString stationTimesText;
				const QList<int> diagStations = participatingStations();
				for (int sta : diagStations) {
					CHAN_DATA* staData = m_vecCacheData.value(sta * CHANNEL_COUNT, nullptr);
					const QVector<qint64>& staTimes = staData ? staData->getTimes() : QVector<qint64>();
					if (staTimes.isEmpty())
						continue;
					const int idx = qBound(0, nstart + nstartIndex, staTimes.size() - 1);
					stationTimesText += QStringLiteral("[站%1:%2ms]")
						.arg(sta + 1)
						.arg(staTimes.at(idx));
				}
				emit CallManage::getInstance()->sig_addLog("STALTA_Process ",
					QStringLiteral("事件红针索引%1 各站绝对时刻：%2")
						.arg(nstart + nstartIndex).arg(stationTimesText));
			}

			// start end 前后加1000毫秒保存mseed,  256次采样，间隔2毫秒，所以前后1秒大约512个点
			int nsingleNum = (saveEnd - saveStart);

			// 统计总轨迹数：单分量通道1条(Z)，三分量通道3条(X/Y/Z)。如16通道=13单+3三 → 13+3*3=22
			int nTraceCnt = 0;
			for (int rc = 0; rc < ChNum; ++rc)
				nTraceCnt += (m_vecCacheData[activeChannels.at(rc)]->getThreeComponents() ? 3 : 1);

			int ndataCnt = nsingleNum * nTraceCnt;
			float* fsave = new float[ndataCnt];

			//strtmp << QString("nsoffset[%1],saveStart[%2],saveEnd[%3]").arg(nsoffset).arg(saveStart).arg(saveEnd);

			QJsonArray rArr;     // 红针位置(每条轨迹一个)
			QJsonArray bArr;     // 蓝针位置
			QJsonArray sArr;     // 信号状态
			QJsonArray gArr;     // 放大倍数
			QJsonArray nameArr;  // 每条轨迹的“通道号-分量号”，如 1-Z、2-X、2-Y、2-Z

			int nsidx = 0;
			for (int rc = 0; rc < ChNum; ++rc)
			{
				const int globalChannel = activeChannels.at(rc);
				const int chanNo = globalChannel + 1;   // 保留原始全局通道号(1-based)

				// 该通道的针位/状态/增益(三分量的 X、Y 与该通道 Z 相同)
				int ntmps = 0, ntmpe = 0;
				if (rc < (int)finalPicks.size())
				{
					ntmps = finalPicks.at(rc).first;
					ntmpe = finalPicks.at(rc).second;
				}
				int rposV = (ntmps > 0) ? (ntmps + nstartIndex - saveStart) : 0;
				int bposV = (ntmpe > 0) ? (ntmpe + nstartIndex - saveStart) : 0;
				int statusV = m_nChanStatus[globalChannel];
				int gainV = m_nGain[globalChannel];

				const QVector<double>& vecZ = m_vecCacheData[globalChannel]->getPoints();
				if (m_vecCacheData[globalChannel]->getThreeComponents())
				{
					// 三分量：按 X, Y, Z 顺序各写一条轨迹
					const QVector<double>& vecX = m_vecCacheData[globalChannel]->getPointsX();
					const QVector<double>& vecY = m_vecCacheData[globalChannel]->getPointsY();
					const QVector<double>* axes[3] = { &vecX, &vecY, &vecZ };
					const char* axisName[3] = { "X", "Y", "Z" };
					for (int a = 0; a < 3; ++a)
					{
						const QVector<double>& v = *axes[a];
						int npcount = v.size();
						for (int rp = saveStart; rp < saveEnd; rp++)
							fsave[nsidx++] = (rp < npcount) ? (float)v.at(rp) : 0.0f;
						nameArr.append(QString("%1-%2").arg(chanNo).arg(axisName[a]));
						rArr.append(rposV);
						bArr.append(bposV);
						sArr.append(statusV);
						gArr.append(gainV);
					}
				}
				else
				{
					// 单分量：仅 Z
					int npcount = vecZ.size();
					for (int rp = saveStart; rp < saveEnd; rp++)
						fsave[nsidx++] = (rp < npcount) ? (float)vecZ.at(rp) : 0.0f;
					nameArr.append(QString("%1-Z").arg(chanNo));
					rArr.append(rposV);
					bArr.append(bposV);
					sArr.append(statusV);
					gArr.append(gainV);
				}
			}

			// 开始拾取的时间
			qint64 nstartTime = 0;
			QString stime = "";
			QVector<qint64>& vecpTms0 = m_vecCacheData[0]->getTimes();
			int nidx = saveStart + nstartIndex;
			if (nidx >= 0 && nidx < vecpTms0.size()) {
				nstartTime = vecpTms0.at(nidx);
				//时间未包尾时间 -512毫秒
				nstartTime -= 512;
				QDateTime dt = QDateTime::fromMSecsSinceEpoch(nstartTime);
				bool valid = false;

				// 校验日期和时间差
				if (dt.isValid()) {
					QDate date = dt.date();
					if (date.isValid() && date.year() >= 2000 && date.year() <= 2100) {
						qint64 diffMs = qAbs(dt.msecsTo(QDateTime::currentDateTime()));
						if (diffMs <= 600 * 1000) {  // 10分钟内
							valid = true;
						}
					}
				}


				if (valid) {
					// 日期有效，直接使用
					stime = dt.toString("yyyyMMdd_hhmmsszzz");
				}
				else {
					// 日期无效 → 保留时分秒，日期用今天
					QDateTime localDt = dt.toLocalTime();
					if (localDt.isValid()) {
						QTime time = localDt.time();
						QDate date = QDate::currentDate();
						QDateTime newDt(date, time);
						QDateTime now = QDateTime::currentDateTime();
						if (newDt > now) {
							newDt = newDt.addDays(-1);
						}
						nstartTime = newDt.toMSecsSinceEpoch();
						nstartTime -= 512; 
						stime = QDateTime::fromMSecsSinceEpoch(nstartTime).toString("yyyyMMdd_hhmmsszzz");
					}
					else {
						QDateTime now = QDateTime::currentDateTime();
						nstartTime = now.toMSecsSinceEpoch();
						nstartTime -= 512;  
						stime = QDateTime::fromMSecsSinceEpoch(nstartTime).toString("yyyyMMdd_hhmmsszzz");
					}
				}
			}
			else {
				QDateTime now = QDateTime::currentDateTime();
				nstartTime = now.toMSecsSinceEpoch();
				nstartTime -= 512;  
				stime = QDateTime::fromMSecsSinceEpoch(nstartTime).toString("yyyyMMdd_hhmmsszzz");
			}

			//

			//QString txtfile = AppConfig::Instance()->getPickerPath() + QString("%1.txt").arg(stime);
			//QFile qf(txtfile);
			//if (qf.open(QIODevice::WriteOnly | QIODevice::Text)) {
			//	QTextStream out(&qf);
			//	out << strtmp.join("|");
			//}
			//qf.close();


			int packedrecords;
			int verbose = 0;
			QString msfile = AppConfig::Instance()->getTempPickerPath() + QString("%1.mseed").arg(stime);
			MS3Record* msr;

			msr = msr3_init(NULL);

			/* Populate MS3Record values */
#ifdef Q_OS_WIN
			strcpy_s(msr->sid, "FDSN:XX_TEXT__B_H_E");
#else
			// Linux/GCC 无 strcpy_s，用 strncpy(msr->sid 为 char[51]，串长19字节不会截断)
			strncpy(msr->sid, "FDSN:XX_TEXT__B_H_E", sizeof(msr->sid) - 1);
			msr->sid[sizeof(msr->sid) - 1] = '\0';
#endif
			msr->pubversion = 1;   // reclen 在 extra 头确定后再按需设置(见下)

//  			if (nstartTime < 1000000000000LL) {  
//  				nstartTime = QDateTime::currentMSecsSinceEpoch();
//  			}
//  			msr->starttime = (nstime_t)nstartTime;

			msr->starttime = nstartTime;// ms_timestr2nstime("2018-12-01T00:00:00.000000000");

			msr->samprate = SF;
			msr->encoding = DE_FLOAT32;

			msr->datasamples = fsave;  /* pointer to 32-bit integer data samples */
			msr->numsamples = ndataCnt;
			msr->sampletype = 'f';  /* declare data type to be 32-bit integers */

			QJsonObject jsonObj;
			//----------------------
			jsonObj.insert("cnum", nTraceCnt);					// 总轨迹数(单分量×1 + 三分量×3)
			jsonObj.insert("pnum", nsingleNum);					// 单条轨迹数据点数
			jsonObj.insert("rpos", rArr);						// 红针位置数组(每条轨迹一个)
			jsonObj.insert("bpos", bArr);						// 蓝针位置

			jsonObj.insert("status", sArr);						// 信号状态
			jsonObj.insert("name", nameArr);					// 每条轨迹的“通道号-分量号”，如 1-Z、2-X、2-Y、2-Z
			jsonObj.insert("gain", gArr);						// 放大倍数
			//jsonObj.insert("power", bArr);							// 供电情况
			jsonObj.insert("clock", m_stationGps.value(
				m_processingStations.isEmpty() ? 0 : *m_processingStations.constBegin(), 0));	// 时钟来源(参与计算的第一个站的GPS状态)
// 			QJsonObject seiObj;
// 			jsonObj.insert("sei", seiObj);								// 拾震器相关数据
			QJsonDocument doc;
			doc.setObject(jsonObj);

			QByteArray bt = doc.toJson(QJsonDocument::Compact);
			msr->extra = bt.data();
			msr->extralength = (uint16_t)bt.size();

			// 记录长度必须能容纳：固定头 + extra头(JSON) + 数据样本。
			// miniSEED3 每条记录头都含 extra；通道多时 JSON 增大，若 reclen 连记录头都放不下，
			// msr3_pack 返回 -1 且文件为空(这正是 reclen=512、50条轨迹时的现象)。
			// 这里按"单条记录装下全部数据"取 reclen；数据极大时退回1MB分多条记录(extra头每条都放得下)。
			qint64 needRec = 256 + (qint64)bt.size() + (qint64)ndataCnt * (qint64)sizeof(float);
			if (needRec > MAXRECLEN)
				needRec = (1 << 20);
			if (needRec < 512)
				needRec = 512;
			msr->reclen = (int)needRec;

			/* Write all data in MS3Record to output file, using MSF_FLUSHDATA flag */
			packedrecords = msr3_writemseed(msr, msfile.toStdString().c_str(), 1, MSF_FLUSHDATA, verbose);

			/* Disconnect datasamples pointer, otherwise msr3_free() will attempt to free() it */
			msr->datasamples = NULL;
			msr->extra = NULL;
			msr->extralength = 0;
			msr3_free(&msr);
			delete[]fsave;

			if (packedrecords > 0)
			{
				m_lastSavedEventStartMs = eventStartMs;
				m_lastSavedEventEndMs = eventEndMs;
				emit CallManage::getInstance()->sig_pickerNewFile(msfile);
			}
			else
			{
				qWarning() << "Failed to write picker mseed:" << msfile;
			}
			}
		}
		else {
			m_nUnclosedPtNum = 0;
			//printf("No event\n");
		}

		if (nrmOldNum > 0)
		{

			for (int j = 0; j < activeChannels.size(); ++j)
			{
				CHAN_DATA* channelData = m_vecCacheData.at(activeChannels.at(j));
				if (!channelData)
				{
					qCritical() << "Null cache channel:" << j;
					continue;
				}

				QVector<double>& z = channelData->getPoints();
				QVector<double>& x = channelData->getPointsX();
				QVector<double>& y = channelData->getPointsY();
				QVector<qint64>& times = channelData->getTimes();

				const int removeCount =
					qMin(nrmOldNum, z.size());

				if (removeCount <= 0)
					continue;

				z.remove(0, removeCount);
				x.remove(0, qMin(removeCount, x.size()));
				y.remove(0, qMin(removeCount, y.size()));
				times.remove(0, qMin(removeCount, times.size()));
			}

// 			for (int j = 0; j < MSEED_CHAN; ++j) {
// 
// 				// 移除过旧数据
// 				QVector<double>& vecpPts = m_vecCacheData[j]->getPoints();
// 				QVector<double>& vecpX = m_vecCacheData[j]->getPointsX();
// 				QVector<double>& vecpY = m_vecCacheData[j]->getPointsY();
// 				QVector<qint64>& vecpTms = m_vecCacheData[j]->getTimes();
// 				int npcount = vecpPts.size();
// 				const int removeCount = qMin(nrmOldNum, npcount);
// 				if (removeCount > 0)
// 				{
// 					vecpPts.remove(0, removeCount);
// 					vecpTms.remove(0, qMin(removeCount, vecpTms.size()));
// 					vecpX.remove(0, qMin(removeCount, vecpX.size()));
// 					vecpY.remove(0, qMin(removeCount, vecpY.size()));
// 				}
// 			}
		}
	}

	return true;
}

void MDataPickder::onCalcTimeout()
{
	QMutexLocker locker(&m_mutex);
	try
	{
		evaluateSyncCoverage(QDateTime::currentMSecsSinceEpoch());
		// 调用算法
		callPickder();
	}
	catch (const std::bad_alloc& e)
	{
		qCritical() << "callPickder bad_alloc:" << e.what();
		resetCalculationState(false);
		m_nUnclosedPtNum = 0;
	}
	catch (const std::exception& e)
	{
		qCritical() << "callPickder exception:" << e.what();
	}
}

void MDataPickder::slot_devStatus(int nrd, int nchan, int nstate, int typeBit)
{
	int ntmp = nrd * CHANNEL_COUNT + nchan;
	if (ntmp >= 0 && ntmp < MSEED_CHAN)
	{
		m_nChanStatus[ntmp] = nstate;
	}
}

void MDataPickder::slot_gpsStatus(int nstaid, int ngps)
{
	// ngps 5/6 表示时钟正常（与 SiteItemWidget::setGPSState 判定一致），其余视为时钟异常。
	if (nstaid < 0 || nstaid >= m_nStationCount)
		return;
	const bool abnormal = (ngps != 5 && ngps != 6);
	QMutexLocker locker(&m_mutex);
	m_stationGps[nstaid] = ngps;
	const bool wasAbnormal = m_gpsAbnormalStations.contains(nstaid);
	if (abnormal == wasAbnormal)
		return;
	if (abnormal) {
		m_gpsAbnormalStations.insert(nstaid);
		// 时钟异常：该站所有通道数据暂停参与拾取计算，等待时钟恢复后重新走同步验证加入。
		m_onlineStations.remove(nstaid);
		m_pendingJoinStations.remove(nstaid);
		m_processingStations.remove(nstaid);
		if (m_stationStreams.contains(nstaid))
			m_stationStreams[nstaid].clear();
		refreshStationTopology();
		setSyncState(2, 0, nstaid,
			QStringLiteral("站%1时钟异常(GPS状态%2)，暂停参与拾取计算，待时钟恢复").arg(nstaid + 1).arg(ngps));
	}
	else {
		m_gpsAbnormalStations.remove(nstaid);
		m_nextGpsDropLogMs[nstaid] = 0;
		emit CallManage::getInstance()->sig_addLog("PickerSync",
			QStringLiteral("站%1时钟恢复正常(GPS状态%2)，将重新参与拾取计算").arg(nstaid + 1).arg(ngps));
	}
}

void MDataPickder::slot_canRecv(int nstaid, int ntag, QList<QStringList> lstDt)
{
	//if (ntag == 0)
	{
		for (int i = 0; i < lstDt.size(); i++)
		{
			int ntmp = lstDt.at(i).at(0).toInt();
			int nch = nstaid * CHANNEL_COUNT + ntmp;
			if (nch > 0 && nch <= MSEED_CHAN)
			{
				m_nGain[nch - 1] = lstDt.at(i).at(2).toInt();
			}
		}

	}
}
