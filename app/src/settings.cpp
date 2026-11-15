// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

static Preset presetFromString(const QString &s)
{
	if (s == QLatin1String("best"))
		return Preset::Best;
	if (s == QLatin1String("small"))
		return Preset::Small;
	if (s == QLatin1String("custom"))
		return Preset::Custom;
	return Preset::Balanced;
}

static QString presetToString(Preset p)
{
	switch (p) {
	case Preset::Best: return QStringLiteral("best");
	case Preset::Small: return QStringLiteral("small");
	case Preset::Custom: return QStringLiteral("custom");
	default: return QStringLiteral("balanced");
	}
}

AppSettings AppSettings::load()
{
	QSettings q;
	AppSettings s;
	s.rec.preset = presetFromString(q.value(QStringLiteral("record/preset")).toString());
	s.rec.customCodec = q.value(QStringLiteral("record/codec"), s.rec.customCodec).toString() == QLatin1String("hevc")
				    ? QStringLiteral("hevc") : QStringLiteral("h264");
	s.rec.customVideoKbps = qBound(500, q.value(QStringLiteral("record/videoKbps"), s.rec.customVideoKbps).toInt(), 150000);
	s.rec.customAudioKbps = qBound(64, q.value(QStringLiteral("record/audioKbps"), s.rec.customAudioKbps).toInt(), 512);
	s.rec.customHeight = q.value(QStringLiteral("record/height"), 0).toInt();
	if (s.rec.customHeight != 720 && s.rec.customHeight != 1080)
		s.rec.customHeight = 0;
	s.rec.container = q.value(QStringLiteral("record/container")).toString() == QLatin1String("mkv")
				  ? QStringLiteral("mkv") : QStringLiteral("mp4");
	s.rec.folder = q.value(QStringLiteral("record/folder")).toString();
	s.previewOn = q.value(QStringLiteral("preview/on"), true).toBool();
	s.listenOn = q.value(QStringLiteral("listen/on"), false).toBool();
	s.listenVolume = qBound(0, q.value(QStringLiteral("listen/volume"), 100).toInt(), 150);
	s.listenSink = q.value(QStringLiteral("listen/sink")).toString();
	return s;
}

void AppSettings::save() const
{
	QSettings q;
	q.setValue(QStringLiteral("record/preset"), presetToString(rec.preset));
	q.setValue(QStringLiteral("record/codec"), rec.customCodec);
	q.setValue(QStringLiteral("record/videoKbps"), rec.customVideoKbps);
	q.setValue(QStringLiteral("record/audioKbps"), rec.customAudioKbps);
	q.setValue(QStringLiteral("record/height"), rec.customHeight);
	q.setValue(QStringLiteral("record/container"), rec.container);
	q.setValue(QStringLiteral("record/folder"), rec.folder);
	q.setValue(QStringLiteral("preview/on"), previewOn);
	q.setValue(QStringLiteral("listen/on"), listenOn);
	q.setValue(QStringLiteral("listen/volume"), listenVolume);
	q.setValue(QStringLiteral("listen/sink"), listenSink);
}

QString AppSettings::recordFolder() const
{
	if (!rec.folder.isEmpty())
		return rec.folder;
	QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
	if (videos.isEmpty())
		videos = QDir::homePath() + QStringLiteral("/Videos");
	return videos + QStringLiteral("/HD60 Pro");
}

static QString autostartPath()
{
	return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/hd60pro-app.desktop");
}

bool appAutostartEnabled()
{
	return QFile::exists(autostartPath());
}

bool setAppAutostart(bool on, QString *error)
{
	const QString path = autostartPath();
	if (!on) {
		if (QFile::exists(path) && !QFile::remove(path)) {
			*error = QStringLiteral("Could not remove %1").arg(path);
			return false;
		}
		return true;
	}
	QDir().mkpath(QFileInfo(path).absolutePath());
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		*error = QStringLiteral("Could not write %1: %2").arg(path, f.errorString());
		return false;
	}
	// Desktop-entry Exec quoting: the path in double quotes, with the reserved characters escaped.
	QString exe = QCoreApplication::applicationFilePath();
	exe.replace(QLatin1Char('\\'), QStringLiteral("\\\\")).replace(QLatin1Char('"'), QStringLiteral("\\\""))
		.replace(QLatin1Char('$'), QStringLiteral("\\$")).replace(QLatin1Char('`'), QStringLiteral("\\`"));
	f.write(QStringLiteral("[Desktop Entry]\nType=Application\nName=HD60 Pro\nExec=\"%1\" --tray\nIcon=hd60pro\n"
			       "X-GNOME-Autostart-enabled=true\n").arg(exe).toUtf8());
	return true;
}

static int systemctlUser(const QStringList &args, QString *out)
{
	QProcess p;
	p.start(QStringLiteral("systemctl"), QStringList{QStringLiteral("--user")} + args);
	if (!p.waitForFinished(10000)) {
		p.kill();
		p.waitForFinished(1000);
		if (out)
			*out = QStringLiteral("systemctl did not answer");
		return -1;
	}
	if (out)
		*out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
	return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1;
}

bool driverAutostartEnabled()
{
	QString out;
	systemctlUser({QStringLiteral("is-enabled"), QStringLiteral("hd60pro.service")}, &out);
	return out == QLatin1String("enabled");
}

bool setDriverAutostart(bool on, QString *error)
{
	return systemctlUser({on ? QStringLiteral("enable") : QStringLiteral("disable"), QStringLiteral("hd60pro.service")}, error) == 0;
}
