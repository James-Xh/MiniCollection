#pragma once

#include <QSqlDatabase>
#include <QMutex>
#include <QThreadStorage>

class ConnectionPool
{
public:
	static QSqlDatabase openConnection();                 // 获取数据库连接
	static void closeConnection(QSqlDatabase connection); // 释放数据库连接回连接池

	virtual ~ConnectionPool();

	static ConnectionPool& getInstance();

private:
	ConnectionPool();
	ConnectionPool(const ConnectionPool &other);
	ConnectionPool& operator=(const ConnectionPool &other);
	QSqlDatabase createConnection(const QString &connectionName); // 创建数据库连接

	void initConfig();

	// 数据库信息
	//QString m_hostName;
	QString m_databaseName;
	//QString m_username;
	//QString m_password;
	QString m_databaseType;
	//unsigned short m_usPort;

	bool    m_bTestOnBorrow;    // 取得连接的时候验证连接是否有效
	QString m_strTestOnBorrowSql; // 测试访问数据库的 SQL

	static QMutex mutex;
	static QThreadStorage<QString> threadConnectionNames;
	static quint64 nextConnectionId;
};
