# The WebMee Harmattan UI shell. Built with the SDK's own qmake + GCC 4.4 so it matches
# the device's Qt 4.7.4 ABI exactly; the engine is a separate GCC-14 process.
TEMPLATE = app
TARGET = WebMee

# The platform share sheet (com.nokia.ShareUi over D-Bus).
# The .prf adds the interface headers and pulls in CONFIG += meegotouch, but
# MDataUri lives in a directory of its own that neither puts on the path, so
# `#include <MDataUri>` fails until this is added by hand.
CONFIG += shareuiinterface-maemo-meegotouch
INCLUDEPATH += /usr/include/MDataUri
LIBS += -lmdatauri

QT += declarative opengl
CONFIG += qdeclarative-boostable

SOURCES += main.cpp browserengine.cpp webviewitem.cpp downloadmanager.cpp memorywatcher.cpp bookmarkstore.cpp historystore.cpp passwordstore.cpp
HEADERS += browserengine.h webviewitem.h downloadmanager.h memorywatcher.h bookmarkstore.h historystore.h passwordstore.h
RESOURCES += qml.qrc

# XComposite: the shell redirects the engine's X window and samples its backing
# pixmap as a texture. EGL/GLESv2 come with QtOpenGL but are linked explicitly
# because webviewitem.cpp calls them directly.
LIBS += -lXcomposite -lX11 -lEGL -lGLESv2

# signond (saved passwords). libsignon-qt.pc emits DEVICE include paths and mad's
# pkg-config does not sysroot-prefix them, so point at the sysroot explicitly.
SYSROOT = $$(HOME)/QtSDK/Madde/sysroots/harmattan_sysroot_10.2011.34-1_slim
INCLUDEPATH += $$SYSROOT/usr/include/signon-qt
INCLUDEPATH += $$SYSROOT/usr/include/signon-qt/SignOn
LIBS += -L$$SYSROOT/usr/lib -lsignon-qt

# The applauncherd booster contract: all four parts, or invoker cannot start it.
DEFINES += HARMATTAN_BOOSTER
QMAKE_CXXFLAGS += -fPIC -fvisibility=hidden -fvisibility-inlines-hidden
QMAKE_LFLAGS += -pie -rdynamic
LIBS += -lmdeclarativecache

target.path = /opt/n9browser/bin
INSTALLS += target

contains(MEEGO_EDITION, harmattan) {
    desktopfile.files = webmee.desktop
    desktopfile.path = /usr/share/applications
    icon.files = webmee80.png
    icon.path = /usr/share/icons/hicolor/80x80/apps
    INSTALLS += desktopfile icon
}
