#pragma once

#include <QWidget>
#include <QVector>
#include <QTimer>

// 只保留当前帧以供重绘，不累积历史、不落盘。
class ZWaveformWidget : public QWidget
{
public:
    explicit ZWaveformWidget(QWidget* parent = nullptr);
    void showFrame(int channel, const QVector<float>& samples);
    void clear();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QVector<float> m_samples;
    int m_channel = -1;
    QTimer m_staleTimer;
};
