# SPDX-License-Identifier: GPL-2.0-or-later
TEMPLATE = app
TARGET = hd60pro-app
QT += widgets multimedia multimediawidgets network
CONFIG += c++17 warn_on
DEFINES += QT_NO_CAST_FROM_ASCII_WARNINGS
OBJECTS_DIR = build
MOC_DIR = build
RCC_DIR = build
SOURCES += src/main.cpp src/mainwindow.cpp src/driverstatus.cpp src/recorder.cpp src/listener.cpp \
           src/settings.cpp src/settingsdialog.cpp
HEADERS += src/mainwindow.h src/driverstatus.h src/recorder.h src/listener.h src/settings.h src/settingsdialog.h
RESOURCES += resources/resources.qrc
