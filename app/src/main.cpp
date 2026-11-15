// SPDX-License-Identifier: GPL-2.0-or-later
// HD60 Pro companion app: preview, one-button recording and headphone listening.
#include <QApplication>
#include <QCommandLineParser>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>
#include <unistd.h>

#include "mainwindow.h"

// SIGTERM/SIGINT/SIGHUP (logout, kill, Ctrl-C) end the app the normal way, so a recording is finished.
static int g_sigFd[2];

static void onSignal(int)
{
	const char c = 1;
	(void)!::write(g_sigFd[0], &c, 1);
}

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	QCoreApplication::setOrganizationName(QStringLiteral("hd60pro-linux"));
	QCoreApplication::setApplicationName(QStringLiteral("hd60pro-app"));
	QGuiApplication::setDesktopFileName(QStringLiteral("hd60pro"));
	QApplication::setQuitOnLastWindowClosed(false);	// it can live in the tray

	QCommandLineParser args;
	args.setApplicationDescription(QStringLiteral("Preview, record and listen to the Elgato HD60 Pro"));
	args.addHelpOption();
	const QCommandLineOption trayOpt(QStringLiteral("tray"), QStringLiteral("Start hidden in the system tray."));
	args.addOption(trayOpt);
	args.process(app);

	// One instance per user: a second start only brings the first window forward.
	const QString serverName = QStringLiteral("hd60pro-app-%1").arg(getuid());
	{
		QLocalSocket probe;
		probe.connectToServer(serverName);
		if (probe.waitForConnected(500)) {
			probe.write("show\n");
			probe.waitForBytesWritten(500);
			return 0;
		}
	}
	QLocalServer::removeServer(serverName);	// stale socket from a crash
	QLocalServer server;
	server.setSocketOptions(QLocalServer::UserAccessOption);
	server.listen(serverName);

	MainWindow w;
	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, g_sigFd) == 0) {
		auto *n = new QSocketNotifier(g_sigFd[1], QSocketNotifier::Read, &app);
		QObject::connect(n, &QSocketNotifier::activated, &app, [n] {
			n->setEnabled(false);
			QCoreApplication::quit();
		});
		struct sigaction sa = {};
		sa.sa_handler = onSignal;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		for (int sig : { SIGTERM, SIGINT, SIGHUP })
			sigaction(sig, &sa, nullptr);
	}
	QObject::connect(&server, &QLocalServer::newConnection, &w, [&] {
		while (QLocalSocket *c = server.nextPendingConnection()) {
			c->deleteLater();
			w.bringToFront();
		}
	});
	if (!args.isSet(trayOpt) || !QSystemTrayIcon::isSystemTrayAvailable())
		w.show();
	return app.exec();
}
