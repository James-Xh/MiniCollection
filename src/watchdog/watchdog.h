#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QUdpSocket>
#include <QStringList>

class WatchDog : public QObject
{
	Q_OBJECT
public:
	explicit WatchDog(const QString& program,
		const QStringList& arguments = {},
		int maxRestarts = 10,
		int restartDelayMs = 5000,
		int heartbeatTimeoutMs = 10000,
		bool enableHeartbeat = false,           // 新增：默认关闭
		quint16 heartbeatPort = 12345,          // 新增：可配置端口
		QObject* parent = nullptr);

	void start();       // 启动监控
	void stop();        // 优雅停止（不再重启）

	// 运行时动态启用心跳（可选）
	void setHeartbeatEnabled(bool enabled, quint16 port = 12345);

signals:
	void processStarted();
	void processStopped();          // 进程最终停止（不再重启）
	void restartScheduled(int remainingRetries);
	void heartbeatLost();           // 心跳丢失（即将强杀）

private slots:
	void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
	void restartProcess();
	void onHeartbeatReceived();
	void checkHeartbeatTimeout();

private:
	void launchProcess(bool restoreState = false);
	void startHeartbeatListener();
	void stopHeartbeatListener();   // 新增：统一清理

	QProcess* m_process;
	QString m_program;
	QStringList m_baseArguments;

	int m_maxRestarts;
	int m_restartDelayMs;
	int m_heartbeatTimeoutMs;
	int m_restartCount;
	bool m_stopping;

	// 心跳相关
	bool m_heartbeatEnabled;        // 新增
	quint16 m_heartbeatPort;        // 新增
	QUdpSocket* m_heartbeatSocket;
	QTimer* m_heartbeatTimer;
	bool m_heartbeatReceived;
};

#endif // WATCHDOG_H