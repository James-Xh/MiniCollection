#ifndef NETWORKREPLYTIMEOUT_H
#define NETWORKREPLYTIMEOUT_H

#include <QNetworkReply>
#include <QNetworkRequest>
#include <QString>
#include <QTimer>

// 设备端与控制台共用的 HTTP 超时保护与失败描述。
//
// Qt 5.12 没有 transferTimeout API，网络异常（路由消失、对端不回应、NAT 半开）
// 时 QNetworkReply 可能长时间不 finished，调用方的 busy/active 标志会永久卡住。
// 这里在创建 reply 后立即 arm()，超时后主动 abort；abort 同样会触发 finished，
// 原有失败分支照常执行，只是日志里能区分“超时”和“对端拒绝/不可达”。
namespace NetTimeout {

// 给回复安装超时保护。超时后打上标记再 abort，便于 describeFailure() 识别。
inline void arm(QNetworkReply* reply, int timeoutMs)
{
    if (!reply || timeoutMs <= 0)
        return;
    reply->setProperty("mcTimeoutMs", timeoutMs);
    QTimer::singleShot(timeoutMs, reply, [reply]() {
        if (reply->isRunning()) {
            reply->setProperty("mcTimedOut", true);
            reply->abort();
        }
    });
}

inline bool timedOut(const QNetworkReply* reply)
{
    return reply && reply->property("mcTimedOut").toBool();
}

inline int armedTimeoutMs(const QNetworkReply* reply)
{
    return reply ? reply->property("mcTimeoutMs").toInt() : 0;
}

// 统一的失败描述：完整 URL + HTTP 状态 + Qt 错误码/文本（+ 超时说明）。
// HTTP=0 表示没有收到任何响应，问题在 DNS/路由/端口，而不是接口参数。
inline QString describeFailure(const QNetworkReply* reply)
{
    if (!reply)
        return QStringLiteral("reply 为空");
    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString text = QStringLiteral("url=%1 HTTP=%2 err=%3(%4)")
                       .arg(reply->url().toString())
                       .arg(http)
                       .arg(int(reply->error()))
                       .arg(reply->errorString());
    if (timedOut(reply)) {
        const int seconds = qMax(1, armedTimeoutMs(reply) / 1000);
        text += QStringLiteral(" [超时 %1 秒无响应]").arg(seconds);
    }
    return text;
}

} // namespace NetTimeout

#endif // NETWORKREPLYTIMEOUT_H
