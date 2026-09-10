#ifndef APPCONFIG_H
#define APPCONFIG_H

#include <QObject>
#include <QSettings>
#include "QVariant"
#include "QMutex"
#define KEY_USER_NAME "user_name"
#define KEY_BACKUP_PATH "Config/backup_path" 
#define KEY_VALID_CHECK "Config/valid_check"
#define KEY_BACKUP_MACHINE "Config/backup_machine"
/**
 * @brief The App class
 * 程序初始化使用,包括配置文件创建,目录创建
 */
class AppConfig : public QObject
{
    Q_OBJECT
public:
	static AppConfig* Instance();
	bool LoadAppSetting(QString strPath);

	QVariant GetSettingValue(const QString& strKey);
	bool SetSettingValue(const QString& strKey, const QVariant& value);

	// 立即将配置写入磁盘
	void syncConfig() { m_pSetting->sync(); }

	QVariant getConfig(const QString& section, const QString& key) {
		return m_pSetting->value("/" + section + "/" + key);
	}
	//写入配置
	template<typename T>
	void setConfig(const QString& section, const QString& key, T value)
	{
		m_pSetting->setValue("/" + section + "/" + key, value);
		return;
	}

	void addStationCfg(const QString& strID, const QString& strIP, const QString& strPort)
	{
		m_pSetting->beginGroup(QString("Sver%1").arg(strID));
		m_pSetting->setValue("IP", strIP);
		m_pSetting->setValue("Port", strPort);
		m_pSetting->endGroup();
		m_pSetting->sync();
	}
	void delStationCfg(const QString& strID)
	{
		m_pSetting->beginGroup(QString("Sver%1").arg(strID));
		m_pSetting->remove(""); 
		m_pSetting->endGroup();
		m_pSetting->sync();
	}

    /**
     * @brief appPath 获取程序所在目录
     * @return
     */
    QString AppConfigPath();
   
    /**
     * @brief initDirs
     * 初始化一些程序必要的目录
     */
    void initDirs();

    void createUnexistDir(const QString& path);

    QString configFilePath();
   
	QString getTempPickerPath();

    QString getPickerPath();

    QString getBinsPath();
signals:

private:
	explicit AppConfig();
	virtual ~AppConfig();

private:
    QSettings *m_pSetting;
	QString m_strGasTag;
	QString m_strPTag;

	static AppConfig* m_instance;
	static QMutex m_Mutex;
};

#define APPCfg (AppConfig::Instance())

#endif // APPINIT_H
