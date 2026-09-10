// mseedfilehandler.h
#ifndef MSEEDFILEHANDLER_H
#define MSEEDFILEHANDLER_H

#include <QObject>
#include <QFileSystemWatcher>
#include <QSet>

class MseedFileHandler : public QObject
{
	Q_OBJECT
public:
	explicit MseedFileHandler(const QString &tempPath,
		const QString &pickerPath,
		const QString &invalidPath,
		QObject *parent = nullptr);
	~MseedFileHandler();

public slots:
	void start();               // 开始监控
	void stop();                // 停止监控

private slots:
	void onDirectoryChanged(const QString &path);   // 目录变化时触发

private:
	void processNewFiles();                         // 处理新增的 .mseed 文件
	bool isCompliantEvent(const QString &filePath); // 调用现有接口的包装函数

	void readOffch();

	QFileSystemWatcher *m_watcher;
	QString m_tempPath;
	QString m_pickerPath;
	QString m_invalidPath;
	QSet<QString> m_processedFiles;   // 记录已处理过的文件（避免重复）
	std::vector<int> m_vecOffch;
	bool m_running;
};

#endif // MSEEDFILEHANDLER_H