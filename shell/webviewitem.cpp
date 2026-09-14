#include "webviewitem.h"

#include <QPainter>
#include <QGraphicsSceneMouseEvent>
#include <QTouchEvent>
#include <math.h>
#include <QX11Info>
#include <QDebug>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

// How far a pinch may magnify. 1.0 is the fitted view: zooming out past it
// would only reveal empty space, since the texture is exactly the viewport.
static const double kMinViewScale = 1.0;
// GL_MAX_TEXTURE_SIZE is 2048 here and the viewport is 674 px tall, so the
// engine cannot render more than 3x. Asking for more would just be clamped.
static const double kMaxViewScale = 3.0;

namespace {

// --- X error trap ----------------------------------------------------------
// XCompositeNameWindowPixmap on a window the engine has just destroyed raises
// BadWindow, and Qt's default handler turns that into an abort. The engine
// dying must not take the shell with it.
int g_xerror = 0;
int (*g_prevHandler)(Display *, XErrorEvent *) = 0;

int trapHandler(Display *, XErrorEvent *e)
{
    g_xerror = e->error_code;
    return 0;
}

void trapStart()
{
    g_xerror = 0;
    g_prevHandler = XSetErrorHandler(trapHandler);
}

int trapEnd(Display *dpy)
{
    XSync(dpy, False);                 // force pending errors to arrive
    XSetErrorHandler(g_prevHandler);
    return g_xerror;
}

PFNEGLCREATEIMAGEKHRPROC eglCreateImage_;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImage_;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glImageTargetTexture_;

bool resolveEglImage()
{
    static int state = -1;
    if (state < 0) {
        eglCreateImage_ = (PFNEGLCREATEIMAGEKHRPROC) eglGetProcAddress("eglCreateImageKHR");
        eglDestroyImage_ = (PFNEGLDESTROYIMAGEKHRPROC) eglGetProcAddress("eglDestroyImageKHR");
        glImageTargetTexture_ = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
            eglGetProcAddress("glEGLImageTargetTexture2DOES");
        state = (eglCreateImage_ && glImageTargetTexture_) ? 1 : 0;
        if (!state)
            qWarning("WebViewItem: no EGLImage entry points; page cannot be composited");
    }
    return state == 1;
}

const char *kVertexShader =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() { v_tex = a_tex; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

const char *kFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_tex); }\n";

