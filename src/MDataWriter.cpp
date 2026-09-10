#include "MDataWriter.h"
#include <QDebug>
#include <QDir>
#include <cmath>
#include <QDateTime>
#include "AppConfig.h"
#include "LogManage.h"


MDataWriter::MDataWriter(QObject* parent)
	: QObject(parent)
	, m_dataTimer(new QTimer(this))
	, m_sampleRate(0)
	, m_maxBufferSize(1000) // 默认缓冲区大小
	, m_sampleCount(0)
	, m_initialized(false)
	, m_isWriting(false)
{
	connect(m_dataTimer, &QTimer::timeout, this, &MDataWriter::onFlushTimeout);
	m_dataTimer->setInterval(1000); // 默认1秒刷新一次

	m_checkTimer = new QTimer(this);
	connect(m_checkTimer, &QTimer::timeout, this, &MDataWriter::onCheckDateChange);
	m_checkTimer->setInterval(1000 * 10);// (1000 * 60); // 默认1分钟刷新一次

	m_readTimer = new QTimer(this);
	connect(m_readTimer, &QTimer::timeout, this, &MDataWriter::onReadTimeout);
	m_readTimer->setInterval(500); // 默认1秒刷新一次

	m_pHeader = new DataSHeader;
	m_pHeader->fSamprate = 500.0f;

}

MDataWriter::~MDataWriter()
{
	stopWrite();
	cleanup();
}

bool MDataWriter::initialize()
{
	QMutexLocker locker(&m_mutex);

	m_sampleRate = 500;

	if (!m_pHeader)
		m_pHeader = new DataSHeader;
	else
		*m_pHeader = DataSHeader();
	m_pHeader->fSamprate = m_sampleRate;

	m_strPath = APPCfg->getBinsPath();
	m_initialized = true;
	m_buffer.clear();
	m_sampleCount = 0;

	// 缓冲区固定为10秒（采样率500Hz × 10秒 = 5000样本），
	// 不再从配置读取（Para/differen 已改用于拾取同步容差）。
	m_maxBufferSize = m_sampleRate * 10;
	m_dataTimer->setInterval(3000);

	return true;
}

void MDataWriter::setBufferSize(int maxSamples)
{
	QMutexLocker locker(&m_mutex);
	m_maxBufferSize = maxSamples;
}

void MDataWriter::setFlushInterval(int milliseconds)
{
	QMutexLocker locker(&m_mutex);
	m_dataTimer->setInterval(milliseconds);
} 
 
QString MDataWriter::getNewFileName()
{
	return QDateTime::currentDateTime().toString("yyyy-MM-dd");
}

void MDataWriter::addData(const QVector<float>& values)
{
	{
		QMutexLocker locker(&m_mutex);
		if (!m_initialized) {
			emit errorOccurred("Writer not initialized");
			return;
		}
	}

	if (!m_pFile || !m_pFile->isOpen())
		createFile();

	QMutexLocker locker(&m_mutex);
	if (!m_pFile || !m_pFile->isOpen() || !m_pHeader) {
		emit errorOccurred("Data file is not available");
		return;
	}

	m_buffer.append(values);
	m_sampleCount += values.size();

	// 如果缓冲区达到最大大小，立即写入
	if (m_buffer.size() >= m_maxBufferSize) {
		locker.unlock();
		writeToFile();

	}
}

Q_INVOKABLE void MDataWriter::startRead(QString strFile)
{
	m_strFileName = strFile;

	m_readTimer->start();
}

