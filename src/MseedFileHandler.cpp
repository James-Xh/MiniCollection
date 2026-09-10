// mseedfilehandler.cpp
#include "MseedFileHandler.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include "is_compliant_event.h"
#include "libmseed.h"
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCoreApplication>
#include "CallManage.h"
#include "AppConfig.h"

MseedFileHandler::MseedFileHandler(const QString &tempPath,
	const QString &pickerPath,
	const QString &invalidPath,
	QObject *parent)
	: QObject(parent),
	m_tempPath(tempPath),
	m_pickerPath(pickerPath),
	m_invalidPath(invalidPath),
	m_running(false)
{
	// 确保目标文件夹存在
	QDir().mkpath(m_tempPath);
	QDir().mkpath(m_pickerPath);
	QDir().mkpath(m_invalidPath);

	m_watcher = new QFileSystemWatcher(this);
	connect(m_watcher, &QFileSystemWatcher::directoryChanged,
		this, &MseedFileHandler::onDirectoryChanged);

	this->readOffch();
}

MseedFileHandler::~MseedFileHandler()
{
	stop();
}

void MseedFileHandler::start()
{
	if (m_running) return;
	if (!m_watcher->addPath(m_tempPath)) {
		qWarning() << "Failed to watch directory:" << m_tempPath;
		return;
	}
	m_running = true;
	qDebug() << "Started monitoring" << m_tempPath;

	// 启动后立即扫描一次现有文件
	QMetaObject::invokeMethod(this, "processNewFiles", Qt::QueuedConnection);
}

void MseedFileHandler::stop()
{
	if (!m_running) return;
	m_watcher->removePath(m_tempPath);
	m_running = false;
	qDebug() << "Stopped monitoring";
}

void MseedFileHandler::onDirectoryChanged(const QString &path)
{
	Q_UNUSED(path);
	// 目录变化时，延迟一小段时间再处理，避免文件还未写完
	QTimer::singleShot(200, this, &MseedFileHandler::processNewFiles);
}

