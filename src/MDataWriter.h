#ifndef MDataWriter_H
#define MDataWriter_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QFile>
#include <QDateTime>
#include <QMutex>
#include <QTimer>
#include <libmseed.h>
#include "Commdef.h"

typedef struct DataSHeader
{
	DataSHeader() :fSamprate(0.0f), nSampleCnt(0), nStartTime(0), nWriteTime(0), nClockStatus(0), nVersion(2) {}
	//采样率，样本总数，开始时间 时钟状态 版本
	float	fSamprate;		// 采样率
	int		nSampleCnt;		// 样本总数
	qint64	nStartTime;		// 开始时间
	qint64	nWriteTime;		// 当前写入时间
	qint8	nClockStatus;	// 时钟状态
	qint8	nVersion;		// 数据版本
}DATAS_HEADER;

class MDataWriter : public QObject
{
	Q_OBJECT

public:
	explicit MDataWriter(QObject* parent = nullptr);
	~MDataWriter();

	Q_INVOKABLE void startWrite();
	Q_INVOKABLE void stopWrite();
	// 添加数据点
	Q_INVOKABLE void addData(const QVector<float>& values);
	// 测试读取文件
	Q_INVOKABLE void startRead(QString strFile);

	// 手动刷新缓冲区到文件
	void flush();

	// 配置方法
	void setBufferSize(int maxSamples);
	void setFlushInterval(int milliseconds);

	QString getNewFileName();

	void updateFileName();

	void createFile();
	void openFile();


	void readTestFile();

signals:
	void errorOccurred(const QString& errorMessage);
	void dataWritten(int samplesWritten);

	void sigReadData(QVector<float> vec);

public slots:
	// 初始化配置
	bool initialize();

private slots:
	void onFlushTimeout();
	void onCheckDateChange();
	void onReadTimeout();

private:
	bool writeToFile();
	void cleanup();
	QString generateSID() const;

	QFile* m_pFile = NULL;
	DataSHeader* m_pHeader = NULL;

	QMutex m_mutex;
	QTimer* m_dataTimer;
	QTimer* m_checkTimer;

	QTimer* m_readTimer = NULL;

	QString m_strPath;
	QString m_strCurrentDate;
	QString m_strFileName;

	QString m_filename;
	QString m_network;
	QString m_station;
	QString m_location;
	QString m_channel;
	double m_sampleRate;

	QVector<float> m_buffer;
	int m_maxBufferSize;
	int m_nLen = 0;
	float* m_fFileData = NULL;
	int m_nReadCount = 0;
	int m_nSingleNum = CHANNEL_COUNT * 256 * 2;
	// 开始读取的秒数,读取此处凌晨4点
	int m_nStartReadTime = 60 * 60 * 4 + 23 * 60;

	nstime_t m_startTime;
	int64_t m_sampleCount;
	bool m_initialized;
	bool m_isWriting;
};

#endif // MDataWriter_H