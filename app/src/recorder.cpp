// SPDX-License-Identifier: GPL-2.0-or-later
#include "recorder.h"

#include <QDir>
#include <QFileInfo>
#include <QStorageInfo>
#include <signal.h>
#include <sys/prctl.h>

QList<Encoder> encoderCandidates(const QString &codec)
{
	const bool hevc = codec == QLatin1String("hevc");
	return {
		{ hevc ? QStringLiteral("hevc_nvenc") : QStringLiteral("h264_nvenc"), QStringLiteral("NVIDIA (NVENC)"), Encoder::Nvenc },
		{ hevc ? QStringLiteral("hevc_vaapi") : QStringLiteral("h264_vaapi"), QStringLiteral("VAAPI"), Encoder::Vaapi },
		{ hevc ? QStringLiteral("libx265") : QStringLiteral("libx264"), QStringLiteral("software"), Encoder::Software },
	};
}

RecordPlan planFor(const RecordSettings &rec, const DriverState &in)
{
	RecordPlan p;
	p.codec = QStringLiteral("h264");
	switch (rec.preset) {
	case Preset::Best:
		p.videoKbps = 40000;
		p.audioKbps = 320;
		break;
	case Preset::Balanced:
		p.videoKbps = 15000;
		p.audioKbps = 192;
		break;
	case Preset::Small:
		p.videoKbps = 6000;
		p.audioKbps = 128;
		p.outHeight = 1080;
		break;
	case Preset::Custom:
		p.codec = rec.customCodec == QLatin1String("hevc") ? QStringLiteral("hevc") : QStringLiteral("h264");
		p.videoKbps = qBound(500, rec.customVideoKbps, 150000);
		p.audioKbps = qBound(64, rec.customAudioKbps, 512);
		p.outHeight = rec.customHeight;
		break;
	}
	// Never scale up, and do not scale to the size the input already has.
	if (p.outHeight >= in.height)
		p.outHeight = 0;
	return p;
}

static QString fpsArg(const DriverState &in)
{
	int fps = in.fps;
	if (in.interlaced)
		fps /= 2;	// the card delivers deinterlaced frames at the frame rate
	if (fps <= 0)
		fps = 60;
	return in.ntsc ? QStringLiteral("%1/1001").arg(fps * 1000) : QString::number(fps);
}

QStringList buildFfmpegArgs(const DriverState &in, const RecordPlan &plan, const Encoder &enc, const QString &container,
			    const QString &outFile)
{
	QStringList a{ QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("warning"),
		       QStringLiteral("-n") };	// never overwrite an existing file
	if (enc.kind == Encoder::Vaapi)
		a << QStringLiteral("-vaapi_device") << QStringLiteral("/dev/dri/renderD128");
	a << QStringLiteral("-f") << QStringLiteral("v4l2") << QStringLiteral("-thread_queue_size") << QStringLiteral("1024")
	  << QStringLiteral("-input_format") << QStringLiteral("yuv420p")
	  << QStringLiteral("-video_size") << QStringLiteral("%1x%2").arg(in.width).arg(in.height)
	  << QStringLiteral("-framerate") << fpsArg(in) << QStringLiteral("-i") << in.videoDevice;
	a << QStringLiteral("-f") << QStringLiteral("pulse") << QStringLiteral("-thread_queue_size") << QStringLiteral("1024")
	  << QStringLiteral("-sample_rate") << QStringLiteral("48000") << QStringLiteral("-channels") << QStringLiteral("2")
	  << QStringLiteral("-i") << QStringLiteral("hd60pro");

	QStringList vf;
	if (plan.outHeight > 0)
		vf << QStringLiteral("scale=-2:%1").arg(plan.outHeight);
	if (enc.kind == Encoder::Vaapi)
		vf << QStringLiteral("format=nv12") << QStringLiteral("hwupload");
	if (!vf.isEmpty())
		a << QStringLiteral("-vf") << vf.join(QLatin1Char(','));

	const QString rate = QStringLiteral("%1k").arg(plan.videoKbps);
	a << QStringLiteral("-c:v") << enc.name << QStringLiteral("-b:v") << rate << QStringLiteral("-maxrate") << rate
	  << QStringLiteral("-bufsize") << QStringLiteral("%1k").arg(plan.videoKbps * 2);
	int gop = (in.interlaced ? in.fps / 2 : in.fps) * 2;
	a << QStringLiteral("-g") << QString::number(gop > 0 ? gop : 120);
	switch (enc.kind) {
	case Encoder::Nvenc:
		a << QStringLiteral("-preset") << QStringLiteral("p5") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
		break;
	case Encoder::Software:
		a << QStringLiteral("-preset") << QStringLiteral("veryfast") << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
		break;
	case Encoder::Vaapi:
		break;
	}
	if (plan.codec == QLatin1String("hevc") && container == QLatin1String("mp4"))
		a << QStringLiteral("-tag:v") << QStringLiteral("hvc1");	// so players recognise HEVC in MP4

	a << QStringLiteral("-c:a") << QStringLiteral("aac") << QStringLiteral("-b:a") << QStringLiteral("%1k").arg(plan.audioKbps);
	if (container == QLatin1String("mp4"))	// fragmented: a crash or power cut leaves a playable file
		a << QStringLiteral("-movflags") << QStringLiteral("+frag_keyframe+empty_moov+default_base_moof");
	a << QStringLiteral("-f") << (container == QLatin1String("mkv") ? QStringLiteral("matroska") : QStringLiteral("mp4"));
	a << outFile;
	return a;
}

