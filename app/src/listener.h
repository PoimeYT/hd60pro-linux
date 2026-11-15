// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QProcess>
#include <QTimer>

struct AudioSink {
	QString name, description;
};
// Parses `pactl -f json list sinks`.
QList<AudioSink> parseSinks(const QByteArray &json);
// Finds the sink-input index of our loopback in `pactl -f json list sink-inputs`, -1 if absent.
int parseListenInput(const QByteArray &json);
QList<AudioSink> listSinks();

// Plays the "hd60pro" source into an output with pw-loopback (headphone listening).
class Listener : public QObject {
	Q_OBJECT
public:
	explicit Listener(QObject *parent = nullptr);
	~Listener() override;
	// wanted: the user switched Listen on. Audio only plays while the source exists.
	void setWanted(bool on);
	void setSourceAvailable(bool on);
	void setVolume(int percent);
	void setSink(const QString &name);
	bool isWanted() const { return m_wanted; }
	bool isPlaying() const { return m_proc.state() == QProcess::Running; }
signals:
	void playingChanged(bool playing);
	void failed(const QString &message);
private:
	void update();
	void applyVolume();
	QProcess m_proc;
	QTimer m_restart, m_volTimer;
	bool m_wanted = false, m_source = false, m_expectExit = false;
	int m_volume = 100, m_volTries = 0, m_inputIndex = -1;
	QString m_sink;
	QList<qint64> m_exits;	// unexpected exits, for the restart limit
};
