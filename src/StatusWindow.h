#ifndef STATUSWINDOW_H
#define STATUSWINDOW_H

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QLabel>
#include <QPushButton>
#include "Commdef.h"

// 精简版原生界面：
//  - 表格显示每个已配置站的连接状态、在线通道数、GPS、数据同步状态
//  - 最简参数设置（写入 Config.ini 并即时生效到算法）
//  - 底部滚动日志
class StatusWindow : public QWidget
{
    Q_OBJECT
public:
    explicit StatusWindow(QWidget* parent = nullptr);
    void setPickder(QObject* pickder) { m_pickder = pickder; }

private slots:
    void onStationConnectStatus(int stationId, bool connected);
    void onDevStatus(int stationId, int channel, int state, int typeBit);
    void onGpsStatus(int stationId, int gps);
    void onPickerSyncStatus(int state, qint64 maxDiffUs, int stationId, const QString& message);
    void onPickerNewFile(const QString& file);
    void onAddLog(const QString& type, const QString& desc);
    void onApplyParams();

private:
    void initParamForm();
    QWidget* initParamFormWidget();
    int stationRow(int stationId);

private:
    QTableWidget* m_table = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QLabel* m_syncLabel = nullptr;
    // 每站在线通道计数（替代函数内 static，断线时可正确重置）
    int m_online[MAX_STATION];
    QObject* m_pickder = nullptr;   // MDataPickder（跨线程）

    // 参数输入框（下标对应 MDataPickder::changeCallcValue 的 ntag）
    QList<QLineEdit*> m_paramEdits;
    QStringList m_paramNames;
    QList<int> m_paramTags;
};

#endif // STATUSWINDOW_H
