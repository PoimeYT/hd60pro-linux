// SPDX-License-Identifier: GPL-2.0-or-later
#include "settingsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include "listener.h"

SettingsDialog::SettingsDialog(const AppSettings &current, QWidget *parent) : QDialog(parent), m_s(current)
{
	setWindowTitle(tr("HD60 Pro settings"));
	auto *top = new QVBoxLayout(this);

	auto *recBox = new QGroupBox(tr("Recording"));
	auto *rec = new QFormLayout(recBox);
	m_preset = new QComboBox;
	m_preset->addItem(tr("Best quality (40 Mbit/s)"), int(Preset::Best));
	m_preset->addItem(tr("Balanced (15 Mbit/s)"), int(Preset::Balanced));
	m_preset->addItem(tr("Small for sharing (6 Mbit/s, 1080p at most)"), int(Preset::Small));
	m_preset->addItem(tr("Custom"), int(Preset::Custom));
	m_preset->setCurrentIndex(m_preset->findData(int(m_s.rec.preset)));
	rec->addRow(tr("Quality"), m_preset);

	m_custom = new QWidget;
	auto *cust = new QFormLayout(m_custom);
	cust->setContentsMargins(0, 0, 0, 0);
	m_codec = new QComboBox;
	m_codec->addItem(tr("H.264 (plays everywhere)"), QStringLiteral("h264"));
	m_codec->addItem(tr("HEVC / H.265 (smaller files)"), QStringLiteral("hevc"));
	m_codec->setCurrentIndex(m_codec->findData(m_s.rec.customCodec));
	cust->addRow(tr("Video format"), m_codec);
	m_videoMbps = new QSpinBox;
	m_videoMbps->setRange(1, 150);
	m_videoMbps->setSuffix(tr(" Mbit/s"));
	m_videoMbps->setValue(qMax(1, m_s.rec.customVideoKbps / 1000));
	cust->addRow(tr("Video bitrate"), m_videoMbps);
	m_height = new QComboBox;
	m_height->addItem(tr("Same as the input"), 0);
	m_height->addItem(tr("1080p"), 1080);
	m_height->addItem(tr("720p"), 720);
	m_height->setCurrentIndex(qMax(0, m_height->findData(m_s.rec.customHeight)));
	cust->addRow(tr("Resolution"), m_height);
	m_audioKbps = new QSpinBox;
	m_audioKbps->setRange(64, 512);
	m_audioKbps->setSingleStep(32);
	m_audioKbps->setSuffix(tr(" kbit/s"));
	m_audioKbps->setValue(m_s.rec.customAudioKbps);
	cust->addRow(tr("Audio bitrate"), m_audioKbps);
	rec->addRow(m_custom);

	m_container = new QComboBox;
	m_container->addItem(tr("MP4"), QStringLiteral("mp4"));
	m_container->addItem(tr("MKV"), QStringLiteral("mkv"));
	m_container->setCurrentIndex(m_container->findData(m_s.rec.container));
	rec->addRow(tr("File type"), m_container);

	auto *folderRow = new QHBoxLayout;
	m_folder = new QLineEdit(m_s.recordFolder());
	auto *browse = new QPushButton(tr("Choose…"));
	folderRow->addWidget(m_folder);
	folderRow->addWidget(browse);
	rec->addRow(tr("Save to"), folderRow);
	connect(browse, &QPushButton::clicked, this, [this] {
		const QString d = QFileDialog::getExistingDirectory(this, tr("Recording folder"), m_folder->text());
		if (!d.isEmpty())
			m_folder->setText(d);
	});
	top->addWidget(recBox);

	auto *listenBox = new QGroupBox(tr("Listening"));
	auto *listen = new QFormLayout(listenBox);
	m_sink = new QComboBox;
	m_sink->addItem(tr("Default output"), QString());
	for (const AudioSink &s : listSinks())
		m_sink->addItem(s.description, s.name);
	int idx = m_sink->findData(m_s.listenSink);
	if (idx < 0 && !m_s.listenSink.isEmpty()) {	// the chosen device is not connected right now
		m_sink->addItem(tr("%1 (not connected)").arg(m_s.listenSink), m_s.listenSink);
		idx = m_sink->count() - 1;
	}
	m_sink->setCurrentIndex(qMax(0, idx));
	listen->addRow(tr("Play game sound on"), m_sink);
	top->addWidget(listenBox);

	auto *generalBox = new QGroupBox(tr("General"));
	auto *general = new QVBoxLayout(generalBox);
	m_preview = new QCheckBox(tr("Show the live preview"));
	m_preview->setChecked(m_s.previewOn);
	m_appAutostart = new QCheckBox(tr("Open HD60 Pro in the tray when I log in (Listen stays as I left it)"));
	m_appAutostart->setChecked(appAutostartEnabled());
	m_driverAutostart = new QCheckBox(tr("Start the driver when I log in (needed for HDMI passthrough)"));
	m_driverAutostart->setChecked(driverAutostartEnabled());
	general->addWidget(m_preview);
	general->addWidget(m_appAutostart);
	general->addWidget(m_driverAutostart);
	top->addWidget(generalBox);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &SettingsDialog::reject);
	top->addWidget(buttons);

	connect(m_preset, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateCustomEnabled);
	updateCustomEnabled();
}

void SettingsDialog::updateCustomEnabled()
{
	m_custom->setVisible(Preset(m_preset->currentData().toInt()) == Preset::Custom);
	adjustSize();
}

void SettingsDialog::accept()
{
	m_s.rec.preset = Preset(m_preset->currentData().toInt());
	m_s.rec.customCodec = m_codec->currentData().toString();
	m_s.rec.customVideoKbps = m_videoMbps->value() * 1000;
	m_s.rec.customAudioKbps = m_audioKbps->value();
	m_s.rec.customHeight = m_height->currentData().toInt();
	m_s.rec.container = m_container->currentData().toString();
	const QString folder = m_folder->text().trimmed();
	const QString def = AppSettings().recordFolder();
	m_s.rec.folder = (folder.isEmpty() || folder == def) ? QString() : folder;
	m_s.listenSink = m_sink->currentData().toString();
	m_s.previewOn = m_preview->isChecked();

	QString err;
	if (m_appAutostart->isChecked() != appAutostartEnabled() && !setAppAutostart(m_appAutostart->isChecked(), &err)) {
		QMessageBox::warning(this, windowTitle(), err);
		return;
	}
	if (m_driverAutostart->isChecked() != driverAutostartEnabled() &&
	    !setDriverAutostart(m_driverAutostart->isChecked(), &err)) {
		QMessageBox::warning(this, windowTitle(), tr("Could not change the driver's start at login:\n%1").arg(err));
		return;
	}
	QDialog::accept();
}

AppSettings SettingsDialog::result() const
{
	return m_s;
}