void MseedFileHandler::processNewFiles()
{
	if (!m_running) return;

	QDir dir(m_tempPath);
	QStringList filters;
	filters << "*.mseed";
	QFileInfoList files = dir.entryInfoList(filters, QDir::Files | QDir::Readable);

	for (const QFileInfo &info : files) {
		QString absPath = info.absoluteFilePath();
		if (m_processedFiles.contains(absPath))
			continue;   // 已经处理过

		// 简单检查文件是否正在被写入（可选：尝试以只读方式打开）
		QFile file(absPath);
		if (!file.open(QIODevice::ReadOnly)) {
			qWarning() << "File cannot be opened (maybe locked):" << absPath;
			continue;
		}
		file.close();


		MS3Record* msr = NULL;
		uint32_t flags = MSF_UNPACKDATA;
		int verbose = 0;
		int retcode;

		int nChannum = 0;
		int nPointNum = 0;
		QVector<double> vecPoint;
		std::vector<int> vecStatus;
		std::vector<int> vecRpos;
		std::vector<int> vecBpos;
		
		while ((retcode = ms3_readmsr(&msr, absPath.toStdString().c_str(), flags, verbose)) == MS_NOERROR)
		{
			if (nChannum == 0)
			{
				QString strExt = QString(msr->extra);
				QJsonParseError err_rpt;
				qDebug() << "extra " << strExt;
				QJsonDocument  root_Doc = QJsonDocument::fromJson(strExt.toUtf8(), &err_rpt);//字符串格式化为JSON
				if (err_rpt.error == QJsonParseError::NoError)
				{
					QJsonObject root_Obj = root_Doc.object();
					nChannum = root_Obj.value("cnum").toInt();
					nPointNum = root_Obj.value("pnum").toInt();

					QJsonArray sarr = root_Obj.value("status").toArray();
					QJsonArray rpos = root_Obj.value("rpos").toArray();
					QJsonArray bpos = root_Obj.value("bpos").toArray();

					for (int i = 0; i < sarr.count(); i++)
					{
						vecStatus.push_back(sarr.at(i).toInt());
					}

					for (int i = 0; i < rpos.count(); i++)
					{
						vecRpos.push_back(rpos.at(i).toInt());
					}

					for (int i = 0; i < bpos.count(); i++)
					{
						vecBpos.push_back(bpos.at(i).toInt());
					}
				}
			}

			// datasamples 的所有权属于 msr。不要先 new[] 再覆盖指针，
			// 否则每处理一条 miniSEED 记录都会泄漏一块内存。
			const float* arrVal = static_cast<const float*>(msr->datasamples);
			if (!arrVal || msr->numsamples <= 0)
				continue;
			for (int i = 0; i < msr->numsamples; i++)
			{
				vecPoint << arrVal[i];
			}

			/* Do something with the record here, e.g. print */
			msr3_print(msr, verbose);
		}

		/* Cleanup memory and close file */
		ms3_readmsr(&msr, NULL, flags, verbose);

		std::vector<std::vector<double>> vecPt;
		for (int i = 0; i < nChannum; i++)
		{
			QVector<double> values = vecPoint.mid(i * nPointNum, nPointNum);
			//vecPt.push_back(values.toStdVector());

			for (int k = 0; k < values.size(); k++)
			{
				if (i == 0)
				{
					std::vector<double> v;
					v.push_back(values.at(k));
					vecPt.push_back(v);
				}
				else
				{
					vecPt[k].push_back(values.at(k));
				}
			}
		}
		//m_vecOffch.clear();
		// 读取配置 valid_check：1=启用有效性判定；其他值(包括0)=不判定，所有事件视为有效事件
		int validCheck = AppConfig::Instance()->GetSettingValue(KEY_VALID_CHECK).toInt();
		int compliant = 1;
		if (validCheck == 1)
		{
			// 调用接口判断
			compliant = is_compliant_event(vecPt, vecStatus, vecRpos, vecBpos, m_vecOffch);
		}
		emit CallManage::getInstance()->sig_addLog("is_compliant_event ", QString("1# compliant:%1").arg(compliant));
		QString destPath = "";
		if (compliant == 1)
		{
			// 拷贝到 picker 文件夹（保持原文件名）
			destPath = m_pickerPath + QDir::separator() + info.fileName();
		}
		else
		{
			// 拷贝到 invalid 文件夹（保持原文件名）
			destPath = m_invalidPath + QDir::separator() + info.fileName();
		}
		QFile::setPermissions(absPath, QFile::ReadUser | QFile::WriteUser);
		if (QFile::copy(absPath, destPath)) {
			qDebug() << "Copied compliant file to" << destPath;
			// 拷贝成功后删除源文件
			if (!QFile::remove(absPath)) {

				qWarning() << "Failed to delete source file:" << absPath;
			}
		}
		else {
			qWarning() << "Failed to copy file to" << destPath;
		}

		// 标记为已处理（避免重复）
		m_processedFiles.insert(absPath);
	}

	// 可选：清理已删除文件的记录（简单起见，每次处理完不再清理，因为文件已不存在）
	// 若想避免 m_processedFiles 无限增长，可以定期移除不存在的文件
	QSet<QString> existing;
	for (const QFileInfo &info : files)
		existing.insert(info.absoluteFilePath());
	QMutableSetIterator<QString> it(m_processedFiles);
	while (it.hasNext()) {
		if (!existing.contains(it.next()))
			it.remove();
	}
}

bool MseedFileHandler::isCompliantEvent(const QString &filePath)
{
	// 调用现有接口
	return true;// ::is_compliant_event(filePath);
}

void MseedFileHandler::readOffch()
{
	QString strDir = QCoreApplication::applicationDirPath();
	QFile file(strDir+"/data/offch.txt");
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return;

	QTextStream stream(&file);
	while (!stream.atEnd()) {
		QString line = stream.readLine();
		m_vecOffch.push_back(line.toInt());
	}
}
