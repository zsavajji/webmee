TEMPLATE = app
TARGET = signonprobe
QT = core

# The libsignon-qt.pc emits DEVICE include paths (/usr/include/signon-qt) and
# mad's pkg-config does not prefix them with the sysroot, so point at them here.
SYSROOT = $$(HOME)/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim
INCLUDEPATH += $$SYSROOT/usr/include/signon-qt
INCLUDEPATH += $$SYSROOT/usr/include/signon-qt/SignOn
LIBS += -L$$SYSROOT/usr/lib -lsignon-qt

SOURCES += signonprobe.cpp
