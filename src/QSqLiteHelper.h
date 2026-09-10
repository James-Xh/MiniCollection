#pragma once

#include <QObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QMutex>
#include <QDateTime>
class QSqLiteHelper : public QObject
{
	Q_OBJECT

public:	//当前项目接口

	QString getErrorMsg();

public:	//通用接口

	~QSqLiteHelper();
	QSqLiteHelper(const QSqLiteHelper&) = delete;
	QSqLiteHelper& operator=(const QSqLiteHelper&) = delete;

	static QSqLiteHelper* getInstance();

	/**
	 * 从数据库中获取数据，失败则返回空数据
	 *
	 * @param tablename 表名
	 * @param columnList 所要获取的列名称，为空则获取所有列
	 * @param condition 查找条件，包含"where"关键字，例如"where id=10"
	 * @param limit 查询条数，小于0表示无限制
	 * @return 每一个QStringList保存一行数据，顺序与columnList相对应
	 */
	QList<QStringList> selectData(const QString& tablename, const QStringList& columnList, const QString& condition = "", int limit = -1);

	/**
	 * 插入单条数据
	 *
	 * @param tablename 表名
	 * @param columnList 所要插入的列名称，数量可以少于数据库中的列
	 * @param dataList 所要插入的数据，与"columnList"一一对应
	 * @return 插入成功返回true,插入失败返回false
	 */
	bool insertSingleData(const QString& tablename, const QStringList& columnList, const QStringList& dataList);
	/**
	 * 更新已存在的数据
	 *
	 * @param tablename 表名
	 * @param columnList 所要更新的列名称，数量可以少于数据库中的列
	 * @param dataList 所要更新的数据，与"columnList"一一对应
	 * @param condition 查找条件，包含"where"关键字，例如"where id=10"
	 * @return 更新成功返回true，更新失败返回false
	 */
	bool updateSingleData(const QString& tablename, const QStringList& columnList, const QStringList& dataList, const QString& condition);

	/**
	 * 删除数据
	 *
	 * @param tablename 表名
	 * @param condition 删除条件，包含"where"关键字，例如"where id=10"
	 * @return 删除成功返回true，删除失败返回false
	 */
	bool deleteData(const QString& tablename, const QString& condition);

	bool executeSql(const QString& sql);

	// 创建业务所需但预置库中尚不存在的表（站点、分量配置）
	void ensureSchema();

	QList<QStringList> selectDataWithSql(const QString& sql, const QString& errorTip = "");

	QString getUUid();
private:
	QSqLiteHelper() = default;

private:
	static QSqLiteHelper* m_instance;
	static QMutex m_mutex;

	QString m_errorMsg;
};

#define SQLMgr (QSqLiteHelper::getInstance())
