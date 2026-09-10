#include "FrameParser.h"
#include "Commdef.h"
#include "CallManage.h"
#include "LogManage.h"
#include "AppConfig.h"
#include "MDataPickder.h"
#include "MDataWriter.h"
#include <QtEndian>
#include <QDebug>
#include <QDateTime>
#include <limits>

namespace {
// 云端节点帧
constexpr int kCloudFrameSize = 4585;
constexpr int kCloudVibrationOffset = 77;
constexpr int kCloudSampleCount = 500;
constexpr int kCloudCrcOffset = 4577;
constexpr int kCloudTailOffset = 4581;

quint32 cloudCrc32(const uchar* data, int length)
{
	quint32 crc = 0xFFFFFFFFu;
	for (int i = 0; i < length; ++i) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
	}
	return crc ^ 0xFFFFFFFFu;
}

qint32 readInt24BE(const uchar* p)
{
	qint32 value = (static_cast<qint32>(p[0]) << 16)
		| (static_cast<qint32>(p[1]) << 8)
		| static_cast<qint32>(p[2]);
	if (value & 0x00800000)
		value |= static_cast<qint32>(0xFF000000);
	return value;
}

// 提取协议第21位（零基索引20）的分量类型位：0=单分量，1=三分量
int decodeTypeBit(const unsigned char* pointData)
{
	uint64_t value = 0;
	for (int i = 0; i < 8; ++i) {
		value |= static_cast<uint64_t>(pointData[i]) << (i * 8);
	}
	constexpr int TYPE_BIT_INDEX = 20;
	return static_cast<int>((value >> TYPE_BIT_INDEX) & 1);
}
}

FrameParser::FrameParser(QObject* parent)
	: QObject(parent)
{
	for (int sta = 0; sta < MAX_STATION; ++sta) {
		m_nGps[sta] = -1;
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
			m_nChanStatus[sta][ch] = -1;
			m_nChanType[sta][ch] = -1;
		}
	}
}

FrameParser::~FrameParser()
{
}

