#include "watchdog.h"
#include <QDebug>
#include <QCoreApplication>
#include <QDateTime>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
WatchDog::WatchDog(const QString& program,
	const QStringList& arguments,
	int maxRestarts,
	int restartDelayMs,
	int heartbeatTimeoutMs,
	bool enableHeartbeat,
	quint16 heartbeatPort,
	QObject* parent)
	: QObject(parent)
	, m_process(new QProcess(this))
	, m_program(program)
	, m_baseArguments(arguments)
	, m_maxRestarts(maxRestarts)
	, m_restartDelayMs(restartDelayMs)
	, m_heartbeatTimeoutMs(heartbeatTimeoutMs)
	, m_restartCount(0)
	, m_stopping(false)
	, m_heartbeatEnabled(enableHeartbeat)      // [Modified]
	, m_heartbeatPort(heartbeatPort)           // [Modified]
	, m_heartbeatSocket(nullptr)
	, m_heartbeatTimer(new QTimer(this))
	, m_heartbeatReceived(false)
{
	connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
		this, &WatchDog::onProcessFinished);

	connect(m_heartbeatTimer, &QTimer::timeout, this, &WatchDog::checkHeartbeatTimeout);
}

void WatchDog::start()
{
	m_stopping = false;
	m_restartCount = 0;

	if (m_heartbeatEnabled) {
		startHeartbeatListener();    // 仅当启用时绑定端口
	}

	launchProcess(false);
}

void WatchDog::stop()
{
	m_stopping = true;
	stopHeartbeatListener();        // [Modified] 统一清理

	if (m_process->state() != QProcess::NotRunning) {
		m_process->terminate();
		if (!m_process->waitForFinished(5000)) {
			m_process->kill();
			m_process->waitForFinished(3000);
		}
	}
}
void WatchDog::launchProcess(bool restoreState)
{
	QStringList args = m_baseArguments;
	if (restoreState) {
		args << "--restore-state";
	}

	qInfo() << "[" << QDateTime::currentDateTime().toString("hh:mm:ss")
		<< "] Launching:" << m_program << args;

#ifdef Q_OS_WIN
	m_process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
		args->flags |= CREATE_NO_WINDOW;
		});
#endif

	m_process->start(m_program, args);

	// 只有启用心跳且进程启动后才启动心跳定时器
	if (m_heartbeatEnabled) {
		m_heartbeatReceived = false;
		m_heartbeatTimer->start(m_heartbeatTimeoutMs);
	}

	emit processStarted();
}
void WatchDog::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
	// 进程退出，立即停止心跳检测
	if (m_heartbeatEnabled) {
		m_heartbeatTimer->stop();
		m_heartbeatReceived = false;
	}

	if (m_stopping) {
		emit processStopped();
		return;
	}

	bool shouldRestart = false;
	bool restoreState = false;

	if (exitStatus == QProcess::CrashExit) {
		qWarning() << "Process crashed!";
		shouldRestart = true;
		restoreState = true;
	}
	else {
		if (exitCode == 100) {
			qInfo() << "Process requested restart (exit 100)";
			// 按你的业务，这里不重启（保留原逻辑）
			shouldRestart = false;
			restoreState = false;
		}
		else {
			qInfo() << "Process exited normally with code:" << exitCode;
		}
	}

	if (shouldRestart) {
		if (m_maxRestarts > 0 && m_restartCount >= m_maxRestarts) {
			qCritical() << "Max restart attempts reached, giving up.";
			emit processStopped();
			return;
		}

		m_restartCount++;
		int remaining = m_maxRestarts > 0 ? m_maxRestarts - m_restartCount : -1;
		emit restartScheduled(remaining);

		bool rs = restoreState;
		QTimer::singleShot(m_restartDelayMs, this, [this, rs]() {
			if (!m_stopping) {
				qInfo() << "Restarting process... (attempt" << m_restartCount << ")";
				launchProcess(rs);
			}
			});
	}
	else {
		emit processStopped();
	}
}

void WatchDog::restartProcess()
{
    if (m_stopping)
        return;
    qInfo() << "Restarting process... (attempt" << m_restartCount << ")";
    launchProcess();
}

void WatchDog::onHeartbeatReceived()
{
	while (m_heartbeatSocket->hasPendingDatagrams()) {
		QByteArray datagram;
		datagram.resize(m_heartbeatSocket->pendingDatagramSize());
		m_heartbeatSocket->readDatagram(datagram.data(), datagram.size());
		m_heartbeatReceived = true;
		// 重置超时定时器（收到心跳即重新计时）
		m_heartbeatTimer->start(m_heartbeatTimeoutMs);
	}
}
void WatchDog::checkHeartbeatTimeout()
{
	// [Modified] 进程未运行或已退出，绝不处理超时
	if (m_process->state() == QProcess::NotRunning) {
		return;
	}

	if (!m_heartbeatReceived) {
		qWarning() << "Heartbeat timeout! Killing process...";
		emit heartbeatLost();
		m_process->kill();
	}
	else {
		m_heartbeatReceived = false;
	}
}

void WatchDog::stopHeartbeatListener()
{
	if (m_heartbeatTimer) {
		m_heartbeatTimer->stop();
	}
	if (m_heartbeatSocket) {
		m_heartbeatSocket->close();
		delete m_heartbeatSocket;
		m_heartbeatSocket = nullptr;
	}
	m_heartbeatReceived = false;
}

void WatchDog::startHeartbeatListener()
{
	if (m_heartbeatSocket) {
		delete m_heartbeatSocket;
		m_heartbeatSocket = nullptr;
	}
	m_heartbeatSocket = new QUdpSocket(this);
	if (!m_heartbeatSocket->bind(QHostAddress::LocalHost, m_heartbeatPort)) {
		qCritical() << "Failed to bind heartbeat socket:" << m_heartbeatSocket->errorString();
		return;
	}
	connect(m_heartbeatSocket, &QUdpSocket::readyRead, this, &WatchDog::onHeartbeatReceived);
	qInfo() << "Heartbeat listener started on port" << m_heartbeatPort;
}