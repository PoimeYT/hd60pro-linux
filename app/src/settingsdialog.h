// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDialog>

#include "settings.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QWidget;

class SettingsDialog : public QDialog {
	Q_OBJECT
public:
	SettingsDialog(const AppSettings &current, QWidget *parent = nullptr);
	AppSettings result() const;	// valid after accept()
	void accept() override;
private:
	void updateCustomEnabled();
	AppSettings m_s;
	QComboBox *m_preset, *m_codec, *m_height, *m_container, *m_sink;
	QSpinBox *m_videoMbps, *m_audioKbps;
	QLineEdit *m_folder;
	QCheckBox *m_preview, *m_appAutostart, *m_driverAutostart;
	QWidget *m_custom;
};
