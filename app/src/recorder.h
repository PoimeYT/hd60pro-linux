// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDateTime>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>

#include "driverstatus.h"
#include "settings.h"

// One way of encoding video with ffmpeg.
struct Encoder {
	QString name;		// ffmpeg encoder, e.g. "h264_nvenc"
	QString label;		// for people, e.g. "NVIDIA (NVENC)"
	enum Kind { Nvenc, Vaapi, Software } kind = Software;
};

// Candidates for a codec ("h264" or "hevc") in the order they are preferred.
QList<Encoder> encoderCandidates(const QString &codec);

struct RecordPlan {
	QString codec;
	int videoKbps = 0, audioKbps = 0;
	int outHeight = 0;	// 0 = keep the input size
};
// Resolves a preset against the input mode.
RecordPlan planFor(const RecordSettings &rec, const DriverState &in);

// Full ffmpeg argument list for a recording of the driver's stream.
QStringList buildFfmpegArgs(const DriverState &in, const RecordPlan &plan, const Encoder &enc, const QString &container,
			    const QString &outFile);

QString recordingFileName(const QDateTime &when, const QString &container);

class Recorder : public QObject {
	Q_OBJECT
public:
	explicit Recorder(QObject *parent = nullptr);
	~Recorder() override;
	bool isRecording() const { return m_proc.state() != QProcess::NotRunning; }
	qint64 elapsedMs() const;
	QString file() const { return m_file; }
	// Starts recording. On failure returns false with a message for the user.
	bool start(const DriverState &in, const AppSettings &settings, QString *error);
	// Stops and finishes the file. reason is shown to the user when not empty.
	void stop(const QString &reason = QString());
signals:
	void started(const QString &file);
	// ok: the file is complete. message is ready to show.
	void finished(bool ok, const QString &message, const QString &file);
private:
	bool pickEncoder(const QString &codec, Encoder *out, QString *error);
	void onFinished(int code, QProcess::ExitStatus st);
	QProcess m_proc;
	QTimer m_killTimer;
	QString m_file, m_stopReason, m_log;
	bool m_stopping = false;
	qint64 m_startMs = 0;
	QHash<QString, Encoder> m_encoderCache;
};
