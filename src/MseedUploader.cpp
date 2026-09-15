#include "MseedUploader.h"
#include "AppConfig.h"
#include "LogManage.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrlQuery>
#include <QMutex>

namespace {
const qint64 kChunkSize = 256 * 1024;
const char* kUploadPath = "/coalmine/mseed/upload";
const char* kDoneFile = "done.txt";
const char* kProgressFile = "progress.ini";
}

QScopedPointer<MseedUploader> MseedUploader::__self;

MseedUploader::MseedUploader(QObject* parent)
	: QObject(parent)
{
	m_nam = new QNetworkAccessManager(this);
}

MseedUploader* MseedUploader::Instance()
{
	if (__self.isNull()) {
		static QMutex mutex;
		QMutexLocker locker(&mutex);
		if (__self.isNull()) {
			__self.reset(new MseedUploader);
		}
	}
	return __self.data();
}

void MseedUploader::start(const QString& pickerPath)
{
	if (m_started)
		return;
	m_started = true;
	m_pickerPath = pickerPath;
	// 状态目录与 picker 平级：.../data/picker -> .../data/upload
	m_stateDir = QDir(QDir(m_pickerPath).absoluteFilePath("../upload")).absolutePath();
	QDir().mkpath(m_stateDir);
	loadDoneList();
	applyFromConfig();
}

void MseedUploader::applySettings(bool enabled, const QString& serverUrl)
{
	m_enabled = enabled;
	m_serverUrl = serverUrl.trimmed();
	while (m_serverUrl.endsWith('/'))
		m_serverUrl.chop(1);
	AppConfig::Instance()->setConfig("Upload", "enabled", m_enabled ? 1 : 0);
	AppConfig::Instance()->setConfig("Upload", "serverUrl", m_serverUrl);
	AppConfig::Instance()->syncConfig();
	if (m_enabled && !m_serverUrl.isEmpty()) {
		logThrottled("MseedUploader",
			QStringLiteral("上传已启用：%1").arg(m_serverUrl));
	}
	else {
		logThrottled("MseedUploader", QStringLiteral("上传已停用"));
	}
}

void MseedUploader::applyFromConfig()
{
	m_enabled = AppConfig::Instance()->getConfig("Upload", "enabled").toInt() != 0;
	m_serverUrl = AppConfig::Instance()->getConfig("Upload", "serverUrl").toString().trimmed();
	while (m_serverUrl.endsWith('/'))
		m_serverUrl.chop(1);
	int pollSec = AppConfig::Instance()->getConfig("Upload", "pollSec").toInt();
	if (pollSec <= 0)
		pollSec = 5;
	if (!m_scanTimer) {
		m_scanTimer = new QTimer(this);
		connect(m_scanTimer, &QTimer::timeout, this, &MseedUploader::onScan);
	}
	m_scanTimer->start(pollSec * 1000);
}

void MseedUploader::loadDoneList()
{
	m_doneFiles.clear();
	QFile f(m_stateDir + "/" + kDoneFile);
	if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
		const QStringList lines = QString::fromUtf8(f.readAll())
			.split('\n', QString::SkipEmptyParts);
		for (QString line : lines) {
			line = line.trimmed();
			if (!line.isEmpty())
				m_doneFiles.insert(line);
		}
	}
}

void MseedUploader::saveDoneList()
{
	QFile f(m_stateDir + "/" + kDoneFile);
	if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		for (const QString& name : m_doneFiles)
			f.write((name + "\n").toUtf8());
	}
}

qint64 MseedUploader::localProgress(const QString& fileName) const
{
	QSettings progress(m_stateDir + "/" + kProgressFile, QSettings::IniFormat);
	return progress.value(fileName).toLongLong();
}

void MseedUploader::saveProgress(const QString& fileName, qint64 offset)
{
	QSettings progress(m_stateDir + "/" + kProgressFile, QSettings::IniFormat);
	progress.setValue(fileName, offset);
	progress.sync();
}

void MseedUploader::clearProgress(const QString& fileName)
{
	QSettings progress(m_stateDir + "/" + kProgressFile, QSettings::IniFormat);
	progress.remove(fileName);
	progress.sync();
}

void MseedUploader::markDone(const QString& fileName)
{
	m_doneFiles.insert(fileName);
	saveDoneList();
	clearProgress(fileName);
}

QString MseedUploader::uploadUrl(const QString& filePath) const
{
	QUrl url(m_serverUrl + kUploadPath);
	QUrlQuery query;
	query.addQueryItem("name",
		QString::fromUtf8(QUrl::toPercentEncoding(QFileInfo(filePath).fileName())));
	url.setQuery(query);
	return url.toString();
}

