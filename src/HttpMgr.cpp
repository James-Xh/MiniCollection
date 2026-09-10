#include "HttpMgr.h"
#include <QCoreApplication>
#include <QSettings>
#include <QFileInfo>
#include <QUrlQuery>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <iostream>

QScopedPointer<HttpMgr> HttpMgr::__self;

HttpMgr::HttpMgr(QObject* parent /*= nullptr*/):QObject(parent)
{
	
	if (NULL == m_pAccessManager)
	{
		m_pAccessManager = new QNetworkAccessManager(this);
		connect(m_pAccessManager, SIGNAL(finished(QNetworkReply*)), this, SLOT(requestFinished(QNetworkReply*)));
	}
	//m_strUrl = "http://www.jiatq.com:10008";
	m_strUrl = "http://127.0.0.1:9001";
}

HttpMgr::~HttpMgr()
{
	if (m_pPostReply)
		m_pPostReply->abort();
}

HttpMgr* HttpMgr::Instance()
{
	if (__self.isNull()) {
		static QMutex mutex;
		QMutexLocker locker(&mutex);
		if (__self.isNull()) {
			__self.reset(new HttpMgr);
		}
	}
	return __self.data();
}

void HttpMgr::setUrl(QString strUrl)
{
	m_strUrl = strUrl;
}


void HttpMgr::calc_json(const QJsonArray & jArr)
{
	QJsonDocument doc(jArr);
	QByteArray body = doc.toJson();

// 	QFile fie("./josndata.txt");
// 	if (fie.open(QIODevice::WriteOnly | QIODevice::Text)) {
// 		fie.write(body);
// 	}
// 	fie.close();

	// 服务端速度跟不上数据流时，不允许 Reply 无限堆积。
	// 已有请求在执行时，仅替换一份“最新待发数据”。
	if (m_pPostReply) {
		m_pendingBody = body;
		m_hasPendingBody = true;
		return;
	}

	postBody(body);
}

void HttpMgr::postBody(const QByteArray& body)
{
	QUrl url(m_strUrl + "/coalmine/wave/recive");
	QNetworkRequest request(url);
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

	QNetworkReply* reply = m_pAccessManager->post(request, body);
	m_pPostReply = reply;

	// Qt 5 没有统一的 transferTimeout API，用与 Reply 同生命周期的计时器中止超时请求。
	QTimer::singleShot(10000, reply, [reply]() {
		if (reply->isRunning())
			reply->abort();
	});
}

void HttpMgr::requestFinished(QNetworkReply* reply)
{
	if (reply == NULL)
		return;

	// 获取http状态码
	QVariant statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
	QVariant reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();

	//QString strtxt = "";

	if (reason.isValid())
	{
		//strtxt = reason.toString();
	}
	QString error;
	QNetworkReply::NetworkError err = reply->error();

	bool bSucc = false;
	if (err == QNetworkReply::NoError)
	{
		QString str = reply->readAll();

		//strtxt += "\n";
		//strtxt += str;
		qDebug() << "Response:" << str;// reply->readAll();
		bSucc = true;
	}
	else
	{
// 		strtxt += "\nerr\n";
// 		strtxt += reply->errorString();
	}

// 	QFile fie("./josndata_ret.txt");
// 	if (fie.open(QIODevice::WriteOnly | QIODevice::Text)) {
// 		fie.write(strtxt.toUtf8());
// 	}
// 	fie.close();

	if (m_pPostReply == reply)
		m_pPostReply = nullptr;

	reply->deleteLater();

	if (m_hasPendingBody) {
		QByteArray nextBody = m_pendingBody;
		m_pendingBody.clear();
		m_hasPendingBody = false;
		postBody(nextBody);
	}
}

