#include "ModbusRtuClient.h"

#include <QElapsedTimer>
#include <QThread>

namespace {
const int kResponseTimeoutMs = 1200;
// 协议要求帧间至少 10ms 静默；9600 波特率下 3.5 字符约 4ms。
const int kInterFrameDelayMs = 20;
}

ModbusRtuClient::ModbusRtuClient() = default;

ModbusRtuClient::~ModbusRtuClient()
{
    close();
}

static QSerialPort::Parity toParity(char parity)
{
    switch (parity) {
    case 'E':
    case 'e':
        return QSerialPort::EvenParity;
    case 'O':
    case 'o':
        return QSerialPort::OddParity;
    default:
        return QSerialPort::NoParity;
    }
}

bool ModbusRtuClient::open(const QString& port, int baud, char parity,
                           int dataBits, int stopBits, QString* error)
{
    close();

    m_port.setPortName(port);
    m_port.setBaudRate(baud);
    m_port.setParity(toParity(parity));
    m_port.setDataBits(static_cast<QSerialPort::DataBits>(dataBits));
    m_port.setStopBits(stopBits == 2 ? QSerialPort::TwoStop
                                     : QSerialPort::OneStop);
    m_port.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port.open(QIODevice::ReadWrite)) {
        if (error)
            *error = QStringLiteral("打开 %1 失败：%2").arg(port, m_port.errorString());
        return false;
    }

    m_port.clear();
    return true;
}

void ModbusRtuClient::close()
{
    if (m_port.isOpen())
        m_port.close();
}

bool ModbusRtuClient::isOpen() const
{
    return m_port.isOpen();
}

quint16 ModbusRtuClient::crc16(const QByteArray& data)
{
    quint16 crc = 0xFFFF;
    for (char ch : data) {
        crc ^= quint8(ch);
        for (int i = 0; i < 8; ++i)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}

bool ModbusRtuClient::readHoldingRegisters(int slave, int start, int count,
                                           QVector<quint16>* out, QString* error)
{
    if (!m_port.isOpen()) {
        if (error)
            *error = QStringLiteral("串口未打开");
        return false;
    }
    if (!out || slave < 1 || slave > 247 || start < 0 || start > 0xFFFF
        || count < 1 || count > 125) {
        if (error)
            *error = QStringLiteral("Modbus 参数无效");
        return false;
    }

    // 协议要求帧间静默 > 10ms，避免上一帧刚结束就发下一帧。
    QThread::msleep(kInterFrameDelayMs);

    QByteArray request;
    request.append(char(slave));
    request.append(char(0x03));
    request.append(char((start >> 8) & 0xFF));
    request.append(char(start & 0xFF));
    request.append(char((count >> 8) & 0xFF));
    request.append(char(count & 0xFF));

    const quint16 requestCrc = crc16(request);
    request.append(char(requestCrc & 0xFF));
    request.append(char(requestCrc >> 8));

    m_port.clear();
    if (m_port.write(request) != request.size()) {
        if (error)
            *error = QStringLiteral("串口写入失败：%1").arg(m_port.errorString());
        return false;
    }
    if (!m_port.waitForBytesWritten(500)) {
        if (error)
            *error = QStringLiteral("串口写入超时");
        return false;
    }

    QByteArray response;
    QElapsedTimer timer;
    timer.start();

    // 先读 3 字节头：从站地址、功能码、数据长度。
    while (response.size() < 3 && timer.elapsed() < kResponseTimeoutMs) {
        if (m_port.waitForReadyRead(100))
            response += m_port.readAll();
    }

    if (response.size() < 3) {
        if (error)
            *error = QStringLiteral("Modbus 响应超时（从站 %1）").arg(slave);
        return false;
    }

    if (quint8(response.at(0)) != slave) {
        if (error)
            *error = QStringLiteral("从站地址不匹配：期望 %1，收到 %2")
                         .arg(slave).arg(quint8(response.at(0)));
        return false;
    }

    const quint8 function = quint8(response.at(1));
    if (function == 0x83) {
        if (response.size() < 5) {
            while (response.size() < 5 && timer.elapsed() < kResponseTimeoutMs) {
                if (m_port.waitForReadyRead(100))
                    response += m_port.readAll();
            }
        }
        const int exceptionCode = response.size() >= 3 ? quint8(response.at(2)) : -1;
        if (error)
            *error = QStringLiteral("Modbus 异常响应，异常码 %1").arg(exceptionCode);
        return false;
    }
    if (function != 0x03) {
        if (error)
            *error = QStringLiteral("Modbus 功能码不匹配：0x%1")
                         .arg(function, 2, 16, QLatin1Char('0'));
        return false;
    }

    const int byteCount = quint8(response.at(2));
    const int expectedSize = 3 + byteCount + 2;
    while (response.size() < expectedSize && timer.elapsed() < kResponseTimeoutMs) {
        if (m_port.waitForReadyRead(100))
            response += m_port.readAll();
    }

    if (response.size() != expectedSize) {
        if (error)
            *error = QStringLiteral("Modbus 响应长度错误：期望 %1，收到 %2")
                         .arg(expectedSize).arg(response.size());
        return false;
    }
    if (byteCount != count * 2) {
        if (error)
            *error = QStringLiteral("Modbus 数据长度错误：期望 %1 字节，收到 %2")
                         .arg(count * 2).arg(byteCount);
        return false;
    }

    const quint16 receivedCrc = quint8(response.at(expectedSize - 2))
                              | (quint8(response.at(expectedSize - 1)) << 8);
    if (receivedCrc != crc16(response.left(expectedSize - 2))) {
        if (error)
            *error = QStringLiteral("Modbus CRC 校验失败");
        return false;
    }

    out->clear();
    out->reserve(count);
    for (int i = 0; i < count; ++i) {
        const int index = 3 + i * 2;
        out->append((quint8(response.at(index)) << 8)
                    | quint8(response.at(index + 1)));
    }
    return true;
}