GLuint compileShader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        glGetShaderInfoLog(s, sizeof log, 0, log);
        qWarning("WebViewItem: shader failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

WebViewItem::WebViewItem(QDeclarativeItem *parent)
    : QDeclarativeItem(parent)
    , m_window(0)
    , m_pixmap(0)
    , m_image(EGL_NO_IMAGE_KHR)
    , m_texture(0)
    , m_program(0)
    , m_attrPos(-1)
    , m_attrTex(-1)
    , m_programFailed(false)
    , m_sawFrame(false)
    , m_crashed(false)
    , m_topInset(0)
    , m_bottomInset(0)
    , m_haveTouch(false)
    , m_touchDown(false)
    , m_pinching(false)
    , m_pinchBaseDistance(0.0)
    , m_viewScale(1.0)
    , m_pinchStartScale(1.0)
    , m_pendingSurface(false)
    , m_rejected(0)
    , m_verifyLeft(qgetenv("N9_VERIFY_PAGE").toInt())
{
    // Resizing the engine's surface means resizing WebKit's viewport, which
    // relayouts the page. Showing the keyboard churns this item's height for a
    // moment, so let it settle first: a show-then-hide never reaches the engine
    // and the page does not reflow under the user's finger.
    m_resizeTimer.setSingleShot(true);
    m_resizeTimer.setInterval(250);
    connect(&m_resizeTimer, SIGNAL(timeout()), this, SLOT(commitSurfaceSize()));

    // QGraphicsItem ignores the mouse unless a button is accepted; the N9
    // delivers single-finger touch as mouse events.
    setAcceptedMouseButtons(Qt::LeftButton);
    // Two fingers are the whole point: pinch cannot be seen through the
    // single-point mouse events the platform synthesises.
    setAcceptTouchEvents(true);
    // QDeclarativeItem sets ItemHasNoContents, so paint() is never called
    // unless this is cleared. Forgetting it looks exactly like a black page.
    setFlag(QGraphicsItem::ItemHasNoContents, false);
}

WebViewItem::~WebViewItem()
{
    releasePixmap();
}

void WebViewItem::setEngineWindow(qulonglong window)
{
    if (m_window == (unsigned long) window)
        return;
    releasePixmap();
    m_window = (unsigned long) window;
    update();
}

// The chrome floats over this item: the URL bar over the top m_topInset rows,
// the platform toolbar over the bottom m_bottomInset. Rather than let either
// cover the page -- which made those rows untappable, and invisible on exactly
// the pages whose dialogs live there -- the page is drawn BETWEEN them. The
// item's geometry does not change, so the engine's surface is not resized and
// the page is not relaid out; only the bottom-most rows of the surface go
// unshown while the chrome is up, and a scroll hides it and reveals them.
static bool takeInset(int &slot, int value)
{
    if (value < 0)
        value = 0;
    if (value == slot)
        return false;
    slot = value;
    return true;
}

void WebViewItem::setTopInset(int inset)
{
    if (takeInset(m_topInset, inset))
        update();
}

void WebViewItem::setBottomInset(int inset)
{
    if (takeInset(m_bottomInset, inset))
        update();
}

// Item y of the page's bottom edge.
double WebViewItem::pageBottom() const
{
    const double b = height() - m_bottomInset;
    return b > 0.0 ? b : 0.0;
}

// The chrome is plain QML items, which accept neither mouse nor touch, so
// QGraphicsScene hands events over them straight down to this one -- where they
// would click whatever the bar happens to be sitting over.
bool WebViewItem::inPage(const QPointF &p) const
{
    return p.y() >= m_topInset && p.y() < pageBottom();
}

// Item y -> how far down the surface that is, as a fraction of an unmagnified
// view. paint() and toSurface() both go through here, so what is drawn and what
// is touched cannot drift apart -- an offset between them is invisible until a
// tap lands on the wrong link.
double WebViewItem::vFraction(double itemY) const
{
    const double h = height();
    if (h <= 0)
        return 0.0;
    const double y = itemY - m_topInset;
    return y > 0.0 ? y / h : 0.0;
}

void WebViewItem::commitSurfaceSize()
{
    if (m_requestedSize.isEmpty() || m_requestedSize == m_wantSize)
        return;
    m_wantSize = m_requestedSize;
    emit surfaceSizeChanged(m_wantSize);
}

void WebViewItem::setSurfaceSize(const QSize &size)
{
    if (size.isEmpty() || size == m_surfaceSize)
        return;
    m_surfaceSize = size;
    // Keep drawing the old surface, briefly stretched, until a frame arrives at
    // the new size. Dropping it here is what produced the glitch.
    m_pendingSurface = true;
    update();
}

void WebViewItem::frameReady()
{
    // A frame at the new size means the replacement pixmap now has content.
    if (m_pendingSurface && acquirePixmap())
        m_pendingSurface = false;

    if (!m_sawFrame || m_crashed) {
        m_sawFrame = true;
        m_crashed = false;
        emit paintedChanged();
    }
    update();
}

void WebViewItem::pageCrashed()
{
    // Drop the texture as well as hiding it. A dead process's window pixmap is
    // undefined memory -- on this GPU routinely the previous framebuffer, which
    // is how a crash ended up displaying a rotated copy of our own UI.
    m_sawFrame = false;
    m_crashed = true;
    releasePixmap();
    emit paintedChanged();
    update();
}

int WebViewItem::verifyRemaining() const
{
    return m_verifyLeft;
}

void WebViewItem::geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QDeclarativeItem::geometryChanged(newGeometry, oldGeometry);
    const QSize s(int(newGeometry.width()), int(newGeometry.height()));
    if (s.isEmpty())
        return;
    if (s == m_wantSize) {
        // Back to the size the engine already has. Cancel any pending resize:
        // a keyboard that opens and closes must not leave a queued reflow to
        // the intermediate height behind it.
        m_resizeTimer.stop();
        m_requestedSize = s;
        return;
    }
    // The 320px phone layout is the ENGINE's business -- it derives WebKit's
    // device scale factor from this size. Nothing to zoom here.
    m_requestedSize = s;
    m_resizeTimer.start();
}

