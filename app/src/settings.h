// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QString>

// Recording quality. Custom uses the custom* fields; the others are fixed.
enum class Preset { Best, Balanced, Small, Custom };

struct RecordSettings {
	Preset preset = Preset::Balanced;
	QString customCodec = QStringLiteral("h264");	// "h264" or "hevc"
	int customVideoKbps = 20000;
	int customAudioKbps = 192;
	int customHeight = 0;				// 0 = same as the input
	QString container = QStringLiteral("mp4");	// "mp4" or "mkv"
	QString folder;					// empty = ~/Videos/HD60 Pro
};

struct AppSettings {
	RecordSettings rec;
	bool previewOn = true;
	bool listenOn = false;		// restored when the app starts
	int listenVolume = 100;		// percent
	QString listenSink;		// PipeWire sink name, empty = default output

	static AppSettings load();
	void save() const;
	QString recordFolder() const;	// rec.folder or the default
};

// "Open at login in the tray": an XDG autostart entry for the app.
bool appAutostartEnabled();
bool setAppAutostart(bool on, QString *error);
// "Start the driver at login": the hd60pro.service user unit.
bool driverAutostartEnabled();
bool setDriverAutostart(bool on, QString *error);