void MDataWriter::startWrite()
{
	QMutexLocker locker(&m_mutex);

	if (!m_initialized) {
		emit errorOccurred("Cannot start - writer not initialized");
		return;
	}

	updateFileName();
	QString spath = m_strPath + m_strFileName;
	// 检查文件是否已存在（处理多次启动）
	bool fileExists = QFile::exists(spath);
	if (fileExists)
	{
		m_pFile = new QFile(spath);
		if (!m_pFile->open(QIODevice::ReadWrite)) {
			qWarning() << "Cannot open file:" << m_pFile->errorString();
			delete m_pFile;
			m_pFile = nullptr;
		}
		else 
		{
			qDebug() << "reOpened file:" << m_pFile->fileName();

			LOGMgr->addLog("MDataWriter", QString("存在文件，打开文件补全数据：%1").arg(m_pFile->fileName()));

			m_pFile->read((char*)m_pHeader, sizeof(DataSHeader));
 
			if (m_pHeader->nWriteTime > 0)
			{
				// 离线秒
				int noffSec = (QDateTime::currentMSecsSinceEpoch() - m_pHeader->nWriteTime) / 1000;
				int nwriteNum = noffSec * (256 * 16 * 2);// 256次采样相当于512毫秒，16个通道，*2=1秒钟数据量
				LOGMgr->addLog("MDataWriter", QString("离线秒:%1 补全数据点:%2")
					.arg(noffSec)
					.arg(nwriteNum));

				locker.unlock();
				int nstep = 3600;
				while (noffSec > 0)
				{
					int nfill = nstep < noffSec ? nstep : noffSec;
					nwriteNum = nfill * (256 * 16 * 2);// 256次采样相当于512毫秒，16个通道，*2=1秒钟数据量
					m_buffer.resize(nwriteNum);
					m_buffer.fill(0.0f);
					writeToFile();

					noffSec -= nfill;
				}


			}
		}
	}
	else
	{
		m_strCurrentDate = "";
		m_strFileName = "";
	}

	m_isWriting = true;
	m_dataTimer->start();
	m_checkTimer->start();
	qDebug() << "MDataWriter started for file:" << m_filename;
}

void MDataWriter::stopWrite()
{
	if (m_pFile == NULL)
		return;

	QMutexLocker locker(&m_mutex);

	m_dataTimer->stop();
	m_checkTimer->stop();
	m_isWriting = false;

	// 写入剩余数据
	if (!m_buffer.isEmpty()) {
		locker.unlock();
		writeToFile();
	}
	m_pFile->close();
	delete m_pFile;
	m_pFile = NULL;
	qDebug() << "MDataWriter stopped";
}

void MDataWriter::flush()
{
	QMutexLocker locker(&m_mutex);

	if (!m_buffer.isEmpty()) {
		locker.unlock();
		writeToFile();
	}
}

void MDataWriter::onFlushTimeout()
{
	QMutexLocker locker(&m_mutex);

	if (!m_buffer.isEmpty()) {
		locker.unlock();
		writeToFile();
	}
}

void MDataWriter::onCheckDateChange()
{
	createFile();
}


void MDataWriter::onReadTimeout()
{
	m_buffer.clear();
	if (m_strFileName.length() > 0)
	{
		if (m_fFileData)
		{
			delete[]m_fFileData;
			m_fFileData = NULL;
		}
		m_fFileData = new float[m_nSingleNum];

		if (m_pFile)
		{
			m_pFile->close();
			delete m_pFile;
			m_pFile = NULL;
		}
		if (m_pHeader)
		{
			delete m_pHeader;
			m_pHeader = NULL;
		}
		m_pFile = new QFile(m_strFileName);
		if (!m_pFile->open(QIODevice::ReadOnly)) {
			delete m_pFile;
			m_pFile = nullptr;
		}
		else
		{
			m_pHeader = new DataSHeader;
			m_pFile->read((char*)m_pHeader, sizeof(DataSHeader));

			m_nLen = m_pFile->size() - sizeof(DataSHeader);
		}
		m_strFileName = "";
	}
	else
	{
		if (!m_pFile || !m_pFile->isOpen() || !m_pHeader || !m_fFileData) {
			emit errorOccurred("Read file is not available");
			m_readTimer->stop();
			return;
		}
		int nUnCount = m_pHeader->nSampleCnt - m_nReadCount;
		int nRNum = m_nSingleNum;
		if (nUnCount < m_nSingleNum)
		{
			nRNum = nUnCount;
		}
		if (m_nStartReadTime > 0)
		{
			int nptSize = m_nStartReadTime * 512 * sizeof(float) + sizeof(DataSHeader);
			m_pFile->seek(nptSize);
			m_nStartReadTime = 0;
		}
		qDebug() << "offset:" << m_pFile->pos();
		m_pFile->read((char*)m_fFileData, nRNum * sizeof(float));

		for (int i = 0; i < nRNum; i++)
		{
			m_buffer << m_fFileData[i];
		}
		if (m_buffer.length() > 0)
		{
			emit sigReadData(m_buffer);
		}
	}
}