void WebViewItem::releasePixmap()
{
    Display *dpy = QX11Info::display();
    if (m_texture) {
        glDeleteTextures(1, &m_texture);
        m_texture = 0;
    }
    if (m_image != EGL_NO_IMAGE_KHR) {
        if (eglDestroyImage_)
            eglDestroyImage_(eglGetCurrentDisplay(), m_image);
        m_image = EGL_NO_IMAGE_KHR;
    }
    if (m_pixmap && dpy) {
        trapStart();
        XFreePixmap(dpy, m_pixmap);
        trapEnd(dpy);
        m_pixmap = 0;
    }
    m_pixmapSize = QSize();
}

bool WebViewItem::acquirePixmap()
{
    Display *dpy = QX11Info::display();
    if (!dpy || !m_window || !resolveEglImage())
        return false;

    // Deliberately NOT releasing the current texture first. Resizing a
    // redirected window orphans its pixmap, and a freshly named one holds
    // undefined memory until WebKit paints into it -- which on this GPU is
    // routinely the last framebuffer. Keeping the old, still-valid pixmap on
    // screen until the replacement is proven correct is what stops the page
    // area flashing someone else's pixels every time the keyboard appears.

    // The ENGINE redirects its own window, before it is ever mapped -- only it
    // can do so early enough to stop the page flashing across the screen
    // unrotated at startup. A window may have just one redirecting client, so
    // this side must not ask again; it only names the pixmap.
    trapStart();
    Pixmap pm = XCompositeNameWindowPixmap(dpy, m_window);
    if (int err = trapEnd(dpy)) {
        qWarning("WebViewItem: NameWindowPixmap failed (X error %d)", err);
        return false;
    }

    // Ask X how big it really is rather than assuming it matches what we
    // requested: the engine resizes asynchronously and we may be a frame early.
    Window rootRet;
    int x, y;
    unsigned int w, h, bw, depth;
    trapStart();
    Status ok = XGetGeometry(dpy, pm, &rootRet, &x, &y, &w, &h, &bw, &depth);
    if (trapEnd(dpy) || !ok) {
        XFreePixmap(dpy, pm);
        return false;
    }

    // If a resize is in flight, a pixmap of the old size is the stale one we
    // are trying to avoid: leave the current texture alone and try again on the
    // next frame.
    // Tolerant, not exact: the logical size is device/scale rounded, so the
    // surface WebKit ends up rendering can be a pixel off what either side
    // predicted (480x673 against an expected 480x674). Demanding equality
    // rejected every pixmap and showed a black page. A few pixels of slack
    // still rejects a genuinely stale surface from before a resize.
    const QSize expected = m_surfaceSize.isEmpty() ? m_wantSize : m_surfaceSize;
    if (!expected.isEmpty()
        && (qAbs(int(w) - expected.width()) > 4 || qAbs(int(h) - expected.height()) > 4)) {
        // Silent rejection here looks exactly like a frozen page: the old
        // texture keeps being drawn and nothing ever says why.
        if (++m_rejected == 1 || m_rejected % 60 == 0)
            qWarning("WebViewItem: pixmap %ux%u != expected %dx%d (%d rejected)",
                     w, h, expected.width(), expected.height(), m_rejected);
        trapStart();
        XFreePixmap(dpy, pm);
        trapEnd(dpy);
        return false;
    }
    m_rejected = 0;

    EGLDisplay edpy = eglGetCurrentDisplay();
    EGLImageKHR img = eglCreateImage_(edpy, EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
                                      (EGLClientBuffer) pm, 0);
    if (img == EGL_NO_IMAGE_KHR) {
        qWarning("WebViewItem: eglCreateImageKHR failed (0x%x)", eglGetError());
        trapStart();
        XFreePixmap(dpy, pm);
        trapEnd(dpy);
        return false;
    }

    releasePixmap();     // only now: the replacement is ready and the right size

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Not just tidiness: NPOT textures are only legal in core GLES2 with
    // CLAMP_TO_EDGE and no mipmaps, and GL_OES_texture_npot is absent here.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glImageTargetTexture_(GL_TEXTURE_2D, img);

    m_pixmap = pm;
    m_image = img;
    m_texture = tex;
    m_pixmapSize = QSize(int(w), int(h));
    qWarning("WebViewItem: bound window 0x%lx -> pixmap 0x%lx %ux%u depth %u",
             m_window, (unsigned long) pm, w, h, depth);
    return true;
}

