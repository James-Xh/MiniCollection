#pragma once

#include <QObject>
#include <QUdpSocket>
#include <QMutex>

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QHttpMultiPart>
#include <QHttpPart>
#include <QUrl>
#include <QFile>
#include <QNetworkReply>

class HttpMgr :public QObject
{
	Q_OBJECT
public:
	explicit HttpMgr(QObject* parent = nullptr);
	~HttpMgr();
	static HttpMgr* Instance();

	void setUrl(QString strUrl);
private:
	static QScopedPointer<HttpMgr> __self;

	QString             m_strUrl = "";

	// 1. 创建网络管理器
	QNetworkAccessManager* m_pAccessManager = NULL;
	QNetworkReply* m_pPostReply = NULL; // 最多一个正在进行的请求
	QByteArray m_pendingBody;           // 只保留最新一份待发数据
	bool m_hasPendingBody = false;

	void postBody(const QByteArray& body);

public:
	void calc_json(const QJsonArray & jArr);

public:

signals:

private slots:
	//请求结果
	void requestFinished(QNetworkReply* reply);
};
