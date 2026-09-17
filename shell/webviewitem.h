#ifndef WEBVIEWITEM_H
#define WEBVIEWITEM_H

// The page, as a QML item.
//
// The engine is a separate process and paints through EGL/GLES2 into its own X
// window. An X window cannot be rotated, so parenting it into the shell's
// window left the page stuck in landscape while Qt drew the chrome in portrait.
// Instead the shell redirects that window with XComposite, wraps its backing
// pixmap in an EGLImage, and samples it as a GLES2 texture -- so the page is
// ordinary QML content and Qt's own transform rotates it along with everything
// else. Zero copies: the engine still renders straight to the GPU.
//
// Measured on device (probe/tfpprobe.c): EGL_KHR_image_pixmap and
// GL_OES_EGL_image are present, the texture tracks the window WITHOUT rebinding
// after each frame, and the pixmap's origin is TOP-left -- so the item's
// top-left corner samples v=0.

#include <QDeclarativeItem>
#include <QSize>
#include <QPointF>
#include <QTimer>

class QTouchEvent;

class WebViewItem : public QDeclarativeItem
{
    Q_OBJECT
    // False until the engine has rendered its first frame. QML keeps a splash
    // over the page area while it is false -- see paint() for why drawing
    // before then shows garbage rather than nothing.
    Q_PROPERTY(bool painted READ painted NOTIFY paintedChanged)
    // True after the web process died: the surface it was drawing into is gone
    // and whatever is left in that pixmap is not ours to show.
    Q_PROPERTY(bool crashed READ crashed NOTIFY paintedChanged)
    // Pixels at the top of the item that the URL bar floats over. The page is
    // DRAWN below them instead of under them, so the bar hides nothing and taps
    // land where they look. The item's geometry is untouched, so the surface
    // never resizes and the page never relayouts.
    Q_PROPERTY(int topInset READ topInset WRITE setTopInset)
    // Same at the other end, for the platform toolbar. PageStackWindow takes
    // the toolbar's height off the content area as it animates, so the item
    // cancels that with a negative bottom margin and keeps a constant height --
    // the chrome hiding must not resize the surface.
    Q_PROPERTY(int bottomInset READ bottomInset WRITE setBottomInset)
public:
    int topInset() const { return m_topInset; }
    void setTopInset(int inset);
    int bottomInset() const { return m_bottomInset; }
    void setBottomInset(int inset);
    bool painted() const { return m_sawFrame; }
    bool crashed() const { return m_crashed; }
    explicit WebViewItem(QDeclarativeItem *parent = 0);
    ~WebViewItem();

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget);

public slots:
    // The engine reports its window id once, when its EGL target is created.
    void setEngineWindow(qulonglong window);
    // The engine re-rendered at a new surface size; the old pixmap is stale.
    void setSurfaceSize(const QSize &size);
    // One frame finished rendering; the texture already shows it, we only need
    // to repaint the scene.
    void frameReady();
    // The web process is gone; stop showing its last surface.
    void pageCrashed();

public:
    int verifyRemaining() const;

signals:
    // How big the engine's window (and WebKit's viewport) should be, in pixels.
    void surfaceSizeChanged(const QSize &size);
    // Magnification to render at. The engine raises device size and scale
    // factor together, so the CSS viewport -- and the layout -- do not change.
    void scaleRequested(double magnification);
    // A touchpoint, in PAGE coordinates. Item coordinates already are page
    // coordinates -- Qt's rotation lives in the transform, not in the values
    // QGraphicsScene hands a mouse event -- so nothing has to be un-rotated.
    void touched(const QString &kind, int slot, int x, int y);
    void paintedChanged();

private slots:
    // Geometry settles before the engine hears about it -- see the timer.
    void commitSurfaceSize();

protected:
    void geometryChanged(const QRectF &newGeometry, const QRectF &oldGeometry);
    void mousePressEvent(QGraphicsSceneMouseEvent *event);
    void mouseMoveEvent(QGraphicsSceneMouseEvent *event);
    void mouseReleaseEvent(QGraphicsSceneMouseEvent *event);
    bool sceneEvent(QEvent *event);

private:
    void handleTouch(QTouchEvent *event);
    void pressAt(const QPointF &itemPoint);
    // Slide the window into the surface sideways by a drag of dxItem item
    // pixels, so the content under the finger stays under the finger.
    void panBy(double dxItem);
    QPointF toSurface(const QPointF &itemPoint) const;
    double vFraction(double itemY) const;
    double pageBottom() const;
    bool inPage(const QPointF &itemPoint) const;
    bool ensureTexture();
    bool acquirePixmap();
    void releasePixmap();
    bool buildProgram();

    unsigned long m_window;      // engine's X window (Window)
    unsigned long m_pixmap;      // its redirected backing pixmap (Pixmap)
    void *m_image;               // EGLImageKHR
    unsigned int m_texture;      // GLuint
    unsigned int m_program;      // GLuint
    int m_attrPos;
    int m_attrTex;
    bool m_programFailed;
    bool m_sawFrame;
    bool m_crashed;
    int m_topInset;              // item px the URL bar covers at the top
    int m_bottomInset;           // item px the platform toolbar covers
    bool m_haveTouch;            // real touch arrived; ignore synthesised mice
    bool m_touchDown;
    // While magnified a one-finger drag has two possible meanings -- pan the
    // window into the surface, or scroll the page -- so it is recognised
    // before anything is forwarded. Unmagnified there is only ever the page,
    // and the gesture starts in DragPage.
    enum DragKind { DragUndecided, DragPage, DragPan };
    DragKind m_drag;
    QPointF m_touchStart;        // item coords of the press
    QPointF m_panLast;           // item coords the pan last consumed
    bool m_pinching;
    double m_pinchBaseDistance;
    double m_pinchBaseZoom;
    // The pinch transform, applied to the texture only: item point p is drawn
    // at p * m_viewScale + m_viewOffset. WebKit never hears about it, so the
    // page is magnified rather than re-laid-out.
    // m_viewScale is the magnification; m_viewOrigin is the top-left of the
    // visible window into the surface, in NORMALISED texture coordinates, so
    // it stays correct when the engine re-renders the surface at a new size.
    double m_viewScale;
    QPointF m_viewOrigin;
    QSize m_surfaceSize;         // what the engine says it is rendering
    double m_pinchStartScale;
    QPointF m_pinchStartOrigin;
    QPointF m_pinchStartCentre;
    QTimer m_resizeTimer;
    QSize m_requestedSize;
    bool m_pendingSurface;
    int m_rejected;              // consecutive size-mismatched pixmaps       // resize in flight; old texture still shown
    int m_verifyLeft;   // N9_VERIFY_PAGE: how many more paints to sample
    QSize m_pixmapSize;          // size the pixmap actually has
    QSize m_wantSize;            // size we last asked the engine for
};

#endif
