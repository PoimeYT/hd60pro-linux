// SPDX-License-Identifier: GPL-2.0-or-later
#include "listener.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <signal.h>
#include <sys/prctl.h>

static const char kNodeName[] = "hd60pro-listen";

QList<AudioSink> parseSinks(const QByteArray &json)
{
	QList<AudioSink> out;
	const QJsonArray arr = QJsonDocument::fromJson(json).array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		const QString name = o.value(QLatin1String("name")).toString();
		if (name.isEmpty())
			continue;
		QString desc = o.value(QLatin1String("description")).toString();
		out.append({ name, desc.isEmpty() ? name : desc });
	}
	return out;
}

int parseListenInput(const QByteArray &json)
{
	const QJsonArray arr = QJsonDocument::fromJson(json).array();
	for (const QJsonValue &v : arr) {
		const QJsonObject o = v.toObject();
		const QString node = o.value(QLatin1String("properties")).toObject().value(QLatin1String("node.name")).toString();
		if (node == QLatin1String("output.") + QLatin1String(kNodeName) || node == QLatin1String(kNodeName))
			return o.value(QLatin1String("index")).toInt(-1);
	}
	return -1;
}

static QByteArray pactl(const QStringList &args)
{
	QProcess p;
	p.start(QStringLiteral("pactl"), args);
	if (!p.waitForFinished(3000)) {
		p.kill();
		p.waitForFinished(500);
		return {};
	}
	return p.exitCode() == 0 ? p.readAllStandardOutput() : QByteArray();
}

QList<AudioSink> listSinks()
{
	return parseSinks(pactl({ QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"), QStringLiteral("sinks") }));
}

Listener::Listener(QObject *parent) : QObject(parent)
{
	m_proc.setProcessChannelMode(QProcess::MergedChannels);
	// Never leave game sound playing with no app (and no Listen button) to stop it.
	m_proc.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
	m_restart.setSingleShot(true);
	connect(&m_restart, &QTimer::timeout, this, &Listener::update);
	connect(&m_volTimer, &QTimer::timeout, this, &Listener::applyVolume);
	connect(&m_proc, &QProcess::started, this, [this] {
		m_inputIndex = -1;
		m_volTries = 0;
		m_volTimer.start(250);	// the stream shows up a moment after the process starts
		emit playingChanged(true);
	});
	connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
		if (e == QProcess::FailedToStart) {
			m_wanted = false;
			emit failed(tr("Could not start pw-loopback. Is PipeWire installed?"));
			emit playingChanged(false);
		}
	});
	connect(&m_proc, &QProcess::finished, this, [this] {
		m_volTimer.stop();
		m_inputIndex = -1;
		emit playingChanged(false);
		if (m_expectExit) {
			m_expectExit = false;
			update();	// e.g. restart with another output
			return;
		}
		if (!m_wanted || !m_source)
			return;
		// It stopped by itself (output unplugged, PipeWire restarted): try again, but not forever.
		const qint64 now = QDateTime::currentMSecsSinceEpoch();
		m_exits.append(now);
		while (!m_exits.isEmpty() && now - m_exits.first() > 60000)
			m_exits.removeFirst();
		if (m_exits.size() > 5) {
			m_wanted = false;
			m_exits.clear();
			emit failed(tr("Listening keeps stopping:\n%1").arg(QString::fromUtf8(m_proc.readAll()).trimmed().right(400)));
			return;
		}
		m_restart.start(1000);
	});
}

Listener::~Listener()
{
	m_proc.disconnect(this);
	if (m_proc.state() != QProcess::NotRunning) {
		m_proc.terminate();
		if (!m_proc.waitForFinished(2000))
			m_proc.kill(), m_proc.waitForFinished(1000);
	}
}

void Listener::setWanted(bool on)
{
	m_wanted = on;
	m_exits.clear();
	update();
}

void Listener::setSourceAvailable(bool on)
{
	if (m_source == on)
		return;
	m_source = on;
	update();
}

void Listener::setSink(const QString &name)
{
	if (m_sink == name)
		return;
	m_sink = name;
	if (m_proc.state() != QProcess::NotRunning) {
		m_expectExit = true;
		m_proc.terminate();
	}
}

void Listener::update()
{
	const bool shouldRun = m_wanted && m_source;
	const bool running = m_proc.state() != QProcess::NotRunning;
	if (shouldRun && !running) {
		QStringList a{ QStringLiteral("-n"), QLatin1String(kNodeName), QStringLiteral("-C"), QStringLiteral("hd60pro"),
			       QStringLiteral("-l"), QStringLiteral("20"),
			       // Only ever capture from the card: if its source goes away, stop instead of
			       // being moved to the default input (which would play the microphone).
			       QStringLiteral("-i"), QStringLiteral("node.dont-reconnect=true node.dont-fallback=true"),
			       QStringLiteral("-o"), QStringLiteral("media.role=Game node.description=\"HD60 Pro\"") };
		if (!m_sink.isEmpty())
			a << QStringLiteral("-P") << m_sink;
		m_proc.start(QStringLiteral("pw-loopback"), a);
	} else if (!shouldRun && running && !m_expectExit) {
		m_expectExit = true;
		m_proc.terminate();
	}
}

void Listener::setVolume(int percent)
{
	m_volume = qBound(0, percent, 150);
	if (m_proc.state() == QProcess::Running && !m_volTimer.isActive())
		applyVolume();
}

void Listener::applyVolume()
{
	if (m_proc.state() != QProcess::Running) {
		m_volTimer.stop();
		return;
	}
	if (m_inputIndex < 0)
		m_inputIndex = parseListenInput(pactl({ QStringLiteral("-f"), QStringLiteral("json"), QStringLiteral("list"),
							QStringLiteral("sink-inputs") }));
	if (m_inputIndex < 0) {
		if (++m_volTries > 20)
			m_volTimer.stop();
		return;
	}
	m_volTimer.stop();
	QProcess p;
	p.start(QStringLiteral("pactl"), { QStringLiteral("set-sink-input-volume"), QString::number(m_inputIndex),
					   QStringLiteral("%1%").arg(m_volume) });
	if (!p.waitForFinished(3000) || p.exitCode() != 0)
		m_inputIndex = -1;	// stale index: look it up again next time
}
