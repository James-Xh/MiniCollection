#include "TcpConnection.h"
#include <QNetworkProxy>
#include <QDateTime>
#include "LogManage.h"

TcpConnection::TcpConnection(int stationId, const QString &ip, quint16 port, QObject *parent)
    : QObject(parent), m_stationId(stationId), m_ip(ip), m_port(port)
{
    m_checkTimer = new QTimer(this);
    m_checkTimer->setInterval(5000);
    connect(m_checkTimer, &QTimer::timeout, this, &TcpConnection::onCheckTimeout);
}

TcpConnection::~TcpConnection()
{
    stop();
}

void TcpConnection::start()
{
    if (m_socket) return;
    m_stopping = false;
    attemptReconnect();
    m_checkTimer->start();
}

void TcpConnection::stop()
{
	const bool wasConnected = m_isConnected;
	m_stopping = true;
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
        delete m_reconnectTimer;
        m_reconnectTimer = nullptr;
    }
    if (m_checkTimer) {
        m_checkTimer->stop();
    }
    if (m_socket) {
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
	m_isConnected = false;
	if (wasConnected)
		emit connectionChanged(m_stationId, false);
	m_buffer.clear();
}

void TcpConnection::setupSocketSignals()
{
    connect(m_socket, &QTcpSocket::readyRead, this, &TcpConnection::onReadyRead);
    connect(m_socket, &QTcpSocket::connected, this, &TcpConnection::onConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &TcpConnection::onDisconnected);
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::errorOccurred),
            this, &TcpConnection::onSocketError);
}

void TcpConnection::onReadyRead()
{
    m_buffer.append(m_socket->readAll());
    m_lastDataTime = QDateTime::currentSecsSinceEpoch();
    parseData();
}

void TcpConnection::parseData()
{
    constexpr int kFrameLength = 32779;
    if (m_buffer.size() < kFrameLength)
        return;

    const QByteArray frameEnd = QByteArray::fromHex("d5ea");
    while (true) {
        // 局域网旧协议：按 D5EA + 尾部共11字节分帧。
        int endIdx = m_buffer.indexOf(frameEnd);
        if (endIdx == -1) {
            // 没有完整帧尾，等下一次
            break;
        }
        int frameLen = endIdx + 11;  // 帧尾占11字节
        if (m_buffer.size() < frameLen) {
            // 数据不够，等下一包
            break;
        }
        QByteArray frame = m_buffer.left(frameLen);
        m_buffer.remove(0, frameLen);
        emit frameReady(m_stationId, frame);
    }
    // 防止缓冲区过大，若超过1MB且没有帧尾则清空（避免内存泄漏）
    if (m_buffer.size() > 1024 * 1024) {
        LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 缓冲区过大，清空").arg(m_stationId));
        m_buffer.clear();
    }
}

void TcpConnection::onConnected()
{
    m_isConnected = true;
    // 从新连接建立时重新计时，避免上一条连接的时间戳导致立即超时。
    m_lastDataTime = QDateTime::currentSecsSinceEpoch();
    m_reconnectAttempts = 0;
    stopReconnectTimer();
    emit connectionChanged(m_stationId, true);
    LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 连接成功 %2:%3").arg(m_stationId).arg(m_ip).arg(m_port));
}

void TcpConnection::onDisconnected()
{
    const bool wasConnected = m_isConnected;
    m_isConnected = false;
    if (wasConnected)
        emit connectionChanged(m_stationId, false);
    LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 断开连接").arg(m_stationId));
    if (!m_stopping)
        startReconnectTimer();
}

void TcpConnection::onSocketError(QAbstractSocket::SocketError socketError)
{
    if (!m_socket || m_stopping)
        return;

    LOGMgr->addLog("TcpConnection",
        QStringLiteral("站%1 Socket错误(%2) %3:%4 - %5")
                       .arg(m_stationId)
                       .arg(static_cast<int>(socketError))
                       .arg(m_ip)
                       .arg(m_port)
                       .arg(m_socket->errorString()));

    // 连接失败不一定会触发 disconnected，因此在错误信号中也保底安排重连。
	if (m_isConnected) {
		m_isConnected = false;
		emit connectionChanged(m_stationId, false);
	}
	startReconnectTimer();
}

void TcpConnection::startReconnectTimer()
{
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, &TcpConnection::onReconnectTimeout);
    }
    if (m_stopping || m_reconnectTimer->isActive())
        return;

    m_reconnectAttempts++;
    LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 %2秒后重连，次数%3")
                   .arg(m_stationId).arg(5).arg(m_reconnectAttempts));
    m_reconnectTimer->start(5000);
}

void TcpConnection::stopReconnectTimer()
{
    if (m_reconnectTimer && m_reconnectTimer->isActive())
        m_reconnectTimer->stop();
}

void TcpConnection::onReconnectTimeout()
{
    attemptReconnect();
}

void TcpConnection::attemptReconnect()
{
    if (m_stopping)
        return;

    if (m_socket) {
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_socket = new QTcpSocket(this);
    setupSocketSignals();
    m_buffer.clear();
    m_socket->setProxy(QNetworkProxy::NoProxy);
    m_socket->connectToHost(m_ip, m_port);

    // 连接超时检测
    QTcpSocket *socket = m_socket;
    QTimer::singleShot(5000, this, [this, socket]() {
        if (!m_stopping && m_socket == socket &&
            socket->state() != QAbstractSocket::ConnectedState) {
            socket->abort();
            LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 连接超时").arg(m_stationId));
            startReconnectTimer();
        }
    });
}

void TcpConnection::onCheckTimeout()
{
    if (!m_isConnected) return;
    qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_lastDataTime > 0 && (now - m_lastDataTime) > 10) {
        LOGMgr->addLog("TcpConnection", QStringLiteral("站%1 超过10秒无数据，断开旧连接后重连").arg(m_stationId));
        m_lastDataTime = 0;
        m_socket->abort();
        startReconnectTimer();
    }
}