void FrameParser::onTcpData(int stationId, const QByteArray& data)
{
	if (stationId < 0 || stationId >= MAX_STATION)
		return;

	// 按帧长度区分协议：云端节点帧 与 局域网旧协议帧
	if (data.size() == kCloudFrameSize)
		parseCloudNodeFrame(stationId, data);
	else
		parseLanFrame(stationId, data);
}
bool FrameParser::parseLanFrame(int stationId, const QByteArray& data)
{
	const int ntag = stationId;
	const int dataSize = data.size();
	const int chunkSize = CHANNEL_COUNT * 8;      // 16 * 8
	const int channelStride = 8;
	const int expectedSize = 256 * chunkSize + 11;
	if (dataSize != expectedSize) {
		LOGMgr->addLog("FrameParser", QStringLiteral("Invalid TCP frame size:%1 expected:%2")
			.arg(dataSize).arg(expectedSize));
		return false;
	}
	const unsigned char* rawData = reinterpret_cast<const unsigned char*>(data.constData());
	const int tailOffset = dataSize - 11;         // 尾部11字节时间戳起始位置

	// ---------- 解析时间戳 ----------
	quint16 minutes = rawData[tailOffset + 2] | ((rawData[tailOffset + 3] & 0x0F) << 8);
	int hour = minutes / 60;
	int minute = minutes % 60;
	quint8 second = rawData[tailOffset + 4];
	quint8 msecLow = rawData[tailOffset + 5];
	quint8 msecHigh = rawData[tailOffset + 6];
	quint8 gps = (rawData[tailOffset + 3] & 0xF0) >> 4;
	if (gps != m_nGps[ntag]) {
		m_nGps[ntag] = gps;
		emit CallManage::getInstance()->sig_gpsStatus(ntag, m_nGps[ntag]);
	}
	quint16 msec = (msecHigh << 8) | msecLow;
	quint8 day = rawData[tailOffset + 7];
	quint8 month = rawData[tailOffset + 8];
	quint16 year = rawData[tailOffset + 9] | (rawData[tailOffset + 10] << 8);

	QTime time(hour, minute, second, msec);
	const QDate rawDate(year, month, day);
	const bool isPlaceholderDate = (year == 1970 && month == 1 && day == 1);
	// 离线环境本机时间不可信：时间或日期无效时直接丢弃该帧，禁止用本机时间校准。
	if (!time.isValid() || !rawDate.isValid() || isPlaceholderDate) {
		const qint64 currentMs = QDateTime::currentDateTime().toMSecsSinceEpoch();
		if (currentMs - m_nLastDateWarningTime >= 60000) {
			LOGMgr->addLog(QStringLiteral("GPS时间异常"),
				QStringLiteral("台站:%1 原始时间:%2:%3:%4.%5 原始日期:%6-%7-%8，时间或日期无效，本帧已丢弃（不使用本机时间校准）")
				.arg(ntag).arg(hour).arg(minute).arg(second).arg(msec)
				.arg(year, 4, 10, QChar('0'))
				.arg(month, 2, 10, QChar('0'))
				.arg(day, 2, 10, QChar('0')));
			m_nLastDateWarningTime = currentMs;
		}
		return false;
	}
	// 直接采用GPS原始日期+时间，不做任何基于本机时间的日期推断
	const QDateTime timestamp(rawDate, time);
	qint64 ntime = timestamp.toMSecsSinceEpoch();
	qint64 frameStartUs = ntime * 1000LL;
	// 解析出的Z/X/Y值（Z 喂算法；X/Y 三分量通道有效，随Z保存mseed）
	QVector<float> vecDt;
	vecDt.reserve(256 * CHANNEL_COUNT);
	QVector<float> vecX; vecX.reserve(256 * CHANNEL_COUNT);
	QVector<float> vecY; vecY.reserve(256 * CHANNEL_COUNT);
	QVector<float> vecComp(CHANNEL_COUNT, 0.0f);   // 每通道三分量标志(1=三分量)

	// ---------- 遍历256个采样点 ----------
	for (int i = 0; i < 256; ++i) {
		int baseOffset = i * chunkSize;
		if (baseOffset + CHANNEL_COUNT * 8 + 11 > dataSize) {
			qWarning() << "Incomplete packet at sample" << i;
			break;
		}
		const unsigned char* gData = rawData + baseOffset;
		for (int ch = 0; ch < CHANNEL_COUNT; ++ch) {
			const unsigned char* sData = gData + ch * channelStride;

			// ----- 1. 判断空点（全0xFF）-----
			bool isEmpty = true;
			for (int j = 0; j < 8; ++j) {
				if (sData[j] != 0xFF) {
					isEmpty = false;
					break;
				}
			}
			if (isEmpty) {
				// 空点：通道离线（按站去重）
				if (m_nChanStatus[ntag][ch] != 0 || m_nChanType[ntag][ch] != -1) {
					m_nChanStatus[ntag][ch] = 0;
					m_nChanType[ntag][ch] = -1;
					emit CallManage::getInstance()->sig_devStatus(ntag, ch, 0, -1);
				}
				vecDt.append(0.0f);
				vecX.append(0.0f);
				vecY.append(0.0f);
				continue;
			}

			// ----- 2. 提取类型位 -----
			const int typeBit = decodeTypeBit(sData);
			if (typeBit == -1) {
				qWarning() << "Invalid point data at ch" << ch << "sample" << i;
				continue;
			}

			// ----- 3. 根据类型位解析数值 -----
			int zRaw = 0, xRaw = 0, yRaw = 0;
			if (typeBit == 0) {
				// ========== 单分量（仅Z）==========
				BitUnion32 bitvalue;
				bitvalue.nvalue = 0;
				bitvalue.int8_v.uv1 = sData[0];
				bitvalue.int8_v.uv2 = sData[1];          // Z低8位
				BitUnion8 bit8;
				bit8.value = sData[2];
				bitvalue.bit_v.v16 = bit8.bit_v.v0;      // Z的第16位

				zRaw = bitvalue.nvalue;
				if (bit8.bit_v.v1 == 1)                  // 符号位
					zRaw = -zRaw;
			}
			else { // typeBit == 1 三分量（Z, X, Y）
				zRaw = ((sData[1] & 0x7F) << 8) | sData[0];
				if (sData[1] & 0x80) zRaw = -zRaw;
				xRaw = ((sData[5] & 0x7F) << 8) | sData[4];
				if (sData[5] & 0x80) xRaw = -xRaw;
				yRaw = ((sData[7] & 0x7F) << 8) | sData[6];
				if (sData[7] & 0x80) yRaw = -yRaw;
				vecComp[ch] = 1.0f;
			}

			vecDt.append(static_cast<float>(zRaw));
			vecX.append(static_cast<float>(xRaw));
			vecY.append(static_cast<float>(yRaw));

			// ----- 4. 设备状态（正常为1）-----
			if (m_nChanStatus[ntag][ch] != 1 || m_nChanType[ntag][ch] != typeBit) {
				m_nChanStatus[ntag][ch] = 1;
				m_nChanType[ntag][ch] = typeBit;
				emit CallManage::getInstance()->sig_devStatus(ntag, ch, 1, typeBit);
			}
		}
	}

	forwardData(ntag, vecDt, vecX, vecY, vecComp, frameStartUs);
	return true;
}

