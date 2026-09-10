#include <QtWidgets/QApplication>
#include <QThread>

#include "Commdef.h"
#include "AppConfig.h"
#include "LogManage.h"
#include "CallManage.h"
#include "QSqLiteHelper.h"
#include "TcpDataManager.h"
#include "FrameParser.h"
#include "MDataPickder.h"
#include "MDataWriter.h"
#include "MseedFileHandler.h"
#include "StatusWindow.h"
#include <QCoreApplication>
#include <QDir>
#include <QThread>

// [Para] 参数默认值（与原项目 Config.ini 一致）。
// 启动时若 Config.ini 缺少某项则写入默认值，保证界面初始化时能读到完整参数。
static void ensureDefaultParams()
{
	struct Para { const char* key; const char* value; };
	static const Para defaults[] = {
		{ "tri_on",        "6"     },   // 信号开始阈值
		{ "tri_off",       "2"     },   // 信号结束阈值
		{ "nsta",          "50"    },   // 短窗长度(样点数)
		{ "nlta",          "500"   },   // 长窗长度(样点数)
		{ "detect_ch",     "4"     },   // 触发达标最小通道数
		{ "qualified_ch",  "4"     },   // 限制达标最小通道数
		{ "energy_thre",   "0.25"  },   // 能量限制
		{ "amp_thre",      "0.25"  },   // 强度限制
		{ "intrach",       "100"   },   // 单通道相位间隔点数
		{ "interch",       "250"   },   // 多通道相位间隔点数
		{ "calc_win_len",  "2500"  },   // 计算窗口长度(样点数)
	};
	bool changed = false;
	for (const Para& p : defaults) {
		const QVariant cur = AppConfig::Instance()->getConfig("Para", p.key);
		if (!cur.isValid() || cur.toString().trimmed().isEmpty()) {
			AppConfig::Instance()->setConfig("Para", p.key, p.value);
			changed = true;
		}
	}
	if (changed)
		AppConfig::Instance()->syncConfig();
}

// 精简版采集程序入口：
// 无登录/无波形/无用户管理/无样式资源，仅保留：
//   TCP数据接收 -> 帧解析 -> 跨站同步 -> STALTA计算 -> 事件mseed -> 文件分发
int main(int argc, char* argv[])
{
	QApplication a(argc, argv);
	a.setApplicationName("MiniCollection");

	// 初始化配置与目录（data/Config.ini、data/temp、data/picker、data/bins、Log）
	AppConfig::Instance();

	// 参数默认值补齐（在界面初始化之前，确保读入完整的参数）
	ensureDefaultParams();

	// 数据库：确保站点配置表存在，并补齐分量记录
	SQLMgr->ensureSchema();
	TcpDataManager::migrateStationsFromIni();   // Config.ini [SverN] 导入 tbl_station（仅缺失时）
	TcpDataManager::syncCompTables();

	// 站点连接管理
	TcpDataManager* tcpMgr = TcpDataManager::instance();
	tcpMgr->initFromConfig();

	// 帧解析器（主线程，帧通过队列信号到达）
	FrameParser* parser = new FrameParser(&a);

	// 连续数据写线程（站0 -> data/bins）
	MDataWriter* writer = new MDataWriter();
	QThread* writerThread = new QThread(&a);
	writer->moveToThread(writerThread);
	QObject::connect(writerThread, &QThread::started, writer, &MDataWriter::initialize);
	QObject::connect(writerThread, &QThread::started, writer, &MDataWriter::startWrite);
	QObject::connect(writerThread, &QThread::finished, writer, &MDataWriter::deleteLater);
	QObject::connect(writerThread, &QThread::finished, writerThread, &QThread::deleteLater);
	writerThread->start();

	// 算法线程（跨站同步 + STALTA + 事件mseed生成 -> data/temp）
	MDataPickder* pickder = new MDataPickder();
	QThread* pickderThread = new QThread(&a);
	pickder->moveToThread(pickderThread);
	QObject::connect(pickderThread, &QThread::finished, pickder, &MDataPickder::deleteLater);
	QObject::connect(pickderThread, &QThread::finished, pickderThread, &QThread::deleteLater);
	pickderThread->start();

	// mseed 事件文件分发线程（temp -> picker / invalid）
	QString strDir = QCoreApplication::applicationDirPath();
	MseedFileHandler* mseedHandler = new MseedFileHandler(
		strDir + "/data/temp", strDir + "/data/picker", strDir + "/data/invalid");
	QThread* mseedThread = new QThread(&a);
	mseedHandler->moveToThread(mseedThread);
	QObject::connect(mseedThread, &QThread::started, mseedHandler, &MseedFileHandler::start);
	QObject::connect(mseedThread, &QThread::finished, mseedHandler, &MseedFileHandler::deleteLater);
	QObject::connect(mseedThread, &QThread::finished, mseedThread, &QThread::deleteLater);
	mseedThread->start();

	// 组装数据流：TCP帧 -> 解析 -> 算法/写文件
	QObject::connect(CallManage::getInstance(), &CallManage::sig_tcpData,
		parser, &FrameParser::onTcpData, Qt::QueuedConnection);
	parser->setPickder(pickder);
	parser->setWriter(writer);

	// 运行日志落盘（精简版UI简单，同步/拾取诊断统一写入 Log/*.log 便于排查）
	QObject::connect(CallManage::getInstance(), &CallManage::sig_addLog,
		&a, [](const QString& type, const QString& desc) {
			LOGMgr->addLog(type, desc);
		}, Qt::QueuedConnection);

	// 精简界面：站点/通道在线状态 + 同步状态 + 最简参数设置
	StatusWindow window;
	window.setPickder(pickder);
	window.show();

	// 启动所有站点连接（精简版无登录流程）
	tcpMgr->startAll();

	int ret = a.exec();

	tcpMgr->stopAll();

	// ===== 优雅关闭：先停 worker，再停线程，避免 "QThread: Destroyed while
	// thread is still running"（QApplication 析构时线程仍存活）=====
	// 1) 让 worker 在各自线程中停止内部定时器/文件句柄（阻塞等待执行完成）
	QMetaObject::invokeMethod(writer, "stopWrite", Qt::BlockingQueuedConnection);
	QMetaObject::invokeMethod(mseedHandler, "stop", Qt::BlockingQueuedConnection);
	// 2) 退出各线程事件循环并等待结束
	auto stopThread = [](QThread* t) {
		if (t && t->isRunning()) {
			t->quit();
			if (!t->wait(5000))
				t->terminate();
		}
	};
	stopThread(writerThread);
	stopThread(pickderThread);
	stopThread(mseedThread);

	return ret;
}
