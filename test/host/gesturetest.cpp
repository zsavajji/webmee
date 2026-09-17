// Drives WebViewItem's touch handling with synthetic QTouchEvents and checks
// what reaches the engine.
//
// The thing under test is the gesture state machine: while magnified, one
// finger can mean "pan the window into the surface" or "scroll the page", and
// the wrong answer either strands two thirds of a magnified page off screen or
// makes a sideways drag click whatever link it started on.
#include <QApplication>
#include <QGraphicsScene>
#include <QTouchEvent>
#include <QStringList>
#include <cstdio>
#include "webviewitem.h"

// sceneEvent is protected; a subclass is the whole of what is needed to poke it.
class Probe : public WebViewItem {
public:
    bool deliver(QEvent *e) { return sceneEvent(e); }
};

static QStringList g_log;
class Recorder : public QObject {
    Q_OBJECT
public slots:
    void onTouched(const QString &kind, int, int x, int y)
    { g_log << QString("%1 %2,%3").arg(kind).arg(x).arg(y); }
};
#include "gesturetest.moc"

static QTouchEvent::TouchPoint pt(int id, QPointF p, Qt::TouchPointState st)
{
    QTouchEvent::TouchPoint t(id);
    t.setPos(p);
    t.setState(st);
    return t;
}

static void send(Probe *item, QEvent::Type type,
                 const QList<QTouchEvent::TouchPoint> &pts)
{
    QTouchEvent e(type, QTouchEvent::TouchScreen, Qt::NoModifier, 0, pts);
    item->deliver(&e);
}

static int failures = 0;
static void is(const char *what, const QString &got, const QString &want)
{
    bool ok = (got == want);
    failures += !ok;
    printf("%s %-44s %s\n", ok ? "ok  " : "FAIL", what,
           ok ? qPrintable(got.isEmpty() ? QString("(nothing sent)") : got)
              : qPrintable(QString("got [%1] want [%2]").arg(got, want)));
}

// A one-finger drag from a to b, in steps, then release.
static void drag(Probe *item, QPointF a, QPointF b, int steps = 6)
{
    QList<QTouchEvent::TouchPoint> p;
    p << pt(0, a, Qt::TouchPointPressed);
    send(item, QEvent::TouchBegin, p);
    for (int i = 1; i <= steps; ++i) {
        QPointF at = a + (b - a) * (double(i) / steps);
        p.clear(); p << pt(0, at, Qt::TouchPointMoved);
        send(item, QEvent::TouchUpdate, p);
    }
    p.clear(); p << pt(0, b, Qt::TouchPointReleased);
    send(item, QEvent::TouchEnd, p);
}

static void tap(Probe *item, QPointF a)
{
    QList<QTouchEvent::TouchPoint> p;
    p << pt(0, a, Qt::TouchPointPressed);
    send(item, QEvent::TouchBegin, p);
    p.clear(); p << pt(0, a, Qt::TouchPointReleased);
    send(item, QEvent::TouchEnd, p);
}

