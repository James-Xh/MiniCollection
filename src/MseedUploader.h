#ifndef MSEEDUPLOADER_H
#define MSEEDUPLOADER_H

#include <QObject>
#include <QTimer>
#include <QSet>
#include <QPointer>
#include <QScopedPointer>

class QNetworkAccessManager;
class QNetworkReply;

// 将 data/picker 目录下生成的 mseed 事件文件通过 HTTP 上传到远端服务器。
// 断点续传协议（服务端配合可选，缺省退化为本地进度续传）：
//   1) HEAD {serverUrl}/coalmine/mseed/upload?name=<file>
//      服务端若支持，返回响应头 X-Uploaded-Bytes: <已接收字节数>；
//      不支持(404/无该头)则退回本地进度记录 data/upload/progress.ini。
//   2) PUT  {同上URL}，请求头 "Content-Range: bytes <start>-<end>/<total>"，
//      每次发送一个分片(256KB)；每个分片均以 2xx 应答，最后一片完成即上传结束。
// 完成记录：data/upload/done.txt (每行一个文件名，避免重复上传)。
// 配置项：Config.ini [Upload] enabled(0/1)、serverUrl、token、pollSec(默认5)。
class MseedUploader : public QObject
{
	Q_OBJECT
public:
	static MseedUploader* Instance();

	void start(const QString& pickerPath);   // 幂等：只启动一次
	void applySettings(bool enabled, const QString& serverUrl); // UI/配置入口，立即生效并落盘
	bool enabled() const { return m_enabled; }
	QString serverUrl() const { return m_serverUrl; }

private slots:
	void onScan();

private:
	explicit MseedUploader(QObject* parent = nullptr);

	void applyFromConfig();
	void loadDoneList();
	void saveDoneList();
	bool beginNextFile();
	void startHead(const QString& filePath);
	void startChunk(const QString& filePath, qint64 offset);
	QString uploadUrl(const QString& filePath) const;
	qint64 localProgress(const QString& fileName) const;
	void saveProgress(const QString& fileName, qint64 offset);
	void clearProgress(const QString& fileName);
	void markDone(const QString& fileName);
	void finishCurrent();
	void logThrottled(const QString& type, const QString& msg);

	QNetworkAccessManager* m_nam = nullptr;
	QTimer* m_scanTimer = nullptr;
	QString m_pickerPath;
	QString m_stateDir;
	QString m_serverUrl;
	QByteArray m_uploadToken;
	bool m_enabled = false;
	bool m_started = false;
	bool m_busy = false;

	QSet<QString> m_doneFiles;
	QString m_currentFile;       // 当前上传中的文件绝对路径
	QPointer<QNetworkReply> m_reply;
	qint64 m_lastErrorLogMs = 0;

	static QScopedPointer<MseedUploader> __self;
};

#endif // MSEEDUPLOADER_H
