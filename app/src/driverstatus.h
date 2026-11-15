// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QObject>
#include <QString>
#include <QTimer>

// What the driver (hd60pro.service) is doing, from its status file and systemd.
struct DriverState {
	enum Kind {
		NotInstalled,	// no hd60pro.service unit
		Stopped,	// unit exists, not running
		NoSignal,	// running, nothing on HDMI IN
		Unsupported,	// running, input mode cannot be captured
		Streaming,
	};
	Kind kind = Stopped;
	int width = 0, height = 0, fps = 0, audioRate = 0;
	bool interlaced = false, ntsc = false;
	double measuredFps = 0;
	QString hdmiOut;
	QString videoDevice = QStringLiteral("/dev/video60");
	QString problem;	// why it is not running, when known

	bool operator==(const DriverState &o) const;
	bool operator!=(const DriverState &o) const { return !(*this == o); }
	QString modeText() const;	// "1920x1080p59.94"
	// The captured format changed (a recording or preview must restart).
	bool sameStream(const DriverState &o) const;
};

// Parses the driver's status file; false when it is not a status object.
bool parseDriverStatus(const QByteArray &json, DriverState *out);

class DriverStatus : public QObject {
	Q_OBJECT
public:
	explicit DriverStatus(QObject *parent = nullptr);
	const DriverState &state() const { return m_state; }
	void startDriver();
	void refresh();
signals:
	void changed(const DriverState &state);
private:
	QString statusPath() const;
	QTimer m_timer;
	DriverState m_state;
	bool m_first = true;
	int m_tick = 0;
	QString m_unitState;	// cached "active" / "inactive" / "failed" / "not-found"
};
