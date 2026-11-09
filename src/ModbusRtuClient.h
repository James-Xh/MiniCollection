#ifndef MODBUSRTUCLIENT_H
#define MODBUSRTUCLIENT_H

#include <QByteArray>
#include <QSerialPort>
#include <QString>
#include <QVector>

// 最小 Modbus RTU 主站实现，只支持功能码 03（读保持寄存器）。
// 串口读写是阻塞的，必须在工作线程中使用，不能放在 UI 线程。
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

private:
    static quint16 crc16(const QByteArray& data);

    QSerialPort m_port;
};

#endif // MODBUSRTUCLIENT_H
