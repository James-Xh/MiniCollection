#include "ModbusRtuClient.h"

#include <QElapsedTimer>

#ifdef Q_OS_WIN
#include <QThread>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#endif

namespace {
const int kResponseTimeoutMs = 1200;
// 协议要求帧间至少 10ms 静默；9600 波特率下 3.5 字符约 4ms。
const int kInterFrameDelayMs = 20;

bool parseModbusResponse(const QByteArray& response, int slave, int count,
                         QVector<quint16>* out, QString* error)
{
    if (response.size() < 5) {
        if (error)
            *error = QStringLiteral("Modbus 响应长度不足");
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
        const int exceptionCode = quint8(response.at(2));
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
    if (receivedCrc != ModbusRtuClient::crc16(response.left(expectedSize - 2))) {
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
} // namespace

ModbusRtuClient::ModbusRtuClient() = default;

ModbusRtuClient::~ModbusRtuClient()
{
    close();
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

#ifdef Q_OS_WIN

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

    while (response.size() < 3 && timer.elapsed() < kResponseTimeoutMs) {
        if (m_port.waitForReadyRead(100))
            response += m_port.readAll();
    }
    if (response.size() < 3) {
        if (error)
            *error = QStringLiteral("Modbus 响应超时（从站 %1）").arg(slave);
        return false;
    }

    const int expectedSize = quint8(response.at(1)) == 0x03
                                 ? 3 + quint8(response.at(2)) + 2
                                 : 5;
    while (response.size() < expectedSize && timer.elapsed() < kResponseTimeoutMs) {
        if (m_port.waitForReadyRead(100))
            response += m_port.readAll();
    }

    return parseModbusResponse(response, slave, count, out, error);
}

#else // Q_OS_WIN

speed_t ModbusRtuClient::baudToSpeed(int baud)
{
    switch (baud) {
    case 1200:   return B1200;
    case 2400:   return B2400;
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

bool ModbusRtuClient::open(const QString& port, int baud, char parity,
                           int dataBits, int stopBits, QString* error)
{
    close();

    m_fd = ::open(port.toLocal8Bit().constData(),
                  O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        if (error)
            *error = QStringLiteral("打开 %1 失败：%2")
                         .arg(port, QString::fromLocal8Bit(std::strerror(errno)));
        return false;
    }
    // open 时用非阻塞避免等待载波；配置完成后恢复阻塞写。
    fcntl(m_fd, F_SETFL, 0);

    struct termios options;
    if (tcgetattr(m_fd, &options) != 0) {
        if (error)
            *error = QStringLiteral("读取串口参数失败：%1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        close();
        return false;
    }

    cfmakeraw(&options);
    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~CRTSCTS;

    if (stopBits == 2)
        options.c_cflag |= CSTOPB;
    else
        options.c_cflag &= ~CSTOPB;

    options.c_cflag &= ~CSIZE;
    switch (dataBits) {
    case 5: options.c_cflag |= CS5; break;
    case 6: options.c_cflag |= CS6; break;
    case 7: options.c_cflag |= CS7; break;
    default: options.c_cflag |= CS8; break;
    }

    if (parity == 'E' || parity == 'e') {
        options.c_cflag |= PARENB;
        options.c_cflag &= ~PARODD;
    }
    else if (parity == 'O' || parity == 'o') {
        options.c_cflag |= PARENB;
        options.c_cflag |= PARODD;
    }
    else {
        options.c_cflag &= ~PARENB;
    }

    const speed_t speed = baudToSpeed(baud);
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);

    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 10;

    if (tcsetattr(m_fd, TCSANOW, &options) != 0) {
        if (error)
            *error = QStringLiteral("设置串口参数失败：%1")
                         .arg(QString::fromLocal8Bit(std::strerror(errno)));
        close();
        return false;
    }

    tcflush(m_fd, TCIOFLUSH);
    return true;
}

void ModbusRtuClient::close()
{
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool ModbusRtuClient::isOpen() const
{
    return m_fd >= 0;
}

bool ModbusRtuClient::writeAll(const QByteArray& data, QString* error)
{
    const char* cursor = data.constData();
    int remaining = data.size();
    while (remaining > 0) {
        const ssize_t written = ::write(m_fd, cursor, size_t(remaining));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            if (error)
                *error = QStringLiteral("串口写入失败：%1")
                             .arg(QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        if (written == 0) {
            if (error)
                *error = QStringLiteral("串口写入返回 0 字节");
            return false;
        }
        cursor += written;
        remaining -= int(written);
    }
    return true;
}

int ModbusRtuClient::readSome(char* buffer, int size, int timeoutMs)
{
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_fd, &readSet);

    struct timeval timeout;
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int ready = ::select(m_fd + 1, &readSet, nullptr, nullptr, &timeout);
    if (ready <= 0)
        return ready;
    const ssize_t received = ::read(m_fd, buffer, size_t(size));
    if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return int(received);
}

bool ModbusRtuClient::readHoldingRegisters(int slave, int start, int count,
                                           QVector<quint16>* out, QString* error)
{
    if (m_fd < 0) {
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

    usleep(useconds_t(kInterFrameDelayMs) * 1000);
    tcflush(m_fd, TCIFLUSH);

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

    if (!writeAll(request, error))
        return false;
    tcdrain(m_fd);

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    char buffer[256];

    while (response.size() < 3 && timer.elapsed() < kResponseTimeoutMs) {
        const int received = readSome(buffer, sizeof(buffer), 100);
        if (received > 0)
            response.append(buffer, received);
        else if (received < 0 && errno != EINTR)
            break;
    }
    if (response.size() < 3) {
        if (error)
            *error = QStringLiteral("Modbus 响应超时（从站 %1）").arg(slave);
        return false;
    }

    const int expectedSize = quint8(response.at(1)) == 0x03
                                 ? 3 + quint8(response.at(2)) + 2
                                 : 5;
    while (response.size() < expectedSize && timer.elapsed() < kResponseTimeoutMs) {
        const int received = readSome(buffer, sizeof(buffer), 100);
        if (received > 0)
            response.append(buffer, received);
        else if (received < 0 && errno != EINTR)
            break;
    }

    return parseModbusResponse(response, slave, count, out, error);
}

#endif // Q_OS_WIN
