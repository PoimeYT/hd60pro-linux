// SPDX-License-Identifier: GPL-2.0-or-later
// Tests for the parts of the app that decide things: status parsing, ffmpeg arguments, PipeWire parsing.
#include <QtTest>

#include "driverstatus.h"
#include "listener.h"
#include "recorder.h"

static DriverState streaming1080()
{
	DriverState s;
	QByteArray j = R"({"state":"streaming","pid":42,"hdmi_out":"passthrough on","measured_fps":59.9,)"
		       R"("video_device":"/dev/video60","width":1920,"height":1080,"interlaced":false,"fps":60,"ntsc":true,"audio_rate":48000})";
	if (!parseDriverStatus(j, &s))
		qFatal("fixture does not parse");
	return s;
}

class LogicTest : public QObject {
	Q_OBJECT
private slots:
	void parsesStreamingStatus()
	{
		const DriverState s = streaming1080();
		QCOMPARE(s.kind, DriverState::Streaming);
		QCOMPARE(s.width, 1920);
		QCOMPARE(s.height, 1080);
		QCOMPARE(s.modeText(), QStringLiteral("1920x1080p59.94"));
		QCOMPARE(s.hdmiOut, QStringLiteral("passthrough on"));
		QCOMPARE(s.videoDevice, QStringLiteral("/dev/video60"));
	}
	void parsesNoSignal()
	{
		DriverState s;
		QVERIFY(parseDriverStatus(R"({"state":"no_signal","pid":1,"hdmi_out":"no display","measured_fps":0.0,"video_device":"/dev/video60"})", &s));
		QCOMPARE(s.kind, DriverState::NoSignal);
		QVERIFY(s.modeText().isEmpty());
	}
	void rejectsBrokenStatus()
	{
		DriverState s;
		QVERIFY(!parseDriverStatus("", &s));
		QVERIFY(!parseDriverStatus("{\"state\":\"strea", &s));		// half-written file
		QVERIFY(!parseDriverStatus(R"({"state":"bogus"})", &s));
		QVERIFY(!parseDriverStatus(R"({"state":"streaming","width":0,"height":0,"fps":0})", &s));
		QVERIFY(!parseDriverStatus(R"([1,2])", &s));
	}
	void ignoresDeviceOutsideDev()
	{
		DriverState s;
		QVERIFY(parseDriverStatus(R"({"state":"no_signal","video_device":"-i /etc/passwd"})", &s));
		QCOMPARE(s.videoDevice, QStringLiteral("/dev/video60"));
	}
	void streamChangeDetected()
	{
		DriverState a = streaming1080(), b = a;
		b.measuredFps = 30;
		QVERIFY(a.sameStream(b));
		b.height = 720;
		QVERIFY(!a.sameStream(b));
	}
	void presets()
	{
		const DriverState in = streaming1080();
		RecordSettings r;
		r.preset = Preset::Best;
		QCOMPARE(planFor(r, in).videoKbps, 40000);
		QCOMPARE(planFor(r, in).audioKbps, 320);
		r.preset = Preset::Balanced;
		QCOMPARE(planFor(r, in).videoKbps, 15000);
		r.preset = Preset::Small;
		QCOMPARE(planFor(r, in).videoKbps, 6000);
		QCOMPARE(planFor(r, in).outHeight, 0);	// 1080 input is not rescaled to 1080
		DriverState big = in;
		big.height = 1200;
		QCOMPARE(planFor(r, big).outHeight, 1080);
		r.preset = Preset::Custom;
		r.customCodec = QStringLiteral("hevc");
		r.customHeight = 720;
		r.customVideoKbps = 999999;
		QCOMPARE(planFor(r, in).codec, QStringLiteral("hevc"));
		QCOMPARE(planFor(r, in).outHeight, 720);
		QCOMPARE(planFor(r, in).videoKbps, 150000);	// clamped
		DriverState small = in;
		small.height = 720;
		r.customHeight = 1080;
		QCOMPARE(planFor(r, small).outHeight, 0);	// never upscale
	}
	void ffmpegArgsNvencMp4()
	{
		const DriverState in = streaming1080();
		RecordSettings r;
		const QStringList a = buildFfmpegArgs(in, planFor(r, in), encoderCandidates(QStringLiteral("h264")).at(0),
						      QStringLiteral("mp4"), QStringLiteral("/tmp/out file.mp4"));
		QCOMPARE(a.last(), QStringLiteral("/tmp/out file.mp4"));	// one argument, spaces intact
		QVERIFY(a.contains(QStringLiteral("-n")));
		QVERIFY(!a.contains(QStringLiteral("-y")));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-video_size")) + 1), QStringLiteral("1920x1080"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-framerate")) + 1), QStringLiteral("60000/1001"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-c:v")) + 1), QStringLiteral("h264_nvenc"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-b:v")) + 1), QStringLiteral("15000k"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-b:a")) + 1), QStringLiteral("192k"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-movflags")) + 1), QStringLiteral("+frag_keyframe+empty_moov+default_base_moof"));
		QVERIFY(!a.contains(QStringLiteral("-vf")));
		// both inputs come before any output option
		QVERIFY(a.indexOf(QStringLiteral("hd60pro")) < a.indexOf(QStringLiteral("-c:v")));
	}
	void ffmpegArgsVaapiScaledMkv()
	{
		DriverState in = streaming1080();
		in.ntsc = false;
		RecordSettings r;
		r.preset = Preset::Custom;
		r.customCodec = QStringLiteral("hevc");
		r.customHeight = 720;
		const QStringList a = buildFfmpegArgs(in, planFor(r, in), encoderCandidates(QStringLiteral("hevc")).at(1),
						      QStringLiteral("mkv"), QStringLiteral("/x.mkv"));
		QVERIFY(a.indexOf(QStringLiteral("-vaapi_device")) < a.indexOf(QStringLiteral("-i")));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-vf")) + 1), QStringLiteral("scale=-2:720,format=nv12,hwupload"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-framerate")) + 1), QStringLiteral("60"));
		QVERIFY(!a.contains(QStringLiteral("-movflags")));
		QCOMPARE(a.at(a.lastIndexOf(QStringLiteral("-f")) + 1), QStringLiteral("matroska"));
	}
	void hevcMp4Tagged()
	{
		const DriverState in = streaming1080();
		RecordSettings r;
		r.preset = Preset::Custom;
		r.customCodec = QStringLiteral("hevc");
		const QStringList a = buildFfmpegArgs(in, planFor(r, in), encoderCandidates(QStringLiteral("hevc")).at(0),
						      QStringLiteral("mp4"), QStringLiteral("/x.mp4"));
		QCOMPARE(a.at(a.indexOf(QStringLiteral("-tag:v")) + 1), QStringLiteral("hvc1"));
	}
	void fileName()
	{
		const QDateTime t(QDate(2026, 11, 15), QTime(9, 5, 7));
		QCOMPARE(recordingFileName(t, QStringLiteral("mp4")), QStringLiteral("HD60 Pro 2026-11-15 09-05-07.mp4"));
		QCOMPARE(recordingFileName(t, QStringLiteral("mkv")), QStringLiteral("HD60 Pro 2026-11-15 09-05-07.mkv"));
	}
	void recorderRefusesWithoutSignal()
	{
		Recorder r;
		DriverState s;
		s.kind = DriverState::NoSignal;
		QString err;
		QVERIFY(!r.start(s, AppSettings(), &err));
		QVERIFY(!err.isEmpty());
	}
	void recorderRefusesUnwritableFolder()
	{
		Recorder r;
		AppSettings st;
		st.rec.folder = QStringLiteral("/proc/hd60pro-nope");
		QString err;
		QVERIFY(!r.start(streaming1080(), st, &err));
		QVERIFY(err.contains(QStringLiteral("/proc/hd60pro-nope")));
	}
	// Needs a live picture on /dev/video60 and the hd60pro audio source: HD60_LIVE=1 ./tst_logic
	void liveRecording()
	{
		if (qgetenv("HD60_LIVE").isEmpty())
			QSKIP("set HD60_LIVE=1 with the driver streaming");
		QTemporaryDir dir;
		AppSettings st;
		st.rec.folder = dir.path();
		st.rec.preset = Preset(qEnvironmentVariableIntValue("HD60_PRESET"));
		if (st.rec.preset == Preset::Custom) {
			st.rec.customCodec = QStringLiteral("hevc");
			st.rec.customHeight = 720;
			st.rec.container = QStringLiteral("mkv");
		}
		Recorder r;
		QSignalSpy done(&r, &Recorder::finished);
		QString err;
		QVERIFY2(r.start(streaming1080(), st, &err), qPrintable(err));
		QTest::qWait(5000);
		QVERIFY2(r.isRecording(), qPrintable(done.isEmpty() ? QString() : done.at(0).at(1).toString()));
		r.stop();
		QVERIFY(done.wait(10000));
		qInfo() << done.at(0).at(1).toString();
		QVERIFY(done.at(0).at(0).toBool());
		QProcess probe;
		probe.start(QStringLiteral("ffprobe"), { QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
			    QStringLiteral("stream=codec_name,width,height,r_frame_rate:format=duration"), QStringLiteral("-of"),
			    QStringLiteral("compact"), r.file() });
		QVERIFY(probe.waitForFinished(10000));
		const QString out = QString::fromUtf8(probe.readAllStandardOutput());
		qInfo().noquote() << out;
		QVERIFY(out.contains(QStringLiteral("codec_name=aac")));
		QVERIFY(out.contains(st.rec.preset == Preset::Custom ? QStringLiteral("codec_name=hevc|width=1280|height=720")
								    : QStringLiteral("codec_name=h264|width=1920|height=1080")));
	}
	void parsesSinksAndInputs()
	{
		const auto sinks = parseSinks(R"([{"index":64,"name":"alsa_output.hdmi","description":"HDMI"},{"index":1,"name":"bt","description":""},{"index":2}])");
		QCOMPARE(sinks.size(), 2);
		QCOMPARE(sinks.at(0).description, QStringLiteral("HDMI"));
		QCOMPARE(sinks.at(1).description, QStringLiteral("bt"));
		QCOMPARE(parseListenInput(R"([{"index":185,"properties":{"node.name":"discord"}},{"index":200,"properties":{"node.name":"output.hd60pro-listen"}}])"), 200);
		QCOMPARE(parseListenInput(R"([{"index":185,"properties":{"node.name":"discord"}}])"), -1);
		QCOMPARE(parseListenInput("garbage"), -1);
	}
};

QTEST_GUILESS_MAIN(LogicTest)
#include "tst_logic.moc"