void MDataWriter::createFile()
{
	QString today = getNewFileName();
	if (today != m_strCurrentDate) {
		updateFileName();

		LOGMgr->addLog("MDataWriter", QString("createFile:%1")
			.arg(m_strFileName));

		openFile();
	}
}

void MDataWriter::updateFileName() {
	m_strCurrentDate = getNewFileName();
	m_strFileName = QString("data_%1.bin").arg(m_strCurrentDate);
}

void MDataWriter::openFile() {
	// 关闭旧文件
	if (m_pFile && m_pFile->isOpen()) {

		LOGMgr->addLog("MDataWriter", QString("openFile关闭文件:%1")
			.arg(m_pFile->fileName()));

		this->writeToFile();
		m_pFile->close();
		delete m_pFile;
	}

	// 创建新文件
	m_pFile = new QFile(m_strPath + m_strFileName);
	if (!m_pFile->open(QIODevice::WriteOnly | QIODevice::Append)) {
		qWarning() << "Cannot open file:" << m_pFile->errorString();
		delete m_pFile;
		m_pFile = nullptr;
	}
	else {
		qDebug() << "Opened file:" << m_pFile->fileName();
	}

	LOGMgr->addLog("MDataWriter", QString("openFile:%1")
		.arg(m_strFileName));

	m_startTime = QDateTime::currentMSecsSinceEpoch();
	m_pHeader->nStartTime = m_startTime;
	m_pHeader->nWriteTime = 0;
	m_pHeader->nSampleCnt = 0;
}

void MDataWriter::readTestFile()
{

}

bool MDataWriter::writeToFile()
{
	QMutexLocker locker(&m_mutex);

	if (m_buffer.isEmpty()) {
		return true;
	}
	if (!m_pFile || !m_pFile->isOpen() || !m_pHeader) {
		emit errorOccurred("Cannot write: data file is not open");
		return false;
	}

	int nDataNum = m_buffer.size();
	m_pHeader->nSampleCnt += nDataNum;
	m_pHeader->nWriteTime = QDateTime::currentMSecsSinceEpoch();

	m_pFile->seek(0);
	m_pFile->write((const char*)m_pHeader, sizeof(DataSHeader));

	// 分配数据内存并复制数据
	//float* data = new float[nDataNum];
	//memcpy(data, m_buffer.constData(), nDataNum * sizeof(float));

	//内容
	m_pFile->seek(m_pFile->size());
	//m_pFile->write((const char*)data, nDataNum * sizeof(float));
	const qint64 expectedBytes = static_cast<qint64>(nDataNum) * sizeof(float);
	qint64 qlen = m_pFile->write((const char*)m_buffer.constData(), expectedBytes);
	if (qlen != expectedBytes) {
		emit errorOccurred(QString("Incomplete file write: %1/%2 bytes")
			.arg(qlen).arg(expectedBytes));
		return false;
	}
	
	m_buffer.clear();
	//delete[] data;

	locker.unlock();
	//emit dataWritten(samplesWritten);

	return true;
}

QString MDataWriter::generateSID() const
{
	return QString("FDSN:%1_%2_%3_%4")
		.arg(m_network)
		.arg(m_station)
		.arg(m_location)
		.arg(m_channel);
}

void MDataWriter::cleanup()
{
	QMutexLocker locker(&m_mutex);

	m_buffer.clear();
	m_initialized = false;
	m_isWriting = false;

	if (m_dataTimer->isActive()) {
		m_dataTimer->stop();
	}
	if (m_readTimer && m_readTimer->isActive())
		m_readTimer->stop();

	delete[] m_fFileData;
	m_fFileData = nullptr;
	delete m_pHeader;
	m_pHeader = nullptr;
}
