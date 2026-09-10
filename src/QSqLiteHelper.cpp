#include "QSqLiteHelper.h"
#include "ConnectionPool.h"
#include <QVariant>
#include <QDebug>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QUuid>



QSqLiteHelper* QSqLiteHelper::m_instance = nullptr;
QMutex QSqLiteHelper::m_mutex;

QSqLiteHelper::~QSqLiteHelper()
{
	m_instance = nullptr;
}

QSqLiteHelper* QSqLiteHelper::getInstance()
{
	if (m_instance == nullptr)
	{
		QMutexLocker locker(&m_mutex);
		if (m_instance == nullptr)
		{
			m_instance = new QSqLiteHelper();
		}
	}
	return m_instance;
}

QList<QStringList> QSqLiteHelper::selectData(const QString & tablename, const QStringList & columnList, const QString & condition, int limit)
{
	QMutexLocker locker(&m_mutex);

	if (tablename.isEmpty())
	{
		return QList<QStringList>();
	}

	int columnCount = columnList.size();
	QString keyStr;

	if (columnCount <= 0)
	{
		keyStr = "*";
	}
	else
	{
		for (int col = 0; col < columnCount; col++) {
			keyStr.append(columnList.at(col) + ", ");
		}
		keyStr = keyStr.left(keyStr.size() - 2);
	}
	if (keyStr.isEmpty())
	{
		keyStr = "*";
	}

	QString sql("select " + keyStr + " from " + tablename + " " + condition + ";");

	if (limit >= 0)
	{
		sql = sql.remove(';').append(" limit " + QString::number(limit) + ";");
	}

	return selectDataWithSql(sql, "selectData: ");
}

bool QSqLiteHelper::insertSingleData(const QString & tablename, const QStringList & columnList, const QStringList & dataList)
{
	QMutexLocker locker(&m_mutex);

	if (tablename.isEmpty())
	{
		return false;
	}

	int columnCount = columnList.size();
	if (dataList.size() != columnCount) {
		return false;
	}

	QString keyStr, valueStr;
	for (int i = 0; i < columnCount; i++) {
		keyStr.append(columnList.at(i) + ", ");
		valueStr.append(":" + columnList.at(i) + ", ");
	}
	keyStr = keyStr.left(keyStr.size() - 2);
	valueStr = valueStr.left(valueStr.size() - 2);

	QSqlDatabase db = ConnectionPool::openConnection();
	QSqlQuery query(db);

	//insert into table (key...) values(:key...)
	bool res = query.prepare("insert into " + tablename + " ( " + keyStr + " ) values( " + valueStr + " );");
	if (!res) {
		QString s = query.lastError().text();
		ConnectionPool::closeConnection(db);
		return false;
	}

	for (int i = 0; i < columnCount; i++) {
		query.bindValue(":" + columnList.at(i), dataList.at(i));
	}

	res = query.exec();
	if (!res) {
		QString s = query.lastError().text();
		ConnectionPool::closeConnection(db);
		return false;
	}
	ConnectionPool::closeConnection(db);
	return res;
}

QList<QStringList> QSqLiteHelper::selectDataWithSql(const QString & sql, const QString & errorTip)
{
	QSqlDatabase db = ConnectionPool::openConnection();
	QSqlQuery query(db);
	if (!query.exec(sql)) {
		m_errorMsg = query.lastError().text();
		qDebug() << errorTip << m_errorMsg;
		ConnectionPool::closeConnection(db);
		return QList<QStringList>();
	}

	//读取数据
	QList<QStringList> dataMatrix;
	while (query.next())
	{
		QStringList dataList;
		int i = 0;
		while (true)
		{
			QVariant v = query.value(i++);
			if (!v.isValid()) {
				break;
			}
			dataList.push_back(v.toString());
		}
		dataMatrix.push_back(dataList);
	}

	ConnectionPool::closeConnection(db);
	return dataMatrix;
}

bool QSqLiteHelper::updateSingleData(const QString & tablename, const QStringList & columnList, const QStringList & dataList, const QString & condition)
{
	QMutexLocker locker(&m_mutex);

	if (tablename.isEmpty())
	{
		return false;
	}

	int columnCount = columnList.size();
	if (dataList.size() != columnCount) {
		return false;
	}

	QString setStr;
	for (int i = 0; i < columnCount; i++) {
		setStr.append(columnList.at(i) + " = :" + columnList.at(i) + ", ");
	}
	setStr = setStr.left(setStr.size() - 2);

	QSqlDatabase db = ConnectionPool::openConnection();
	QSqlQuery query(db);

	bool res = query.prepare("update " + tablename + " set " + setStr + " " + condition + " ;");
	if (!res) {
		ConnectionPool::closeConnection(db);
		return false;
	}

	for (int i = 0; i < columnCount; i++) {
		query.bindValue(":" + columnList.at(i), dataList.at(i));
	}

	res = query.exec();
	ConnectionPool::closeConnection(db);
	return res;
}

bool QSqLiteHelper::deleteData(const QString & tablename, const QString & condition)
{
	if (tablename.isEmpty())
	{
		return false;
	}

	QSqlDatabase db = ConnectionPool::openConnection();
	QSqlQuery query(db);
	
	QString sql = QString("delete from %1 %2;").arg(tablename).arg(condition);
	bool res = query.exec(sql);
	if (!res) {
		m_errorMsg = query.lastError().text();
	}
	ConnectionPool::closeConnection(db);

	return res;
}

bool QSqLiteHelper::executeSql(const QString& sql)
{
	QMutexLocker locker(&m_mutex);

	if (sql.isEmpty())
	{
		return false;
	}

	QSqlDatabase db = ConnectionPool::openConnection();
	QSqlQuery query(db);

	bool res = query.exec(sql);
	if (!res) {
		m_errorMsg = query.lastError().text();
	}
	ConnectionPool::closeConnection(db);

	return res;
}

QString QSqLiteHelper::getErrorMsg()
{
	return m_errorMsg;
}

void QSqLiteHelper::ensureSchema()
{
	// 站点连接信息（原 [SverN] 配置迁移至此）。ID 为 1-based，与旧配置段号一致。
	executeSql(
		"CREATE TABLE IF NOT EXISTS tbl_station ("
		"ID INTEGER PRIMARY KEY, "
		"IP TEXT, "
		"PORT INTEGER, "
		"CAN_IP TEXT, "
		"CAN_PORT INTEGER"
		");");

	// 分量自动/手动标志，按站保存（原 [Comp]/auto）。
	executeSql(
		"CREATE TABLE IF NOT EXISTS tbl_station_comp ("
		"STATION_ID INTEGER PRIMARY KEY, "
		"AUTO INTEGER DEFAULT 1"
		");");

	// 每站每通道的三分量标志（原 [Comp]/chN），STATION_ID 1-based，CHANNEL 1..16。
	executeSql(
		"CREATE TABLE IF NOT EXISTS tbl_channel_comp ("
		"STATION_ID INTEGER, "
		"CHANNEL INTEGER, "
		"THREE_COMP INTEGER DEFAULT 0, "
		"PRIMARY KEY(STATION_ID, CHANNEL)"
		");");
}

QString QSqLiteHelper::getUUid()
{
	QUuid id = QUuid::createUuid();
	QString strId = id.toString();
	strId.replace("{", "");
	strId.replace("}", "");
	strId.replace("-", "");
	//qDebug() << strId;
	return strId;
}