// Pinch out about the centre of the item until the scale doubles.
static void pinchTo(Probe *item, double factor)
{
    const QPointF c(240, 340);
    QList<QTouchEvent::TouchPoint> p;
    p << pt(0, c + QPointF(-50, 0), Qt::TouchPointPressed)
      << pt(1, c + QPointF(50, 0), Qt::TouchPointPressed);
    send(item, QEvent::TouchBegin, p);
    for (int i = 1; i <= 8; ++i) {
        double f = 1.0 + (factor - 1.0) * (double(i) / 8);
        p.clear();
        p << pt(0, c + QPointF(-50 * f, 0), Qt::TouchPointMoved)
          << pt(1, c + QPointF(50 * f, 0), Qt::TouchPointMoved);
        send(item, QEvent::TouchUpdate, p);
    }
    p.clear();
    p << pt(0, c + QPointF(-50 * factor, 0), Qt::TouchPointReleased)
      << pt(1, c + QPointF(50 * factor, 0), Qt::TouchPointReleased);
    send(item, QEvent::TouchEnd, p);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QGraphicsScene scene;
    Probe *item = new Probe;
    scene.addItem(item);
    item->setWidth(480);
    item->setHeight(674);
    item->setSurfaceSize(QSize(480, 674));   // 1:1 at scale 1

    Recorder rec;
    QObject::connect(item, SIGNAL(touched(QString,int,int,int)),
                     &rec, SLOT(onTouched(QString,int,int,int)));

    // --- unmagnified: unchanged behaviour --------------------------------
    g_log.clear();
    tap(item, QPointF(100, 300));
    is("unmagnified tap is a press and a release", g_log.join(" "),
       "down 100,300 up 100,300");

    g_log.clear();
    drag(item, QPointF(240, 500), QPointF(240, 200), 3);
    // Exact pixels would only be asserting int() truncation in toSurface; what
    // matters is that the whole drag reaches the page, in order.
    is("unmagnified vertical drag scrolls",
       QString("%1 steps, %2..%3").arg(g_log.size())
           .arg(g_log.first().section(' ', 0, 0)).arg(g_log.last().section(' ', 0, 0)),
       "5 steps, down..up");

    g_log.clear();
    drag(item, QPointF(400, 300), QPointF(100, 300), 3);
    is("unmagnified sideways drag still goes to the page", g_log.join(" "),
       "down 400,300 motion 300,300 motion 200,300 motion 100,300 up 100,300");

    // --- magnify ----------------------------------------------------------
    g_log.clear();
    pinchTo(item, 2.0);
    // The engine is asked to re-render; a pinch is not a page gesture.
    is("pinch sends the page nothing", g_log.join(" "), "");

    // --- the reported bug -------------------------------------------------
    // Where a tap lands tells us which slice of the surface is on screen.
    g_log.clear();
    tap(item, QPointF(240, 340));
    const QString beforePan = g_log.join(" ");

    g_log.clear();
    drag(item, QPointF(400, 340), QPointF(100, 340), 6);
    is("magnified sideways drag never reaches the page", g_log.join(" "), "");

    g_log.clear();
    tap(item, QPointF(240, 340));
    const QString afterPan = g_log.join(" ");
    is("...and it moved the window instead",
       beforePan == afterPan ? "unchanged" : "moved", "moved");
    printf("     (same finger point mapped %s, then %s)\n",
           qPrintable(beforePan), qPrintable(afterPan));

    // --- vertical still scrolls while magnified ---------------------------
    // Where a press at the drag's start point maps to, so the replayed press
    // can be checked against it rather than against a hardcoded pixel.
    g_log.clear();
    tap(item, QPointF(240, 500));
    const QString startedAt = g_log.first().section(' ', 1, 1);

    g_log.clear();
    drag(item, QPointF(240, 500), QPointF(240, 200), 3);
    bool scrolled = g_log.size() >= 3 && g_log.first().startsWith("down")
                    && g_log.last().startsWith("up");
    is("magnified vertical drag still scrolls the page",
       scrolled ? "forwarded" : g_log.join(" "), "forwarded");
    is("...and the press is replayed from where the finger landed",
       g_log.first(), "down " + startedAt);

    // --- panning back must stop at the other edge -------------------------
    g_log.clear();
    drag(item, QPointF(100, 340), QPointF(400, 340), 6);   // back to the left
    drag(item, QPointF(100, 340), QPointF(400, 340), 6);   // and further
    is("panning past the edge sends nothing either", g_log.join(" "), "");
    g_log.clear();
    tap(item, QPointF(0, 340));
    is("the window stops at the left of the surface",
       g_log.first(), "down 0,340");

    // --- a tap while magnified must still click ---------------------------
    g_log.clear();
    tap(item, QPointF(240, 340));
    is("magnified tap is still a press and a release",
       g_log.size() == 2 && g_log.at(0).startsWith("down")
                         && g_log.at(1).startsWith("up") ? "click" : g_log.join(" "),
       "click");

    // --- the ghost frame ---------------------------------------------------
    // paint() refuses to draw until a frame has arrived, because an unpainted
    // window's pixmap is undefined memory. That guard is a latch, so a NEW
    // window mid-session has to clear it or the ghost is drawn immediately.
    {
        Probe *w = new Probe;
        scene.addItem(w);
        w->setWidth(480); w->setHeight(674);
        is("a fresh item has nothing to show",
           w->property("painted").toBool() ? "painted" : "blank", "blank");
        w->setEngineWindow(0x1001);
        w->frameReady();
        is("a frame makes it drawable",
           w->property("painted").toBool() ? "painted" : "blank", "painted");
        // Re-reporting the SAME id is not a no-op: the engine only reports a
        // window it has just created, and X recycles the ids of destroyed
        // ones, so an identical id means a different window. This assertion
        // used to say "changes nothing", which is exactly the bug it should
        // have caught -- a test can encode the defect as happily as the fix.
        w->setEngineWindow(0x1001);
        is("the same id again is still a NEW window",
           w->property("painted").toBool() ? "painted" : "blank", "blank");
        w->frameReady();
        w->setEngineWindow(0x2002);
        is("a new window goes blank until it has painted",
           w->property("painted").toBool() ? "painted" : "blank", "blank");
        w->frameReady();
        is("...and comes back on its first frame",
           w->property("painted").toBool() ? "painted" : "blank", "painted");
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
