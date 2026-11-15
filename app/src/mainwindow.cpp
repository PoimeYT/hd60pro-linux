// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.h"

#include <QApplication>
#include <QCamera>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QVideoWidget>

#include "settingsdialog.h"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), m_settings(AppSettings::load())
{
	setWindowTitle(tr("HD60 Pro"));
	setWindowIcon(QIcon(QStringLiteral(":/hd60pro.svg")));
	resize(960, 640);

	auto *central = new QWidget;
	auto *layout = new QVBoxLayout(central);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	m_stack = new QStackedWidget;
	m_video = new QVideoWidget;
	m_video->setAspectRatioMode(Qt::KeepAspectRatio);
	m_messagePage = new QWidget;
	m_messagePage->setAutoFillBackground(true);
	QPalette dark = m_messagePage->palette();
	dark.setColor(QPalette::Window, QColor(0x14, 0x16, 0x1a));
	dark.setColor(QPalette::WindowText, QColor(0xd8, 0xdc, 0xe2));
	m_messagePage->setPalette(dark);
	auto *msgLayout = new QVBoxLayout(m_messagePage);
	m_message = new QLabel;
	m_message->setAlignment(Qt::AlignCenter);
	m_message->setWordWrap(true);
	m_message->setTextInteractionFlags(Qt::TextSelectableByMouse);
	QFont big = m_message->font();
	big.setPointSizeF(big.pointSizeF() * 1.25);
	m_message->setFont(big);
	m_startDriver = new QPushButton(tr("Start the driver"));
	msgLayout->addStretch();
	msgLayout->addWidget(m_message);
	msgLayout->addSpacing(12);
	msgLayout->addWidget(m_startDriver, 0, Qt::AlignHCenter);
	msgLayout->addStretch();
	m_stack->addWidget(m_messagePage);
	m_stack->addWidget(m_video);
	layout->addWidget(m_stack, 1);

	auto *bar = new QWidget;
	auto *barLayout = new QHBoxLayout(bar);
	m_record = new QPushButton;
	m_record->setMinimumWidth(130);
	m_timer = new QLabel;
	m_timer->setMinimumWidth(70);
	m_listen = new QPushButton(tr("Listen"));
	m_listen->setCheckable(true);
	m_listen->setToolTip(tr("Play the game sound on this computer's headphones or speakers"));
	m_volume = new QSlider(Qt::Horizontal);
	m_volume->setRange(0, 150);
	m_volume->setFixedWidth(160);
	m_volume->setValue(m_settings.listenVolume);
	m_volume->setToolTip(tr("Listening volume"));
	m_settingsBtn = new QPushButton(tr("Settings…"));
	barLayout->addWidget(m_record);
	barLayout->addWidget(m_timer);
	barLayout->addStretch();
	barLayout->addWidget(m_listen);
	barLayout->addWidget(m_volume);
	barLayout->addSpacing(16);
	barLayout->addWidget(m_settingsBtn);
	layout->addWidget(bar);
	setCentralWidget(central);

	m_status = new QLabel;
	statusBar()->addWidget(m_status, 1);
	statusBar()->setSizeGripEnabled(false);

	m_session = new QMediaCaptureSession(this);
	m_session->setVideoOutput(m_video);

	connect(m_record, &QPushButton::clicked, this, &MainWindow::toggleRecord);
	connect(m_listen, &QPushButton::toggled, this, &MainWindow::setListen);
	connect(m_settingsBtn, &QPushButton::clicked, this, &MainWindow::openSettings);
	connect(m_startDriver, &QPushButton::clicked, this, [this] {
		m_startDriver->setEnabled(false);
		m_driver.startDriver();
		m_startDriver->setEnabled(true);
	});
	connect(m_volume, &QSlider::valueChanged, this, [this](int v) {
		m_settings.listenVolume = v;
		m_listener.setVolume(v);
	});
	connect(m_volume, &QSlider::sliderReleased, this, [this] { m_settings.save(); });

	connect(&m_driver, &DriverStatus::changed, this, &MainWindow::onDriver);
	connect(&m_recorder, &Recorder::started, this, [this] {
		m_notice.clear();
		updateRecordUi();
		updateStatusLine();
	});
	connect(&m_recorder, &Recorder::finished, this, [this](bool ok, const QString &msg, const QString &) {
		updateRecordUi();
		m_notice = msg.section(QLatin1Char('\n'), 0, 0);
		updateStatusLine();
		// A problem is never only a status-line note: it must be seen.
		if (!ok || msg.startsWith(tr("Recording stopped"))) {
			if (isVisible())
				QMessageBox::warning(this, tr("Recording"), msg);
			else if (m_tray)
				m_tray->showMessage(tr("HD60 Pro recording"), msg, QSystemTrayIcon::Warning, 15000);
		} else if (!isVisible() && m_tray) {
			m_tray->showMessage(tr("HD60 Pro"), msg, QSystemTrayIcon::Information, 5000);
		}
	});
	connect(&m_listener, &Listener::failed, this, [this](const QString &msg) {
		m_listen->setChecked(false);
		if (isVisible())
			QMessageBox::warning(this, tr("Listening"), msg);
		else if (m_tray)
			m_tray->showMessage(tr("HD60 Pro listening"), msg, QSystemTrayIcon::Warning, 15000);
	});
	connect(&m_listener, &Listener::playingChanged, this, [this] { updateStatusLine(); });

	connect(&m_tick, &QTimer::timeout, this, &MainWindow::updateRecordUi);
	m_tick.start(500);
	m_previewRetry.setInterval(1500);
	connect(&m_previewRetry, &QTimer::timeout, this, &MainWindow::updatePreview);

	if (QSystemTrayIcon::isSystemTrayAvailable()) {
		m_tray = new QSystemTrayIcon(windowIcon(), this);
		m_tray->setToolTip(tr("HD60 Pro"));
		auto *menu = new QMenu(this);
		menu->addAction(tr("Show HD60 Pro"), this, &MainWindow::bringToFront);
		m_trayListen = menu->addAction(tr("Listen"));
		m_trayListen->setCheckable(true);
		connect(m_trayListen, &QAction::triggered, m_listen, &QPushButton::setChecked);
		m_trayRecord = menu->addAction(tr("Record"), this, &MainWindow::toggleRecord);
		menu->addSeparator();
		menu->addAction(tr("Quit"), this, &MainWindow::quit);
		m_tray->setContextMenu(menu);
		connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
			if (r == QSystemTrayIcon::Trigger)
				isVisible() ? hide() : bringToFront();
		});
		m_tray->show();
	}

	m_listener.setVolume(m_settings.listenVolume);
	m_listener.setSink(m_settings.listenSink);
	m_listen->setChecked(m_settings.listenOn);	// restores Listen from the last session
	m_driver.refresh();
	updateRecordUi();
}

