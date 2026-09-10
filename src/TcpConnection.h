#ifndef TCPCONNECTION_H
#define TCPCONNECTION_H

#include <QObject>
#include <QTcpSocket>
#include <QTimer>

class TcpConnection : public QObject
{
    Q_OBJECT
public:
    explicit TcpConnection(int stationId, const QString &ip, quint16 port, QObject *parent = nullptr);
    ~TcpConnection();

    void start();            // 开始连接
    void stop();             // 断开连接，停止重连
    int stationId() const { return m_stationId; }

signals:
    void frameReady(int stationId, const QByteArray &frame); // 完整的数据帧
    void connectionChanged(int stationId, bool connected);

private slots:
    void onReadyRead();
    void onConnected();
    void onDisconnected();
    void onSocketError(QAbstractSocket::SocketError socketError);
    void onReconnectTimeout();
    void onCheckTimeout();

private:
    void setupSocketSignals();
    void startReconnectTimer();
    void stopReconnectTimer();
    void attemptReconnect();
    void parseData();        // 解析粘包，提取完整帧

    int m_stationId;
    QString m_ip;
    quint16 m_port;
    QTcpSocket *m_socket = nullptr;
    QByteArray m_buffer;              // 粘包缓冲区
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_checkTimer = nullptr;   // 数据超时检测
    int m_reconnectAttempts = 0;
    qint64 m_lastDataTime = 0;
    bool m_isConnected = false;
    bool m_stopping = false;
};

#endif // TCPCONNECTION_H