bool WebViewItem::buildProgram()
{
    if (m_program)
        return true;
    if (m_programFailed)
        return false;

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) {
        m_programFailed = true;
        return false;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = { 0 };
        glGetProgramInfoLog(m_program, sizeof log, 0, log);
        qWarning("WebViewItem: link failed: %s", log);
        glDeleteProgram(m_program);
        m_program = 0;
        m_programFailed = true;
        return false;
    }
    m_attrPos = glGetAttribLocation(m_program, "a_pos");
    m_attrTex = glGetAttribLocation(m_program, "a_tex");
    return true;
}

bool WebViewItem::ensureTexture()
{
    if (!m_window)
        return false;
    // Re-acquire when the engine's window has been resized under us; the old
    // pixmap still exists but stops tracking the window.
    // Any texture will do here: a stale-but-valid one is drawn while a resize
    // settles, and frameReady() swaps it for the new surface once that surface
    // actually has something in it.
    if (m_texture)
        return true;
    return acquirePixmap();
}

void WebViewItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *)
{
    QPaintDevice *device = painter->device();
    if (!device || width() <= 0 || height() <= 0)
        return;

    // Nothing has been rendered into the engine's window yet, so its backing
    // pixmap holds undefined memory -- on a tiler that is routinely whatever
    // was in the framebuffer before, which flashes the previous screen across
    // the page area for the ten-odd seconds WebKit takes to start. Draw
    // nothing until a frame has actually arrived; QML shows a splash meanwhile.
    if (!m_sawFrame)
        return;

    // Where this item lands on the paint device, rotation and all. Reading the
    // transform is what makes the page follow Qt's orientation handling instead
    // of fighting it.
    const QTransform t = painter->combinedTransform();
    const int dw = device->width();
    const int dh = device->height();
    if (dw <= 0 || dh <= 0)
        return;

    painter->beginNativePainting();

    if (ensureTexture() && buildProgram()) {
        const qreal w = width(), h = height();
        // Draw the page between the two bars, wherever their hide animations
        // currently have them. The surface keeps its full size throughout.
        const qreal top = qMin(qreal(m_topInset), h);
        const qreal bot = qMax(qreal(pageBottom()), top);
        const QPointF corner[4] = {
            t.map(QPointF(0, top)), t.map(QPointF(w, top)),
            t.map(QPointF(0, bot)), t.map(QPointF(w, bot))
        };
        GLfloat pos[8];
        for (int i = 0; i < 4; ++i) {
            pos[i * 2]     = GLfloat(2.0 * corner[i].x() / dw - 1.0);
            pos[i * 2 + 1] = GLfloat(1.0 - 2.0 * corner[i].y() / dh);
        }
        // Magnification is a window into the texture, not a bigger quad: the
        // vertices always fill the item and the texture coordinates select the
        // visible part. Once the engine has re-rendered the surface at this
        // magnification the mapping is 1:1 and the text is sharp.
        // Top-left origin, measured: the item's top-left samples v=0.
        const GLfloat du = GLfloat(1.0 / m_viewScale);
        // Shorter than du by whatever the chrome covers, so the rows that are
        // shown stay 1:1 with the surface instead of being squashed.
        const GLfloat dv = GLfloat(vFraction(bot) / m_viewScale);
        const GLfloat u0 = GLfloat(m_viewOrigin.x()), v0 = GLfloat(m_viewOrigin.y());
        const GLfloat tex[8] = {
            u0,      v0,
            u0 + du, v0,
            u0,      v0 + dv,
            u0 + du, v0 + dv
        };

        // Qt's paint engine leaves a VBO bound; with one bound, client-side
        // array pointers are read as buffer offsets and nothing draws.
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        glUseProgram(m_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_texture);
        glUniform1i(glGetUniformLocation(m_program, "u_tex"), 0);

        glEnableVertexAttribArray(m_attrPos);
        glEnableVertexAttribArray(m_attrTex);
        glVertexAttribPointer(m_attrPos, 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(m_attrTex, 2, GL_FLOAT, GL_FALSE, 0, tex);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(m_attrPos);
        glDisableVertexAttribArray(m_attrTex);

        // N9_VERIFY_PAGE=1 reads a few pixels back out of the framebuffer we
        // just drew into. It is the only way to tell "the page is composited"
        // from "the screen is off or the phone is locked", and it costs a
        // stalled pipeline, so it is off unless asked for.
        // Only sample once the engine has actually delivered a frame: the
        // paints during startup all run before the first one and would just
        // report black.
        if (m_verifyLeft > 0 && m_sawFrame) {
            --m_verifyLeft;
            const QPointF mid = t.map(QPointF(w / 2, h / 2));
            const QPointF q1 = t.map(QPointF(w / 4, h / 4));
            const QPointF q3 = t.map(QPointF(w * 3 / 4, h * 3 / 4));
            const QPointF pts[3] = { q1, mid, q3 };
            const char *names[3] = { "quarter", "centre", "three-quarter" };
            glFinish();
            for (int i = 0; i < 3; ++i) {
                GLubyte px[4] = { 0, 0, 0, 0 };
                const int gx = int(pts[i].x());
                const int gy = dh - 1 - int(pts[i].y());   // GL y is bottom-up
                if (gx < 0 || gy < 0 || gx >= dw || gy >= dh)
                    continue;
                glReadPixels(gx, gy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                qWarning("WebViewItem[%d]: %-14s device(%d,%d) = %d,%d,%d,%d",
                         m_verifyLeft, names[i], gx, gy, px[0], px[1], px[2], px[3]);
            }
        }
    }

    painter->endNativePainting();
}

// WPE's own TouchGestureController turns these into clicks, scrolls and context
// menus, so the shell forwards raw points and recognises nothing itself.
// Mouse is the FALLBACK path. Qt only synthesises mouse events from touch when
// the touch event was not accepted, so these normally never run -- but they keep
// the page usable if touch delivery is not set up on the viewport.
void WebViewItem::mousePressEvent(QGraphicsSceneMouseEvent *event)
{
    if (m_haveTouch) { event->ignore(); return; }
    if (!inPage(event->pos())) { event->ignore(); return; }
    const QPointF sp = toSurface(event->pos());
    emit touched("down", 0, int(sp.x()), int(sp.y()));
    event->accept();
}

void WebViewItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event)
{
    if (m_haveTouch) { event->ignore(); return; }
    const QPointF sp = toSurface(event->pos());
    emit touched("motion", 0, int(sp.x()), int(sp.y()));
    event->accept();
}

void WebViewItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event)
{
    if (m_haveTouch) { event->ignore(); return; }
    const QPointF sp = toSurface(event->pos());
    emit touched("up", 0, int(sp.x()), int(sp.y()));
    event->accept();
}

