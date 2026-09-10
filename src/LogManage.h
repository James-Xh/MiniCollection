#ifndef LogManage_H
#define LogManage_H

#include <QObject>
#include <QString>
#include <QFile>
#include <QTextStream>

class LogManage : public QObject
{
    Q_OBJECT
public:
    explicit LogManage(QObject *parent = nullptr);
    ~LogManage();
    static LogManage* Instance()
    {
        static LogManage _instance;
        return &_instance;
    }


    void Init();

    void createUnexistDir(const QString& path);

    void addLog(QString stype, QString sdesc, bool bisShow=true);


    void writeLog(QString sLog);


signals:
    void logAdded(QString logDate, QString logType, QString logInfo);

public slots:
	void onAddLog(QString stype, QString sval, bool bs);

private:
    QFile               m_logFile;
    QTextStream         m_TextStream;
    QList<QString>      m_listType;
    QMap<qint64, QString> m_mapFile;

};

#define LOGMgr (LogManage::Instance())

#endif // LogManage_H
