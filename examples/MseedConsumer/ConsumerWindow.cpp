#include "ConsumerWindow.h"
#include "MseedConsumer.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

ConsumerWindow::ConsumerWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_configPath(QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("config.ini"))
{
    setWindowTitle(QStringLiteral("mseed文件消费程序"));
    resize(760, 520);

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);

    QGroupBox* configBox = new QGroupBox(QStringLiteral("服务配置"), central);
    QFormLayout* form = new QFormLayout(configBox);
    m_serverEdit = new QLineEdit(configBox);
    m_tokenEdit = new QLineEdit(configBox);
    m_tokenEdit->setEchoMode(QLineEdit::Password);
    m_outputEdit = new QLineEdit(configBox);
    m_stateEdit = new QLineEdit(configBox);
    m_intervalSpin = new QSpinBox(configBox);
    m_intervalSpin->setRange(1, 3600);
    m_intervalSpin->setSuffix(QStringLiteral(" 秒"));
    form->addRow(QStringLiteral("服务器地址"), m_serverEdit);
    form->addRow(QStringLiteral("下载令牌"), m_tokenEdit);
    form->addRow(QStringLiteral("文件保存目录"), m_outputEdit);
    form->addRow(QStringLiteral("游标状态文件"), m_stateEdit);
    form->addRow(QStringLiteral("轮询间隔"), m_intervalSpin);
    root->addWidget(configBox);

    QHBoxLayout* buttons = new QHBoxLayout;
    m_saveButton = new QPushButton(QStringLiteral("保存配置"), central);
    m_startButton = new QPushButton(QStringLiteral("启动"), central);
    m_stopButton = new QPushButton(QStringLiteral("停止"), central);
    buttons->addWidget(m_saveButton);
    buttons->addWidget(m_startButton);
    buttons->addWidget(m_stopButton);
    buttons->addStretch();
    root->addLayout(buttons);

    QGroupBox* statusBox = new QGroupBox(QStringLiteral("运行状态"), central);
    QFormLayout* statusForm = new QFormLayout(statusBox);
    m_statusLabel = new QLabel(QStringLiteral("未启动"), statusBox);
    m_cursorLabel = new QLabel(QStringLiteral("0"), statusBox);
    m_lastFileLabel = new QLabel(QStringLiteral("-"), statusBox);
    m_lastFileLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    statusForm->addRow(QStringLiteral("状态"), m_statusLabel);
    statusForm->addRow(QStringLiteral("已确认游标"), m_cursorLabel);
    statusForm->addRow(QStringLiteral("最近文件"), m_lastFileLabel);
    root->addWidget(statusBox);

    m_logEdit = new QPlainTextEdit(central);
    m_logEdit->setReadOnly(true);
    root->addWidget(m_logEdit, 1);
    setCentralWidget(central);

    connect(m_saveButton, &QPushButton::clicked, this, &ConsumerWindow::saveSettings);
    connect(m_startButton, &QPushButton::clicked, this, &ConsumerWindow::startConsumer);
    connect(m_stopButton, &QPushButton::clicked, this, &ConsumerWindow::stopConsumer);

    createDefaultConfig();
    loadSettings();
    setRunning(false);
    startConsumer();
}
ConsumerWindow::~ConsumerWindow() = default;
void ConsumerWindow::createDefaultConfig() const
{
    if (QFile::exists(m_configPath))
        return;
    QSettings settings(m_configPath, QSettings::IniFormat);
    settings.setValue("Server/url", "http://39.96.41.239:443");
    settings.setValue("Server/downloadToken", "");
    settings.setValue("Consumer/pollSeconds", 5);
    settings.setValue("Consumer/outputDir", "data/mseed");
    settings.setValue("Consumer/stateFile", "data/state/cursor.txt");
    settings.sync();
}

void ConsumerWindow::loadSettings()
{
    QSettings settings(m_configPath, QSettings::IniFormat);
    m_serverEdit->setText(settings.value("Server/url").toString());
    m_tokenEdit->setText(settings.value("Server/downloadToken").toString());
    m_outputEdit->setText(settings.value("Consumer/outputDir", "data/mseed").toString());
    m_stateEdit->setText(settings.value("Consumer/stateFile", "data/state/cursor.txt").toString());
    m_intervalSpin->setValue(settings.value("Consumer/pollSeconds", 5).toInt());
}

void ConsumerWindow::saveSettings()
{
    QSettings settings(m_configPath, QSettings::IniFormat);
    settings.setValue("Server/url", m_serverEdit->text().trimmed());
    settings.setValue("Server/downloadToken", m_tokenEdit->text().trimmed());
    settings.setValue("Consumer/outputDir", m_outputEdit->text().trimmed());
    settings.setValue("Consumer/stateFile", m_stateEdit->text().trimmed());
    settings.setValue("Consumer/pollSeconds", m_intervalSpin->value());
    settings.sync();
    appendLog(QStringLiteral("配置已保存：%1").arg(m_configPath));
}

void ConsumerWindow::startConsumer()
{
    if (m_serverEdit->text().trimmed().isEmpty() || m_tokenEdit->text().trimmed().isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请填写服务器地址和下载令牌"));
        appendLog(QStringLiteral("未启动：config.ini 中服务器地址或下载令牌为空"));
        return;
    }
    saveSettings();
    m_consumer.reset(new MseedConsumer(
        QUrl(m_serverEdit->text().trimmed()),
        m_tokenEdit->text().trimmed().toUtf8(),
        absoluteConfigPath(m_outputEdit->text().trimmed()),
        absoluteConfigPath(m_stateEdit->text().trimmed()),
        m_intervalSpin->value(), this));

    connect(m_consumer.get(), &MseedConsumer::statusChanged,
            m_statusLabel, &QLabel::setText);
    connect(m_consumer.get(), &MseedConsumer::cursorChanged, this, [this](qint64 id) {
        m_cursorLabel->setText(QString::number(id));
    });
    connect(m_consumer.get(), &MseedConsumer::fileDownloaded, this,
            [this](qint64 id, const QString& name, const QString& path) {
                Q_UNUSED(id)
                Q_UNUSED(name)
                m_lastFileLabel->setText(path);
            });
    connect(m_consumer.get(), &MseedConsumer::logMessage,
            this, &ConsumerWindow::appendLog);

    if (!m_consumer->start()) {
        m_consumer.reset();
        setRunning(false);
        return;
    }
    setRunning(true);
}

void ConsumerWindow::stopConsumer()
{
    if (m_consumer)
        m_consumer->stop();
    m_consumer.reset();
    setRunning(false);
}

void ConsumerWindow::appendLog(const QString& message)
{
    m_logEdit->appendPlainText(
        QStringLiteral("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"), message));
}

QString ConsumerWindow::absoluteConfigPath(const QString& value) const
{
    QFileInfo info(value);
    if (info.isAbsolute())
        return info.absoluteFilePath();
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(value);
}

void ConsumerWindow::setRunning(bool running)
{
    m_serverEdit->setEnabled(!running);
    m_tokenEdit->setEnabled(!running);
    m_outputEdit->setEnabled(!running);
    m_stateEdit->setEnabled(!running);
    m_intervalSpin->setEnabled(!running);
    m_saveButton->setEnabled(!running);
    m_startButton->setEnabled(!running);
    m_stopButton->setEnabled(running);
}