bool WebViewItem::sceneEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
        handleTouch(static_cast<QTouchEvent *>(event));
        event->accept();
        return true;
    default:
        return QDeclarativeItem::sceneEvent(event);
    }
}

// Item coordinates -> device pixels of the rendered surface. WebKit divides
// these by the device scale factor to get CSS pixels, so magnification and
// panning both come out right without the engine knowing about either.
QPointF WebViewItem::toSurface(const QPointF &p) const
{
    const double w = width(), h = height();
    if (w <= 0 || h <= 0)
        return p;
    const QSize s = m_surfaceSize.isEmpty() ? m_wantSize : m_surfaceSize;
    if (s.isEmpty())
        return p;
    return QPointF((m_viewOrigin.x() + (p.x() / w) / m_viewScale) * s.width(),
                   (m_viewOrigin.y() + vFraction(p.y()) / m_viewScale) * s.height());
}

void WebViewItem::handleTouch(QTouchEvent *event)
{
    m_haveTouch = true;

    // A point that is Released is reported in the same event that ends it, so
    // "how many fingers are still down" has to exclude those.
    QList<QTouchEvent::TouchPoint> live;
    const QList<QTouchEvent::TouchPoint> all = event->touchPoints();
    for (int i = 0; i < all.size(); ++i)
        if (all.at(i).state() != Qt::TouchPointReleased)
            live.append(all.at(i));

    if (live.size() >= 2) {
        const QPointF a = live.at(0).pos();
        const QPointF b = live.at(1).pos();
        const double d = ::hypot(b.x() - a.x(), b.y() - a.y());
        if (!m_pinching) {
            m_pinching = true;
            m_pinchBaseDistance = d;
            m_pinchStartScale = m_viewScale;
            m_pinchStartOrigin = m_viewOrigin;
            m_pinchStartCentre = (a + b) / 2.0;
            // End the drag the first finger had started, or WebKit keeps
            // scrolling the page underneath the pinch.
            if (m_touchDown) {
                const QPointF ua = toSurface(a);
                emit touched("up", 0, int(ua.x()), int(ua.y()));
                m_touchDown = false;
            }
        } else if (m_pinchBaseDistance > 8.0) {   // ignore a degenerate start
            double scale = m_pinchStartScale * (d / m_pinchBaseDistance);
            if (scale < kMinViewScale) scale = kMinViewScale;
            if (scale > kMaxViewScale) scale = kMaxViewScale;

            // Keep the content under the fingers under the fingers. Everything
            // is in normalised texture coordinates, so this survives the engine
            // re-rendering the surface at a different pixel size.
            const double w = width();
            const QPointF centre = (a + b) / 2.0;
            const QPointF anchor(m_pinchStartOrigin.x()
                                 + (m_pinchStartCentre.x() / w) / m_pinchStartScale,
                                 m_pinchStartOrigin.y()
                                 + vFraction(m_pinchStartCentre.y()) / m_pinchStartScale);
            QPointF origin(anchor.x() - (centre.x() / w) / scale,
                           anchor.y() - vFraction(centre.y()) / scale);

            // Never sample outside the surface. The visible window is shorter
            // vertically than horizontally by whatever the bar covers, so the
            // two limits are not the same number.
            const double maxU = 1.0 - 1.0 / scale;
            const double maxV = 1.0 - vFraction(pageBottom()) / scale;
            if (origin.x() < 0) origin.setX(0);
            if (origin.y() < 0) origin.setY(0);
            if (origin.x() > maxU) origin.setX(maxU);
            if (origin.y() > maxV) origin.setY(maxV);

            m_viewScale = scale;
            m_viewOrigin = origin;
            update();
        }
        return;
    }

    if (m_pinching) {
        // Hold off until every finger is up: resuming a drag from the finger
        // left over from a pinch yanks the page.
        if (live.isEmpty()) {
            m_pinching = false;
            // Now ask the engine to actually RENDER at this magnification. Until
            // that lands we are showing a stretched older surface; afterwards
            // the same normalised window maps 1:1 and the text is sharp. The
            // CSS viewport is unchanged either way, so nothing reflows.
            emit scaleRequested(m_viewScale);
        }
        return;
    }

    if (live.isEmpty()) {
        if (m_touchDown) {
            const QPointF p = toSurface(all.isEmpty() ? QPointF() : all.last().pos());
            emit touched("up", 0, int(p.x()), int(p.y()));
            m_touchDown = false;
        }
        return;
    }

    const QPointF p = toSurface(live.first().pos());
    if (!m_touchDown) {
        // Started on the chrome, not on the page.
        if (!inPage(live.first().pos()))
            return;
        m_touchDown = true;
        emit touched("down", 0, int(p.x()), int(p.y()));
    } else {
        emit touched("motion", 0, int(p.x()), int(p.y()));
    }
}