QString recordingFileName(const QDateTime &when, const QString &container)
{
	return QStringLiteral("HD60 Pro %1.%2").arg(when.toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")),
						    container == QLatin1String("mkv") ? QStringLiteral("mkv") : QStringLiteral("mp4"));
}

Recorder::Recorder(QObject *parent) : QObject(parent)
{
	m_proc.setProcessChannelMode(QProcess::MergedChannels);
	// If the app dies (crash, logout), ffmpeg gets SIGTERM, on which it still finishes the file.
	m_proc.setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGTERM); });
	connect(&m_proc, &QProcess::readyRead, this, [this] {
		m_log += QString::fromUtf8(m_proc.readAll());
		if (m_log.size() > 16000)
			m_log = m_log.right(8000);
	});
	connect(&m_proc, &QProcess::finished, this, &Recorder::onFinished);
	connect(&m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
		if (e == QProcess::FailedToStart)
			emit finished(false, tr("Could not start ffmpeg. Is it installed?"), m_file);
	});
	m_killTimer.setSingleShot(true);
	connect(&m_killTimer, &QTimer::timeout, this, [this] {
		if (m_proc.state() != QProcess::NotRunning)
			m_proc.terminate();	// ffmpeg still finishes the file on SIGTERM
	});
}

Recorder::~Recorder()
{
	if (m_proc.state() != QProcess::NotRunning) {
		m_proc.disconnect(this);
		m_proc.write("q");
		if (!m_proc.waitForFinished(4000)) {
			m_proc.terminate();
			if (!m_proc.waitForFinished(3000))
				m_proc.kill(), m_proc.waitForFinished(1000);
		}
	}
}

qint64 Recorder::elapsedMs() const
{
	return isRecording() ? QDateTime::currentMSecsSinceEpoch() - m_startMs : 0;
}

