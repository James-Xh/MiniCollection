#include "StatusWindow.h"
#include "Commdef.h"
#include "CallManage.h"
#include "TcpDataManager.h"
#include "MDataPickder.h"
#include "AppConfig.h"
#include "LogManage.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileInfo>
#include <QDateTime>
#include <cstring>

StatusWindow::StatusWindow(QWidget* parent)
	: QWidget(parent)
{
	memset(m_online, 0, sizeof(m_online));
	setWindowTitle(QStringLiteral("微震数据采集"));
	resize(860, 560);

	m_table = new QTableWidget(this);
	m_table->setColumnCount(6);
	m_table->setHorizontalHeaderLabels({ QStringLiteral("站号"),
		QStringLiteral("IP:Port"), QStringLiteral("连接状态"),
		QStringLiteral("在线通道"), QStringLiteral("GPS"),
		QStringLiteral("最近事件") });
	m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
	m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

	m_syncLabel = new QLabel(QStringLiteral("同步状态: 等待数据"), this);

	m_log = new QPlainTextEdit(this);
	m_log->setReadOnly(true);
	m_log->setMaximumBlockCount(500);

	QVBoxLayout* mainLayout = new QVBoxLayout(this);
	mainLayout->addWidget(m_table, 3);
	mainLayout->addWidget(m_syncLabel);
	mainLayout->addWidget(initParamFormWidget(), 0);
	mainLayout->addWidget(new QLabel(QStringLiteral("日志:"), this), 0);
	mainLayout->addWidget(m_log, 2);

	// 按已配置站生成表格行
	const QList<TcpDataManager::StationCfg> configs = TcpDataManager::loadStationConfigs();
	m_table->setRowCount(configs.size());
	for (int i = 0; i < configs.size(); ++i) {
		const TcpDataManager::StationCfg& cfg = configs.at(i);
		m_table->setItem(i, 0, new QTableWidgetItem(QString::number(cfg.id + 1)));
		m_table->setItem(i, 1, new QTableWidgetItem(
			QString("%1:%2").arg(cfg.ip).arg(cfg.port)));
		m_table->setItem(i, 2, new QTableWidgetItem(QStringLiteral("未连接")));
		m_table->setItem(i, 3, new QTableWidgetItem("0/" + QString::number(CHANNEL_COUNT)));
		m_table->setItem(i, 4, new QTableWidgetItem("-"));
		m_table->setItem(i, 5, new QTableWidgetItem(""));
	}

	connect(CallManage::getInstance(), &CallManage::sig_staConnectStatus,
		this, &StatusWindow::onStationConnectStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_devStatus,
		this, &StatusWindow::onDevStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_gpsStatus,
		this, &StatusWindow::onGpsStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_pickerSyncStatus,
		this, &StatusWindow::onPickerSyncStatus, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_pickerNewFile,
		this, &StatusWindow::onPickerNewFile, Qt::QueuedConnection);
	connect(CallManage::getInstance(), &CallManage::sig_addLog,
		this, &StatusWindow::onAddLog, Qt::QueuedConnection);
}

QWidget* StatusWindow::initParamFormWidget()
{
	// ntag 顺序必须与 MDataPickder::changeCallcValue 一致
	m_paramNames << "tri_on" << "tri_off" << "nsta" << "nlta"
		<< "" /*4: calc_win_len 不开放*/ << "detect_ch"
		<< "" << "qualified_ch" << "energy_thre" << "amp_thre"
		<< "intrach" << "interch";
	for (int tag = 0; tag < m_paramNames.size(); ++tag)
		m_paramTags.append(tag);

	QGroupBox* box = new QGroupBox(QStringLiteral("算法参数(Config.ini [Para])"), this);
	QFormLayout* form = new QFormLayout(box);

	for (int tag : m_paramTags) {
		const QString& name = m_paramNames.at(tag);
		if (name.isEmpty())
			continue;
		QLineEdit* edit = new QLineEdit(
			AppConfig::Instance()->getConfig("Para", name).toString(), box);
		m_paramEdits.append(edit);
		form->addRow(name, edit);
	}

	QPushButton* applyBtn = new QPushButton(QStringLiteral("应用"), box);
	connect(applyBtn, &QPushButton::clicked, this, &StatusWindow::onApplyParams);
	form->addRow(QString(), applyBtn);
	return box;
}

