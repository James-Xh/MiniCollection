#include "LogManage.h"
#include <QFile>
#include <QDir>
#include <QRegExp>
#include <QLocale>
#include <QDateTime>
#include <QTextStream>
#include <QDebug>
#include "CallManage.h"
#include <QCoreApplication>

LogManage::LogManage(QObject *parent)
{
    Q_UNUSED(parent);

    this->Init();
}

LogManage::~LogManage()
{
    m_logFile.close();
}

void LogManage::Init()
{
    QString strLog = QCoreApplication::applicationDirPath();
    strLog += "/Log/";

    createUnexistDir(strLog);

    QString strName = "log_";
    QDateTime current_date_time =QDateTime::currentDateTime();
    QString current_date =current_date_time.toString("yyyy-MM-dd_hh-mm-ss");
    strName += current_date;
    strName += ".log";
    strLog += strName;

    m_logFile.setFileName(strLog);

	//connect(CallManage::getInstance(), &CallManage::sigAddLog, this, &LogManage::onAddLog, Qt::QueuedConnection);


}

void LogManage::createUnexistDir(const QString& path)
{
	QDir dir(path);
	if (dir.exists())return;
	dir.mkpath(path);
}

void LogManage::addLog(QString stype, QString sdesc, bool bisShow)
{
    if(stype == "TEST")return;
    QDateTime current_date_time =QDateTime::currentDateTime();
    qint64 nsecTime = current_date_time.toMSecsSinceEpoch();
    QString current_date =current_date_time.toString("yyyy-MM-dd hh:mm:ss");

    if(bisShow)
    {
		//QSqLiteHelper::getInstance()->insertGasSystemLog();
        //emit this->logAdded(current_date, stype, sdesc);
    }

    QString strLog = "";
    //strLog += QString::number(m_vecNewLogItem.size());
    //strLog += "|";
    strLog += current_date;
    strLog += "|";
	strLog += stype;
	strLog += "|";
    strLog += sdesc;

    this->writeLog(strLog);
}

void LogManage::writeLog(QString sLog)
{
    if(!m_logFile.isOpen())
    {
        bool bret = m_logFile.open(QIODevice::ReadWrite | QIODevice::Text);
        if(!bret)
            return;
    }

    QTextStream stream(&m_logFile);
    qint64 nsize = m_logFile.size();
    stream.seek(nsize);
    stream << sLog << "\n";
}

void LogManage::onAddLog(QString stype, QString sval, bool bs)
{
	this->addLog(stype, sval, bs);
}

