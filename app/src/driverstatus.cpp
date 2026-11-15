// SPDX-License-Identifier: GPL-2.0-or-later
#include "driverstatus.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <signal.h>

bool DriverState::operator==(const DriverState &o) const
{
	return kind == o.kind && sameStream(o) && qRound(measuredFps) == qRound(o.measuredFps) && hdmiOut == o.hdmiOut &&
	       problem == o.problem;
}

bool DriverState::sameStream(const DriverState &o) const
{
	return width == o.width && height == o.height && fps == o.fps && interlaced == o.interlaced && ntsc == o.ntsc &&
	       audioRate == o.audioRate && videoDevice == o.videoDevice;
}

QString DriverState::modeText() const
{
	if (!width)
		return QString();
	QString rate = ntsc ? QString::number(fps * 1000.0 / 1001.0, 'f', 2) : QString::number(fps);
	return QStringLiteral("%1x%2%3%4").arg(width).arg(height).arg(interlaced ? QLatin1Char('i') : QLatin1Char('p')).arg(rate);
}

bool parseDriverStatus(const QByteArray &json, DriverState *out)
{
	QJsonParseError err;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
	if (err.error != QJsonParseError::NoError || !doc.isObject())
		return false;
	const QJsonObject o = doc.object();
	const QString state = o.value(QLatin1String("state")).toString();
	DriverState s;
	if (state == QLatin1String("streaming"))
		s.kind = DriverState::Streaming;
	else if (state == QLatin1String("no_signal"))
		s.kind = DriverState::NoSignal;
	else if (state == QLatin1String("unsupported"))
		s.kind = DriverState::Unsupported;
	else
		return false;
	s.width = qBound(0, o.value(QLatin1String("width")).toInt(), 8192);
	s.height = qBound(0, o.value(QLatin1String("height")).toInt(), 8192);
	s.fps = qBound(0, o.value(QLatin1String("fps")).toInt(), 240);
	s.audioRate = o.value(QLatin1String("audio_rate")).toInt();
	s.interlaced = o.value(QLatin1String("interlaced")).toBool();
	s.ntsc = o.value(QLatin1String("ntsc")).toBool();
	s.measuredFps = o.value(QLatin1String("measured_fps")).toDouble();
	s.hdmiOut = o.value(QLatin1String("hdmi_out")).toString();
	const QString dev = o.value(QLatin1String("video_device")).toString();
	if (dev.startsWith(QLatin1String("/dev/")))
		s.videoDevice = dev;
	if (s.kind == DriverState::Streaming && (!s.width || !s.height || !s.fps))
		return false;
	*out = s;
	return true;
}

DriverStatus::DriverStatus(QObject *parent) : QObject(parent)
{
	connect(&m_timer, &QTimer::timeout, this, &DriverStatus::refresh);
	m_timer.start(500);
}

QString DriverStatus::statusPath() const
{
	const QByteArray test = qgetenv("HD60_STATUS_FILE");	// tests point the app at a prepared file
	if (!test.isEmpty())
		return QString::fromLocal8Bit(test);
	return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/hd60pro.status");
}

static QString runOut(const QString &prog, const QStringList &args, int timeoutMs = 4000)
{
	QProcess p;
	p.start(prog, args);
	if (!p.waitForFinished(timeoutMs)) {
		p.kill();
		p.waitForFinished(500);
		return QString();
	}
	return QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
}

void DriverStatus::refresh()
{
	DriverState s;
	bool live = false;
	QFile f(statusPath());
	if (f.open(QIODevice::ReadOnly)) {
		const QByteArray data = f.readAll();
		// A crashed driver leaves its file behind: only trust it while that process is alive.
		const qint64 pid = QJsonDocument::fromJson(data).object().value(QLatin1String("pid")).toInteger();
		live = parseDriverStatus(data, &s) && pid > 1 && ::kill(pid_t(pid), 0) == 0;
	}
	if (!live) {
		// systemd is only asked every few seconds: it is a process spawn.
		if (m_first || m_tick++ % 6 == 0) {
			m_unitState = runOut(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("show"),
					     QStringLiteral("-p"), QStringLiteral("LoadState"), QStringLiteral("-p"),
					     QStringLiteral("ActiveState"), QStringLiteral("hd60pro.service")});
		}
		s = DriverState();
		if (m_unitState.contains(QLatin1String("LoadState=not-found")) || m_unitState.isEmpty()) {
			s.kind = DriverState::NotInstalled;
			s.problem = tr("The driver is not installed. Run system/install.sh from the project folder.");
		} else if (m_unitState.contains(QLatin1String("ActiveState=active")) ||
			   m_unitState.contains(QLatin1String("ActiveState=activating"))) {
			s.kind = DriverState::NoSignal;	// running, but an older driver without a status file
			s.problem = tr("The driver is starting.");
		} else {
			s.kind = DriverState::Stopped;
			s.problem = m_state.kind == DriverState::Stopped ? m_state.problem : QString();
		}
	} else {
		m_tick = 0;
	}
	if (m_first || s != m_state) {
		m_first = false;
		m_state = s;
		emit changed(m_state);
	}
}

void DriverStatus::startDriver()
{
	QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("reset-failed"), QStringLiteral("hd60pro.service")});
	QProcess::execute(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("start"), QStringLiteral("hd60pro.service")});
	m_first = true;
	refresh();
	if (m_state.kind == DriverState::Stopped) {
		// The unit skips itself when the system is not ready; "hd60pro check" says why on stderr.
		QProcess p;
		p.start(QStringLiteral("hd60pro"), {QStringLiteral("check")});
		QString why;
		if (p.waitForFinished(5000))
			why = QString::fromUtf8(p.readAllStandardError()).trimmed();
		else
			p.kill(), p.waitForFinished(500);
		m_state.problem = why.isEmpty() ? tr("The driver did not start. See: journalctl --user -u hd60pro -b") : why;
		emit changed(m_state);
	}
}