int StatusWindow::stationRow(int stationId)
{
	for (int i = 0; i < m_table->rowCount(); ++i) {
		if (m_table->item(i, 0)->text().toInt() == stationId + 1)
			return i;
	}
	return -1;
}
void StatusWindow::onStationConnectStatus(int stationId, bool connected)
{
	const int row = stationRow(stationId);
	if (row < 0) return;
	m_table->item(row, 2)->setText(connected
		? QStringLiteral("在线") : QStringLiteral("离线"));
	// 连接状态变化时重置该站在线通道计数并刷新显示
	if (stationId >= 0 && stationId < MAX_STATION)
		m_online[stationId] = 0;
	m_table->item(row, 3)->setText("0/" + QString::number(CHANNEL_COUNT));
}

void StatusWindow::onDevStatus(int stationId, int channel, int state, int typeBit)
{
	Q_UNUSED(typeBit);
	const int row = stationRow(stationId);
	if (row < 0 || channel < 0 || channel >= CHANNEL_COUNT || stationId < 0
		|| stationId >= MAX_STATION)
		return;

	if (state == 1) ++m_online[stationId];
	else if (state == 0) m_online[stationId] = qMax(0, m_online[stationId] - 1);
	m_table->item(row, 3)->setText(
		QString::number(m_online[stationId]) + "/" + QString::number(CHANNEL_COUNT));
	// 诊断：通道状态变化（事件仅状态切换时发出，频率低）
	LOGMgr->addLog("ChanStatus",
		QStringLiteral("站%1 通道%2 状态=%3 在线通道=%4")
			.arg(stationId + 1).arg(channel).arg(state).arg(m_online[stationId]));
}

void StatusWindow::onGpsStatus(int stationId, int gps)
{
	const int row = stationRow(stationId);
	if (row < 0) return;
	m_table->item(row, 4)->setText(QString::number(gps));
}

void StatusWindow::onPickerSyncStatus(int state, qint64 maxDiffUs,
	int stationId, const QString& message)
{
	Q_UNUSED(stationId);
	QString stateStr = (state == 1) ? QStringLiteral("同步正常")
		: (state == 2) ? QStringLiteral("同步异常") : QStringLiteral("等待数据");
	m_syncLabel->setText(QStringLiteral("同步状态: %1 (最大时间差 %2 ms) - %3")
		.arg(stateStr).arg(maxDiffUs / 1000.0, 0, 'f', 1).arg(message));
}

void StatusWindow::onPickerNewFile(const QString& file)
{
	if (m_table->rowCount() > 0) {
		m_table->item(0, 5)->setText(
			QDateTime::currentDateTime().toString("hh:mm:ss ") + QFileInfo(file).fileName());
	}
	m_log->appendPlainText(QStringLiteral("[事件] 新mseed: %1").arg(file));
}

void StatusWindow::onAddLog(const QString& type, const QString& desc)
{
	m_log->appendPlainText(QString("[%1] %2: %3")
		.arg(QDateTime::currentDateTime().toString("hh:mm:ss")).arg(type).arg(desc));
}

void StatusWindow::onApplyParams()
{
	int editIndex = 0;
	for (int tag : m_paramTags) {
		const QString& name = m_paramNames.at(tag);
		if (name.isEmpty())
			continue;
		QLineEdit* edit = m_paramEdits.at(editIndex++);
		if (!edit) continue;
		const QString value = edit->text().trimmed();
		AppConfig::Instance()->setConfig("Para", name, value);
		if (m_pickder) {
			QMetaObject::invokeMethod(m_pickder, "changeCallcValue",
				Qt::QueuedConnection,
				Q_ARG(int, tag), Q_ARG(QString, value));
		}
	}
	AppConfig::Instance()->syncConfig();
	m_log->appendPlainText(QStringLiteral("[设置] 算法参数已应用"));
}