MainWindow::~MainWindow()
{
	stopPreview();
}

void MainWindow::bringToFront()
{
	show();
	setWindowState(windowState() & ~Qt::WindowMinimized);
	raise();
	activateWindow();
}

void MainWindow::quit()
{
	if (m_recorder.isRecording() &&
	    QMessageBox::question(this, tr("HD60 Pro"), tr("A recording is running. Stop it and quit?")) != QMessageBox::Yes)
		return;
	m_quitting = true;
	m_settings.save();
	qApp->quit();	// the Recorder's destructor finishes the file
}

void MainWindow::closeEvent(QCloseEvent *e)
{
	if (m_quitting) {
		e->accept();
		return;
	}
	// Keep working in the tray while there is something to keep alive.
	if (m_tray && (m_listen->isChecked() || m_recorder.isRecording())) {
		hide();
		m_tray->showMessage(tr("HD60 Pro is still running"),
				    m_recorder.isRecording() ? tr("Recording continues. Use the tray icon to open or quit.")
							     : tr("Listening continues. Use the tray icon to open or quit."),
				    QSystemTrayIcon::Information, 4000);
		e->ignore();
		return;
	}
	if (m_recorder.isRecording()) {	// no tray to hide in
		if (QMessageBox::question(this, tr("HD60 Pro"), tr("A recording is running. Stop it and quit?")) != QMessageBox::Yes) {
			e->ignore();
			return;
		}
	}
	m_quitting = true;
	m_settings.save();
	e->accept();
	qApp->quit();
}

