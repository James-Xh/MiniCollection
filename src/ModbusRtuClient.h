#ifndef MODBUSRTUCLIENT_H
#define MODBUSRTUCLIENT_H

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <QSerialPort>
#else
#include <termios.h>
#endif

// 最小 Modbus RTU 主站实现，只支持功能码 03（读保持寄存器）。
// 串口读写是阻塞的，必须在工作线程中使用，不能放在 UI 线程。
//
// Windows 使用 Qt5 SerialPort；Linux/EG628 使用 POSIX termios。
// 这样目标 sysroot 不需要安装 Qt5SerialPort 模块。
class ModbusRtuClient
{
public:
    ModbusRtuClient();
    ~ModbusRtuClient();

    bool open(const QString& port, int baud, char parity,
              int dataBits, int stopBits, QString* error);
    void close();
    bool isOpen() const;

    // slave: 1..247；start: PDU 起始地址；count: 寄存器数量。
    bool readHoldingRegisters(int slave, int start, int count,
                              QVector<quint16>* out, QString* error);

    static quint16 crc16(const QByteArray& data);

private:
#ifdef Q_OS_WIN
    QSerialPort m_port;
#else
    int m_fd = -1;
    static speed_t baudToSpeed(int baud);
    bool writeAll(const QByteArray& data, QString* error);
    int readSome(char* buffer, int size, int timeoutMs);
#endif
};

#endif // MODBUSRTUCLIENT_H
