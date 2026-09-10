#include "ConnectionPool.h"
#include <QCoreApplication>
#include <QSqlQuery>
#include <qsqlerror.h>
#include <QDebug>

QMutex ConnectionPool::mutex;
QThreadStorage<QString> ConnectionPool::threadConnectionNames;
quint64 ConnectionPool::nextConnectionId = 0;

ConnectionPool::ConnectionPool()
{
	m_bTestOnBorrow = true;
	m_strTestOnBorrowSql = "SELECT 1";

	initConfig();
}

ConnectionPool::~ConnectionPool()
{
	// QSqlDatabase 连接必须在创建它的线程中使用/销毁。
	// 这些持久的每线程连接由 Qt 在应用退出时统一清理。
}

ConnectionPool& ConnectionPool::getInstance()
{
	static ConnectionPool ref;
	return ref;
}

void ConnectionPool::initConfig()
{
	QString spath = QCoreApplication::applicationDirPath();
	m_databaseName = spath + "/data/weizhen.db";

	m_databaseType = "QSQLITE";//数据库类型
}

QSqlDatabase ConnectionPool::openConnection()
{
	ConnectionPool& pool = ConnectionPool::getInstance();
	QString connectionName;

	{
		QMutexLocker locker(&mutex);
		if (!threadConnectionNames.hasLocalData()) {
			threadConnectionNames.setLocalData(
				QString("Connection-Thread-%1").arg(++nextConnectionId));
		}
		connectionName = threadConnectionNames.localData();
	}

	QSqlDatabase db;
	if (QSqlDatabase::contains(connectionName)) {
		db = QSqlDatabase::database(connectionName, false);
		if (!db.isOpen() && !db.open()) {
			qDebug() << "Open database error:" << db.lastError().text();
			return QSqlDatabase();
		}
	} else {
		db = pool.createConnection(connectionName);
	}

	return db;
}

void ConnectionPool::closeConnection(QSqlDatabase connection)
{
	// 每个线程复用自己的持久 SQLite 连接。这里不关闭，以免在
	// QSqlQuery 尚未析构时 removeDatabase，也避免将连接转交给其他线程。
	Q_UNUSED(connection);
}

QSqlDatabase ConnectionPool::createConnection(const QString &connectionName)
{
	// 连接已经创建过了，复用它，而不是重新创建
	//if (QSqlDatabase::contains(connectionName))
	//{
	//	QSqlDatabase db1 = QSqlDatabase::database(connectionName);

	//	if (m_bTestOnBorrow)
	//	{
	//		// 返回连接前访问数据库，如果连接断开，重新建立连接
	//		//qDebug() << "Test connection on borrow, execute:" << testOnBorrowSql << ", for" << connectionName;
	//		QSqlQuery query(m_strTestOnBorrowSql, db1);

	//		qDebug() << db1.isOpen();
	//		if (query.lastError().type() != QSqlError::NoError && !db1.open())
	//		{
	//			qDebug() << "Open datatabase error:" << db1.lastError().text();
	//			return QSqlDatabase();
	//		}
	//	}

	//	return db1;
	//}

	// 创建一个新的连接
	QSqlDatabase db = QSqlDatabase::addDatabase(m_databaseType, connectionName);
	db.setConnectOptions("MYSQL_OPT_RECONNECT=1");
	db.setDatabaseName(m_databaseName);

	if (!db.open())
	{
		qDebug() << "Open datatabase error:" << db.lastError().text();
		return QSqlDatabase();
	}

	return db;
}