void MainWindow::showEvent(QShowEvent *e)
{
	QMainWindow::showEvent(e);
	updatePreview();
}

void MainWindow::hideEvent(QHideEvent *e)
{
	QMainWindow::hideEvent(e);
	updatePreview();	// no preview decoding while nobody is looking
}

void MainWindow::onDriver(const DriverState &s)
{
	const DriverState old = m_state;
	m_state = s;

	// A recording cannot outlive the stream it reads.
	if (m_recorder.isRecording() && (s.kind != DriverState::Streaming || !s.sameStream(old))) {
		m_recorder.stop(s.kind == DriverState::Streaming ? tr("the input format changed")
				: s.kind == DriverState::Stopped || s.kind == DriverState::NotInstalled ? tr("the driver stopped")
				: tr("input signal lost"));
	}
	// The audio source exists while the driver runs.
	m_listener.setSourceAvailable(s.kind == DriverState::NoSignal || s.kind == DriverState::Streaming ||
				      s.kind == DriverState::Unsupported);

	QString text;
	bool canStart = false;
	switch (s.kind) {
	case DriverState::NotInstalled:
		text = s.problem;
		break;
	case DriverState::Stopped:
		text = s.problem.isEmpty() ? tr("The driver is not running.") : s.problem;
		canStart = true;
		break;
	case DriverState::NoSignal:
		text = tr("No signal\n\nConnect a console or other HDMI source to the card's HDMI IN and switch it on.");
		break;
	case DriverState::Unsupported:
		text = tr("%1 cannot be captured (the picture is too large for the card).\nHDMI OUT still works.").arg(s.modeText());
		break;
	case DriverState::Streaming:
		text = m_settings.previewOn ? tr("Starting the preview…") : tr("Preview is off (Settings).\n%1").arg(s.modeText());
		break;
	}
	m_message->setText(text);
	m_startDriver->setVisible(canStart);
	updatePreview();
	updateRecordUi();
	updateStatusLine();
}

void MainWindow::stopPreview()
{
	m_previewRetry.stop();
	if (m_camera) {
		m_camera->stop();
		m_session->setCamera(nullptr);
		delete m_camera;
		m_camera = nullptr;
	}
	m_stack->setCurrentWidget(m_messagePage);
}

// The preview runs only while the driver is streaming and the window is visible, and
// it is released as soon as the stream changes so the driver can set a new format.
void MainWindow::updatePreview()
{
	const bool want = m_state.kind == DriverState::Streaming && m_settings.previewOn && isVisible() && !isMinimized();
	if (!want) {
		stopPreview();
		return;
	}
	if (m_camera && m_previewState.sameStream(m_state))
		return;
	stopPreview();

	QCameraDevice found;
	const QByteArray wantId = m_state.videoDevice.toUtf8();
	for (const QCameraDevice &d : QMediaDevices::videoInputs()) {
		if (d.id() == wantId || d.description() == QLatin1String("HD60 Pro")) {
			found = d;
			break;
		}
	}
	if (found.isNull()) {	// the loopback shows up as a camera a moment after the driver starts feeding it
		m_previewRetry.start();
		return;
	}
	m_camera = new QCamera(found, this);
	connect(m_camera, &QCamera::errorOccurred, this, [this](QCamera::Error err, const QString &msg) {
		if (err == QCamera::NoError)
			return;
		m_message->setText(tr("The preview could not be shown:\n%1\n\nRecording and listening still work.").arg(msg));
		QTimer::singleShot(0, this, [this] {
			stopPreview();
			m_previewRetry.start();
		});
	});
	m_session->setCamera(m_camera);
	m_previewState = m_state;
	m_camera->start();
	m_stack->setCurrentWidget(m_video);
}