void FrameParser::forwardData(int stationId, const QVector<float>& z,
	const QVector<float>& x, const QVector<float>& y,
	const QVector<float>& compFlags, qint64 frameStartUs)
{
	// 连续存储：仅站0
	if (m_writer && stationId == 0) {
		QMetaObject::invokeMethod(m_writer, "addData",
			Qt::QueuedConnection,
			Q_ARG(QVector<float>, z));
	}
	// 算法：所有站都转发(Z/X/Y/三分量标志)，由 Picker 按时间戳对齐汇总
	if (m_pickder) {
		const int actualSampleCount = z.size() / CHANNEL_COUNT;
		QMetaObject::invokeMethod(m_pickder, "addData",
			Qt::QueuedConnection,
			Q_ARG(int, stationId),
			Q_ARG(QVector<float>, z),
			Q_ARG(QVector<float>, x),
			Q_ARG(QVector<float>, y),
			Q_ARG(QVector<float>, compFlags),
			Q_ARG(qint64, frameStartUs),
			Q_ARG(int, actualSampleCount),
			Q_ARG(int, 500),
			Q_ARG(quint64, static_cast<quint64>(0)));
	}
}

bool FrameParser::parseCloudNodeFrame(int stationId, const QByteArray& data)
{
	const uchar* raw = reinterpret_cast<const uchar*>(data.constData());
	if (raw[kCloudTailOffset] != 0x20 || raw[kCloudTailOffset + 1] != 0x24 ||
		raw[kCloudTailOffset + 2] != 0x11 || raw[kCloudTailOffset + 3] != 0x20) {
		LOGMgr->addLog("CloudNodeProtocol", QString("站%1 云端帧包尾错误").arg(stationId));
		return false;
	}

	// CRC 从偏移2计算4575字节，CRC字段按小端保存。
	const quint32 expectedCrc = qFromLittleEndian<quint32>(raw + kCloudCrcOffset);
	const quint32 actualCrc = cloudCrc32(raw + 2, kCloudCrcOffset - 2);
	if (expectedCrc != actualCrc) {
		LOGMgr->addLog("CloudNodeProtocol",
			QString("站%1 CRC32校验失败: recv=%2 calc=%3")
			.arg(stationId)
			.arg(expectedCrc, 8, 16, QLatin1Char('0'))
			.arg(actualCrc, 8, 16, QLatin1Char('0')));
		return false;
	}

	const quint64 packageNo = qFromLittleEndian<quint64>(raw + 10);
	const qint64 timestampUs = qFromLittleEndian<qint64>(raw + 18);
	const int sampleRateCode = raw[36];
	const int sampleRates[] = { 250, 500, 1000 };
	const int sampleRate = (sampleRateCode >= 0 && sampleRateCode < 3)
		? sampleRates[sampleRateCode] : 500;
	const qint64 frameStartUs = timestampUs > 0
		? timestampUs : QDateTime::currentMSecsSinceEpoch() * 1000LL;

	QVector<float> z(kCloudSampleCount * CHANNEL_COUNT, 0.0f);
	QVector<float> x(kCloudSampleCount * CHANNEL_COUNT, 0.0f); // X 对应协议 N
	QVector<float> y(kCloudSampleCount * CHANNEL_COUNT, 0.0f); // Y 对应协议 E
	QVector<float> components(CHANNEL_COUNT, 0.0f);
	components[0] = 1.0f;

	for (int sample = 0; sample < kCloudSampleCount; ++sample) {
		const uchar* sampleData = raw + kCloudVibrationOffset + sample * 9;
		const qint32 zValue = readInt24BE(sampleData);
		const qint32 nValue = readInt24BE(sampleData + 3);
		const qint32 eValue = readInt24BE(sampleData + 6);
		const int index = sample * CHANNEL_COUNT;
		z[index] = static_cast<float>(zValue);
		x[index] = static_cast<float>(nValue);
		y[index] = static_cast<float>(eValue);
	}

	if (m_nChanStatus[stationId][0] != 1 || m_nChanType[stationId][0] != 1) {
		m_nChanStatus[stationId][0] = 1;
		m_nChanType[stationId][0] = 1;
		emit CallManage::getInstance()->sig_devStatus(stationId, 0, 1, 1);
	}

	forwardData(stationId, z, x, y, components, frameStartUs);

	const QString deviceId = QString::fromLatin1(
		reinterpret_cast<const char*>(raw + 2), 8).trimmed();
	qDebug() << "Cloud node frame" << deviceId << "package" << packageNo
		<< "sampleRate" << sampleRate << "timestamp(us)" << timestampUs;
	return true;
}


