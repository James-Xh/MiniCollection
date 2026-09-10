#include "AppConfig.h"
#include <QDir>
#include <QCoreApplication>
#include <QSettings>
#include <QDateTime>
#include "QDebug"

AppConfig* AppConfig::m_instance = nullptr;
QMutex AppConfig::m_Mutex;

AppConfig* AppConfig::Instance()
{
	if (m_instance == nullptr) {
		QMutexLocker locker(&m_Mutex);
		if (m_instance == nullptr)
		{
			m_instance = new AppConfig();
		}
	}
	return m_instance;
}

bool AppConfig::LoadAppSetting(QString strPath)
{
	if (!QFile::exists(strPath))
	{
		qDebug() << "[App Info] load app info failed";
	}

	m_pSetting = new QSettings(strPath, QSettings::IniFormat, NULL);
	return true;
}

QVariant AppConfig::GetSettingValue(const QString& strKey)
{
	if (m_pSetting)
	{
		return m_pSetting->value(strKey);
	}

	return "";
}

bool AppConfig::SetSettingValue(const QString& strKey, const QVariant& value)
{
	if (m_pSetting)
	{
		m_pSetting->setValue(strKey, value);

		return true;
	}

	return false;
}

AppConfig::AppConfig() : QObject()
{
	LoadAppSetting(configFilePath());
	initDirs();
}

AppConfig::~AppConfig()
{
	delete m_pSetting;
	m_pSetting = nullptr;
}

QString AppConfig::AppConfigPath()
{
    return QCoreApplication::applicationDirPath();
}

void AppConfig::initDirs()
{
    QString path = AppConfigPath();
    
    createUnexistDir(path + "/Log"); //日志文件夹
	createUnexistDir(getTempPickerPath());      //算法拾取到的临时
    createUnexistDir(getPickerPath());      //算法拾取到的
    createUnexistDir(getBinsPath());        //连续存储路径

	// 备用机数据补齐默认配置（首次运行时写入默认值，可在Config.ini中修改）
	if (!m_pSetting->contains("/BackupMachine/ip"))
		m_pSetting->setValue("/BackupMachine/ip", "");
	if (!m_pSetting->contains("/BackupMachine/sharePath"))
		m_pSetting->setValue("/BackupMachine/sharePath", "");
	if (!m_pSetting->contains("/BackupMachine/localPath"))
		m_pSetting->setValue("/BackupMachine/localPath", getPickerPath());
	if (!m_pSetting->contains("/BackupMachine/lastMseedTime"))
		m_pSetting->setValue("/BackupMachine/lastMseedTime", "");
}

void AppConfig::createUnexistDir(const QString &path)
{
    QDir dir(path);
    if(dir.exists())return;
    dir.mkpath(path);
}

QString AppConfig::configFilePath()
{
	return AppConfigPath() + "/data/Config.ini";
}

QString AppConfig::getTempPickerPath()
{
	return AppConfigPath() + "/data/temp/";
}

QString AppConfig::getPickerPath()
{
    return AppConfigPath() + "/data/picker/";
}

QString AppConfig::getBinsPath()
{
    return AppConfigPath() + "/data/bins/";
}