// Encodes a fraction of a second of test picture: listed encoders can still fail (no GPU, old driver).
static bool encoderWorks(const Encoder &e)
{
	QStringList a{ QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error") };
	if (e.kind == Encoder::Vaapi)
		a << QStringLiteral("-vaapi_device") << QStringLiteral("/dev/dri/renderD128");
	a << QStringLiteral("-f") << QStringLiteral("lavfi") << QStringLiteral("-i")
	  << QStringLiteral("color=c=black:s=1280x720:r=30:d=0.2");
	if (e.kind == Encoder::Vaapi)
		a << QStringLiteral("-vf") << QStringLiteral("format=nv12,hwupload");
	else
		a << QStringLiteral("-pix_fmt") << QStringLiteral("yuv420p");
	a << QStringLiteral("-c:v") << e.name << QStringLiteral("-f") << QStringLiteral("null") << QStringLiteral("-");
	QProcess p;
	p.start(QStringLiteral("ffmpeg"), a);
	if (!p.waitForFinished(8000)) {
		p.kill();
		p.waitForFinished(1000);
		return false;
	}
	return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

bool Recorder::pickEncoder(const QString &codec, Encoder *out, QString *error)
{
	if (m_encoderCache.contains(codec)) {
		*out = m_encoderCache.value(codec);
		return true;
	}
	for (const Encoder &e : encoderCandidates(codec)) {
		if (encoderWorks(e)) {
			m_encoderCache.insert(codec, e);
			*out = e;
			return true;
		}
	}
	*error = tr("ffmpeg has no working %1 encoder on this computer.").arg(codec.toUpper());
	return false;
}

bool Recorder::start(const DriverState &in, const AppSettings &settings, QString *error)
{
	if (isRecording()) {
		*error = tr("Already recording.");
		return false;
	}
	if (in.kind != DriverState::Streaming) {
		*error = tr("There is no input signal to record.");
		return false;
	}
	const QString folder = settings.recordFolder();
	if (!QDir().mkpath(folder) || !QFileInfo(folder).isWritable()) {
		*error = tr("Cannot write to the recording folder:\n%1").arg(folder);
		return false;
	}
	const QStorageInfo disk(folder);
	if (disk.isValid() && disk.bytesAvailable() < 500ll * 1024 * 1024) {
		*error = tr("Less than 500 MB free in %1. Free some space or choose another folder in Settings.").arg(folder);
		return false;
	}
	const RecordPlan plan = planFor(settings.rec, in);
	Encoder enc;
	if (!pickEncoder(plan.codec, &enc, error))
		return false;

	QDateTime when = QDateTime::currentDateTime();
	QString file = folder + QLatin1Char('/') + recordingFileName(when, settings.rec.container);
	while (QFileInfo::exists(file)) {	// two recordings within one second
		when = when.addSecs(1);
		file = folder + QLatin1Char('/') + recordingFileName(when, settings.rec.container);
	}
	m_file = file;
	m_log.clear();
	m_stopReason.clear();
	m_stopping = false;
	m_startMs = QDateTime::currentMSecsSinceEpoch();
	m_proc.start(QStringLiteral("ffmpeg"), buildFfmpegArgs(in, plan, enc, settings.rec.container, file));
	emit started(file);
	return true;
}

void Recorder::stop(const QString &reason)
{
	if (!isRecording() || m_stopping)
		return;
	m_stopping = true;
	m_stopReason = reason;
	m_proc.write("q");	// ffmpeg's own "finish the file and quit"
	m_killTimer.start(6000);
}

void Recorder::onFinished(int code, QProcess::ExitStatus st)
{
	m_killTimer.stop();
	m_log += QString::fromUtf8(m_proc.readAll());
	const QFileInfo fi(m_file);
	const bool haveFile = fi.exists() && fi.size() > 4096;
	// ffmpeg exits 0 after "q" and 255 after SIGTERM; both finish the file.
	const bool clean = m_stopping && st == QProcess::NormalExit && (code == 0 || code == 255);
	QString msg;
	if (clean && haveFile) {
		msg = m_stopReason.isEmpty() ? tr("Saved to %1").arg(m_file)
					     : tr("Recording stopped: %1. Saved to %2").arg(m_stopReason, m_file);
		emit finished(true, msg, m_file);
		return;
	}
	QStringList lines = m_log.trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	const QString tail = QStringList(lines.mid(qMax(0, lines.size() - 4))).join(QLatin1Char('\n'));
	if (!haveFile)
		msg = tr("The recording failed and no usable file was written.");
	else
		msg = tr("The recording stopped unexpectedly. What was recorded so far is in:\n%1").arg(m_file);
	if (!tail.isEmpty())
		msg += QStringLiteral("\n\n") + tail;
	emit finished(false, msg, m_file);
}