void MseedUploader::onScan()
{
	if (!m_enabled || m_busy || m_serverUrl.isEmpty())
		return;
	beginNextFile();
}

bool MseedUploader::beginNextFile()
{
	m_busy = true;
	QDir dir(m_pickerPath);
	const QStringList files = dir.entryList(QStringList("*.mseed"),
		QDir::Files, QDir::Name);
	QString candidate;
	for (const QString& name : files) {
		if (!m_doneFiles.contains(name)) {
			candidate = name;
			break;
		}
	}
	if (candidate.isEmpty()) {
		m_busy = false;
		return false;
	}
	m_currentFile = dir.absoluteFilePath(candidate);
	// 先查询服务端已接收字节数（断点续传），失败则退回本地进度记录
	startHead(m_currentFile);
	return true;
}

void MseedUploader::startHead(const QString& filePath)
{
	QNetworkRequest request(uploadUrl(filePath));
	QNetworkReply* reply = m_nam->head(request);
	m_reply = reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath]() {
		m_reply.clear();
		reply->deleteLater();
		if (!m_busy)   // 上传已被停用
			return;

		const QFileInfo fi(filePath);
		const qint64 total = fi.size();
		qint64 offset = -1;
		if (reply->error() == QNetworkReply::NoError) {
			const QByteArray bytes = reply->rawHeader("X-Uploaded-Bytes");
			if (!bytes.isEmpty()) {
				bool ok = false;
				const qint64 srv = bytes.toLongLong(&ok);
				if (ok && srv >= 0 && srv <= total)
					offset = srv;   // 服务端权威
			}
		}
		if (offset < 0)
			offset = localProgress(fi.fileName());   // 本地进度续传
		if (offset > total)
			offset = total;
		if (offset >= total) {
			// 服务端已有完整文件（或本地记录已传完），直接标记完成
			markDone(fi.fileName());
			LOGMgr->addLog("MseedUploader",
				QStringLiteral("文件已存在于服务端，跳过上传：%1").arg(fi.fileName()));
			m_busy = false;
			return;
		}
		startChunk(filePath, offset);
	});
}

void MseedUploader::startChunk(const QString& filePath, qint64 offset)
{
	QFile file(filePath);
	if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
		logThrottled("MseedUploader",
			QStringLiteral("打开/定位文件失败：%1").arg(filePath));
		m_busy = false;
		return;
	}
	const QByteArray chunk = file.read(kChunkSize);
	const qint64 total = file.size();
	if (chunk.isEmpty()) {
		// 文件内容已读完但未标记完成（如进度记录异常），直接完成
		markDone(QFileInfo(filePath).fileName());
		LOGMgr->addLog("MseedUploader",
			QStringLiteral("无剩余数据，标记完成：%1").arg(QFileInfo(filePath).fileName()));
		m_busy = false;
		return;
	}
	const qint64 end = offset + chunk.size() - 1;

	QNetworkRequest request(uploadUrl(filePath));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
	request.setRawHeader("Content-Range",
		QStringLiteral("bytes %1-%2/%3").arg(offset).arg(end).arg(total).toUtf8());
	QNetworkReply* reply = m_nam->put(request, chunk);
	m_reply = reply;
	connect(reply, &QNetworkReply::finished, this, [this, reply, filePath, offset, chunk, total]() {
		m_reply.clear();
		reply->deleteLater();
		const QFileInfo fi(filePath);
		if (!m_busy)
			return;

		const bool ok = (reply->error() == QNetworkReply::NoError);
		const int status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (ok || (status >= 200 && status < 300)) {
			const qint64 newOffset = offset + chunk.size();
			if (newOffset >= total) {
				markDone(fi.fileName());
				LOGMgr->addLog("MseedUploader",
					QStringLiteral("上传完成：%1 (%2字节)").arg(fi.fileName()).arg(total));
				m_busy = false;
				return;
			}
			saveProgress(fi.fileName(), newOffset);
			// 立即继续下一分片（同一事件循环内串行推进）
			QTimer::singleShot(0, this, [this, filePath, newOffset]() {
				if (m_busy)
					startChunk(filePath, newOffset);
			});
		}
		else {
			logThrottled("MseedUploader",
				QStringLiteral("上传失败(将重试)：%1 offset=%2 err=%3 status=%4")
					.arg(fi.fileName()).arg(offset)
					.arg(reply->errorString())
					.arg(status));
			m_busy = false;   // 等下一次扫描重试，进度已按分片粒度保存
		}
	});
}

void MseedUploader::finishCurrent()
{
	m_busy = false;
}

void MseedUploader::logThrottled(const QString& type, const QString& msg)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (now - m_lastErrorLogMs < 60000)
		return;
	m_lastErrorLogMs = now;
	LOGMgr->addLog(type, msg);
}

