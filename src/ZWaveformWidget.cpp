#include "ZWaveformWidget.h"

#include <QPainter>
#include <QPainterPath>
#include <cmath>

ZWaveformWidget::ZWaveformWidget(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(160);
    m_staleTimer.setSingleShot(true);
    m_staleTimer.setInterval(3000);
    connect(&m_staleTimer, &QTimer::timeout, this, &ZWaveformWidget::clear);
}

void ZWaveformWidget::showFrame(int channel, const QVector<float>& samples)
{
    m_channel = channel;
    m_samples = samples; // 整帧替换；无历史队列。
    m_staleTimer.start();
    update();
}

void ZWaveformWidget::clear()
{
    m_staleTimer.stop();
    m_samples.clear();
    m_channel = -1;
    update();
}

void ZWaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().base());
    painter.setPen(palette().text().color());
    if (m_channel < 0 || m_samples.isEmpty()) {
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Z轴波形：等待有效数据"));
        return;
    }

    float minimum = 0, maximum = 0;
    bool found = false;
    for (float value : m_samples) {
        if (!std::isfinite(value)) continue;
        if (!found) { minimum = maximum = value; found = true; }
        minimum = qMin(minimum, value);
        maximum = qMax(maximum, value);
    }
    if (!found) return;
    painter.drawText(QRect(8, 0, width() - 16, 26), Qt::AlignVCenter,
        QStringLiteral("站台1 · 通道%1 · Z轴    当前帧 %2 点    最小 %3 / 最大 %4（原始值）")
            .arg(m_channel + 1).arg(m_samples.size()).arg(minimum).arg(maximum));
    const QRectF plot(70, 32, qMax(1, width() - 84), qMax(1, height() - 54));
    painter.setPen(palette().mid().color());
    painter.drawRect(plot);
    painter.drawLine(QPointF(plot.left(), plot.center().y()), QPointF(plot.right(), plot.center().y()));
    painter.setPen(palette().text().color());
    painter.drawText(QRectF(0, plot.top(), 64, 20), Qt::AlignRight, QString::number(maximum));
    painter.drawText(QRectF(0, plot.bottom() - 20, 64, 20), Qt::AlignRight, QString::number(minimum));
    painter.drawText(QRectF(plot.left(), plot.bottom() + 2, plot.width(), 20),
        Qt::AlignCenter, QStringLiteral("当前数据帧（按采样顺序）"));

    const double center = (double(minimum) + maximum) / 2;
    const double range = qMax(2.0, double(maximum) - minimum) * 1.1;
    QPainterPath path;
    bool connected = false;
    for (int i = 0; i < m_samples.size(); ++i) {
        if (!std::isfinite(m_samples[i])) { connected = false; continue; }
        const QPointF point(plot.left() + plot.width() * i / qMax(1, m_samples.size() - 1),
            plot.center().y() - (m_samples[i] - center) * plot.height() / range);
        if (connected) path.lineTo(point);
        else path.moveTo(point);
        connected = true;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0, 145, 95), 1.5));
    painter.drawPath(path);
}