void MainWindow::toggleRecord()
{
	if (m_recorder.isRecording()) {
		m_recorder.stop();
		m_record->setEnabled(false);	// until ffmpeg has finished the file
		return;
	}
	QApplication::setOverrideCursor(Qt::WaitCursor);	// the first start tests the encoders
	QString err;
	const bool ok = m_recorder.start(m_state, m_settings, &err);
	QApplication::restoreOverrideCursor();
	if (!ok) {
		if (isVisible())
			QMessageBox::warning(this, tr("Recording"), err);
		else if (m_tray)
			m_tray->showMessage(tr("HD60 Pro recording"), err, QSystemTrayIcon::Warning, 10000);
	}
	updateRecordUi();
}

void MainWindow::updateRecordUi()
{
	const bool rec = m_recorder.isRecording();
	m_record->setText(rec ? tr("■  Stop") : tr("●  Record"));
	m_record->setStyleSheet(rec ? QStringLiteral("QPushButton { font-weight: bold; color: #e5484d; }") : QString());
	if (!rec)
		m_record->setEnabled(m_state.kind == DriverState::Streaming);
	if (m_trayRecord) {
		m_trayRecord->setText(rec ? tr("Stop recording") : tr("Record"));
		m_trayRecord->setEnabled(rec || m_state.kind == DriverState::Streaming);
	}
	if (rec) {
		const qint64 s = m_recorder.elapsedMs() / 1000;
		m_timer->setText(QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
					 .arg(s % 60, 2, 10, QLatin1Char('0')));
	} else {
		m_timer->clear();
	}
	if (m_tray)
		m_tray->setToolTip(rec ? tr("HD60 Pro: recording") : tr("HD60 Pro"));
}

void MainWindow::setListen(bool on)
{
	m_settings.listenOn = on;
	m_settings.save();
	if (m_trayListen)
		m_trayListen->setChecked(on);
	m_listener.setWanted(on);
	updateStatusLine();
}

void MainWindow::openSettings()
{
	SettingsDialog dlg(m_settings, this);
	if (dlg.exec() != QDialog::Accepted)
		return;
	const AppSettings chosen = dlg.result();
	const bool listenOn = m_settings.listenOn;
	const int volume = m_settings.listenVolume;
	m_settings = chosen;
	m_settings.listenOn = listenOn;
	m_settings.listenVolume = volume;
	m_settings.save();
	m_listener.setSink(m_settings.listenSink);
	onDriver(m_state);	// preview on/off
}

void MainWindow::updateStatusLine()
{
	QStringList parts;
	switch (m_state.kind) {
	case DriverState::Streaming:
		parts << tr("Input %1").arg(m_state.modeText());
		if (m_state.audioRate)
			parts << tr("audio %1 kHz").arg(m_state.audioRate / 1000.0, 0, 'g', 3);
		if (m_state.measuredFps > 0)
			parts << tr("%1 fps").arg(m_state.measuredFps, 0, 'f', 1);
		break;
	case DriverState::NoSignal:
		parts << tr("No signal");
		break;
	case DriverState::Unsupported:
		parts << tr("Input %1 (not capturable)").arg(m_state.modeText());
		break;
	case DriverState::Stopped:
		parts << tr("Driver stopped");
		break;
	case DriverState::NotInstalled:
		parts << tr("Driver not installed");
		break;
	}
	if (!m_state.hdmiOut.isEmpty())
		parts << tr("HDMI OUT: %1").arg(m_state.hdmiOut);
	if (m_listen->isChecked())
		parts << (m_listener.isPlaying() ? tr("listening") : tr("listening (waiting for the driver)"));
	if (m_recorder.isRecording())
		parts << tr("recording to %1").arg(m_recorder.file());
	else if (!m_notice.isEmpty())
		parts << m_notice;
	m_status->setText(parts.join(QStringLiteral("   ·   ")));
}
