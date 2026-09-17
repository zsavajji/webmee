// Host-side copy of main.cpp's N9_TEST_QML self-test: compile each page with
// QDeclarativeComponent and print the errors. No window is shown.
#include <QApplication>
#include <QDeclarativeEngine>
#include <QDeclarativeComponent>
#include <QStringList>
#include <QUrl>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QDeclarativeEngine engine;
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "-I") { engine.addImportPath(argv[++i]); continue; }
    }
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        QString a = argv[i];
        if (a == "-I") { ++i; continue; }
        QDeclarativeComponent c(&engine, QUrl::fromLocalFile(a));
        if (c.isError()) {
            ++bad;
            printf("FAIL %s\n%s\n", qPrintable(a), qPrintable(c.errorString().trimmed()));
        } else {
            printf("ok   %s\n", qPrintable(a));
        }
    }
    printf("%d file(s) with errors\n", bad);
    return bad ? 1 : 0;
}
