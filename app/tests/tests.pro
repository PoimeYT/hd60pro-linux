# SPDX-License-Identifier: GPL-2.0-or-later
TEMPLATE = app
TARGET = tst_logic
QT += testlib network
QT -= gui
CONFIG += c++17 testcase no_testcase_installs
OBJECTS_DIR = build
MOC_DIR = build
INCLUDEPATH += ../src
SOURCES += tst_logic.cpp ../src/driverstatus.cpp ../src/recorder.cpp ../src/listener.cpp ../src/settings.cpp
HEADERS += ../src/driverstatus.h ../src/recorder.h ../src/listener.h
