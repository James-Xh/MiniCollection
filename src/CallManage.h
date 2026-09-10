#ifndef CALLMANAGE_H
#define CALLMANAGE_H

#include <QObject>
#include <QMap>
#include <QList>
#include <QPoint>

#include <QColor>
#include <QMutex>

#define HEX(x) QString::number(x, 16).toUpper()
class CallManage : public QObject
{
	Q_OBJECT
private:
	explicit CallManage(QObject *parent = NULL);
	~CallManage();

public:
	static CallManage* getInstance();

signals:
    void sig_tcpData(int ntag, QByteArray data);

	void sig_canRecv(int nstaid, int ntag, QList<QStringList> lst);

	void sig_calcChangeValue(int ntag, QString strV);

	void sig_devStatus(int nrd, int nchan, int nstate, int typeBit);

	void sig_canStatus(int nstaid, bool biscnt);
	void sig_gpsStatus(int nstaid, int ngps);

	// nid，台站ID， biscnt:是否连接
	void sig_staConnectStatus(int nid, bool biscnt);
	// 拾取到事件，产生新的mseed
	void sig_pickerNewFile(QString strfile);

	void sig_addLog(QString stype, QString sdesc);

	// 拾取数据同步状态：0=等待数据，1=同步正常，2=同步异常。
	// maxDiffUs 为最近一次观测到的最大包首时间差，stationId 为相关站号（-1 表示全局）。
	void sig_pickerSyncStatus(int state, qint64 maxDiffUs, int stationId, QString message);

	void sig_loginInToRole(int role);

	void sig_loginOutClear();

	void sig_dataCompletion(bool autosave);

public:
    Q_DISABLE_COPY(CallManage);
};

#endif // CALLMANAGE_H
