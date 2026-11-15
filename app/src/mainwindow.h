// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>

#include "driverstatus.h"
#include "listener.h"
#include "recorder.h"
#include "settings.h"

class QAction;
class QCamera;
class QLabel;
class QMediaCaptureSession;
class QPushButton;
class QSlider;
class QStackedWidget;
class QVideoWidget;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow() override;
	void bringToFront();
protected:
	void closeEvent(QCloseEvent *e) override;
	void showEvent(QShowEvent *e) override;
	void hideEvent(QHideEvent *e) override;
private:
	void onDriver(const DriverState &s);
	void updatePreview();
	void stopPreview();
	void toggleRecord();
	void setListen(bool on);
	void openSettings();
	void updateStatusLine();
	void updateRecordUi();
	void quit();

	AppSettings m_settings;
	DriverStatus m_driver;
	Recorder m_recorder;
	Listener m_listener;
	DriverState m_state, m_previewState;

	QStackedWidget *m_stack;
	QVideoWidget *m_video;
	QWidget *m_messagePage;
	QLabel *m_message, *m_status, *m_timer;
	QPushButton *m_startDriver, *m_record, *m_listen, *m_settingsBtn;
	QSlider *m_volume;
	QCamera *m_camera = nullptr;
	QMediaCaptureSession *m_session;
	QTimer m_tick, m_previewRetry;
	QSystemTrayIcon *m_tray = nullptr;
	QAction *m_trayListen = nullptr, *m_trayRecord = nullptr;
	QString m_notice;	// last recording result, shown in the status line
	bool m_quitting = false;
};
