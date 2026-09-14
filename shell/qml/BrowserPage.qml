import QtQuick 1.1
import com.nokia.meego 1.0
import com.nokia.extras 1.1
import com.webmee 1.0
Page {
    id: page
    // Harmattan metrics from the platform: 72px header portrait, 16px margin.
    property int headerHeight: 72
    property int margin: 16
    function navigate(url) { engine.load(url) }

    // Both halves of the chrome hide together. main.qml drives the platform
    // toolbar off this; the URL bar is ours.
    property alias chromeHidden: urlBar.hidden

    // The platform toolbar's LIVE height -- it animates to 0 as it hides, and
    // PageStackWindow takes exactly that much off the content area while it
    // does. The web view adds it straight back as a negative bottom margin, so
    // its own height never changes and the surface is never resized.
    property real toolBarHeight: (pageStack && pageStack.toolBar) ? pageStack.toolBar.height : 0

    // The platform's own toolbar transition is 500 ms; the URL bar's is 180.
    // Give the toolbar ours so the two move as one piece of chrome. Every other
    // ToolBarStyle default is inherited.
    ToolBarStyle { id: chromeStyle; visibilityTransitionDuration: 180 }

    onStatusChanged: {
        if (status === PageStatus.Active && pageStack && pageStack.toolBar)
            pageStack.toolBar.platformStyle = chromeStyle;
        // Another page is coming up and needs the toolbar, whatever we left it
        // at. It is the window's, not ours.
        if (status !== PageStatus.Active)
            urlBar.hidden = false;
    }

    // Lift the caret clear of the on-screen keyboard. WebKit has no idea the
    // keyboard exists, so the shell measures the overlap and asks the page to
    // scroll. Everything is converted to CSS pixels, because that is the only
    // unit both sides agree on.
    function ensureCaretVisible() {
        if (webViewArea.width <= 0 || engine.caretH <= 0)
            return;
        var panel = inputContext ? inputContext.softwareInputPanelRect : null;
        if (!panel || panel.height <= 0)
            return;

        // Where the keyboard starts, in item pixels.
        var keyboardTopInItem = panel.y - webViewArea.mapToItem(null, 0, 0).y;
        var cssPerItemPx = engine.layoutWidth / webViewArea.width;
        // The page's first row is drawn at topInset, not at the item's top.
        var visibleBottomCss = (keyboardTopInItem - webViewArea.topInset) * cssPerItemPx;
        // The caret is reported relative to the viewport, not the document.
        var caretBottomCss = engine.caretY + engine.caretH;

        var overlap = caretBottomCss - visibleBottomCss + 16;   // a little margin
        if (overlap > 0)
            engine.scrollBy(0, Math.round(overlap));
    }

    Connections {
        target: engine
        // The panel animates open, so its rectangle is not final immediately.
        onInputFocusChanged: if (focused) caretTimer.restart()
        onCaretChanged: if (caretTimer.running) caretTimer.restart()
    }
    Timer {
        id: caretTimer
        interval: 400
        onTriggered: page.ensureCaretVisible()
    }
    function showTab(index) { appWindow.currentTab = index; engine.load(tabModel.get(index).tabUrl) }
    // ---- URL bar ----------------------------------------------------------
    // The stock browser slides this away as you scroll down and brings it back
    // on scroll up. Driven off engine.scrollY once the engine reports it; until
    // then it simply stays put.
    Item {
        id: urlBar
        anchors { left: parent.left; right: parent.right }
        height: page.headerHeight
        z: 5

        // Hidden while scrolling down, back on the way up -- as the stock
        // browser did. Never hidden near the top of the page, and never while
        // the address field has focus.
        property bool hidden: false
        y: hidden ? -height : 0
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.InOutQuad } }

        property int lastY: 0
        Connections {
            target: engine
            onScrollChanged: {
                var y = engine.scrollY;
                if (y < 24 || urlField.activeFocus)
                    urlBar.hidden = false;
                else if (y > urlBar.lastY + 8)
                    urlBar.hidden = true;
                else if (y < urlBar.lastY - 8)
                    urlBar.hidden = false;
                urlBar.lastY = y;
            }
        }
        Rectangle {
            anchors.fill: parent
            color: theme.inverted ? "#1c1c1c" : "#e0e0e0"
        }
        TextField {
            id: urlField
            anchors {
                left: parent.left; leftMargin: page.margin
                right: starButton.left; rightMargin: 8
                verticalCenter: parent.verticalCenter
            }
            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
            placeholderText: qsTr("Search or enter address")
            // Show the live URL unless the user is editing it.
            text: engine.url
            onActiveFocusChanged: {
                if (activeFocus) { selectAllTimer.restart(); suggestions.refresh() }
                else suggestions.hide()
            }

            // Selecting inline does not survive: TextField's internal TextInput
            // has activeFocusOnPress, so the press positions the cursor after
            // this handler runs, and the software input panel then fires its own
            // repositionTimer. Both clear the selection. Wait for that to settle.
            Timer {
                id: selectAllTimer
                interval: 120
                onTriggered: if (urlField.activeFocus) urlField.selectAll()
            }
            onTextChanged: if (activeFocus) suggestions.refresh()
            Keys.onReturnPressed: { suggestions.accept(text) }

        }
        ToolIcon {
            id: stopReload
            anchors { right: parent.right; rightMargin: 4; verticalCenter: parent.verticalCenter }
            platformIconId: engine.loading ? "toolbar-cancle" : "toolbar-refresh"
            onClicked: engine.loading ? engine.stop() : engine.reload()
        }

        // Favourite toggle. Bound to bookmarks.bookmarksChanged rather than a
        // property, because whether THIS url is starred can change from the
        // favourites page too.
        Item {
            id: starButton
            anchors { right: stopReload.left; rightMargin: 2; verticalCenter: parent.verticalCenter }
            width: 56; height: 56

            property bool marked: false
            function refresh() { marked = bookmarks.contains(engine.url) }

            Component.onCompleted: refresh()
            Connections { target: engine; onUrlChanged: starButton.refresh() }
            Connections { target: bookmarks; onBookmarksChanged: starButton.refresh() }

            Image {
                anchors.centerIn: parent
                source: "image://theme/icon-m-common-favorite-"
                        + (starButton.marked ? "mark" : "unmark")
                        + (theme.inverted ? "-inverse" : "")
                opacity: engine.url === "" || engine.url === "about:blank" ? 0.3 : 1
            }

            MouseArea {
                anchors.fill: parent
                enabled: engine.url !== "" && engine.url !== "about:blank"
                onClicked: {
                    var nowMarked = bookmarks.toggle(engine.url, engine.title);
                    starBanner.text = nowMarked ? qsTr("Added to favourites")
                                                : qsTr("Removed from favourites");
                    starBanner.show();
                }
            }
        }
        // Load progress, as a hairline under the URL bar.
        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom }
            height: 4
            width: parent.width * engine.loadProgress
            color: "#4d90fe"
            // Zero-opacity items still hit-test in QML1, so pair the two.
            opacity: engine.loading ? 1 : 0
            visible: opacity != 0
            Behavior on width { NumberAnimation { duration: 120 } }
            Behavior on opacity { NumberAnimation { duration: 200 } }
        }
    }
    // ---- Page text input -------------------------------------------------
    // The page has no keyboard of its own: WebKit reports that it focused an
    // editable, and this off-screen field is what gives the platform something
    // to attach the on-screen keyboard to. What is typed here is forwarded to
    // the engine and committed into the page's field.
    TextInput {
        id: pageInput
        width: 1; height: 1
        opacity: 0
        // Not visible:false -- an invisible item cannot take focus, and without
        // focus the software input panel has nothing to open for.
        activeFocusOnPress: false
        inputMethodHints: Qt.ImhNoPredictiveText

        Connections {
            target: engine
            onInputFocusChanged: {
                if (focused) {
                    pageInput.forceActiveFocus();
                    pageInput.openSoftwareInputPanel();
                } else {
                    pageInput.closeSoftwareInputPanel();
                    pageInput.focus = false;
                }
            }
        }

        // Printable characters go in as text; editing keys as keysyms, because
        // a committed string cannot express "delete backwards".
        Keys.onPressed: {
            if (event.key === Qt.Key_Backspace)      engine.sendKey(0xFF08, true);
            else if (event.key === Qt.Key_Return
                     || event.key === Qt.Key_Enter)  engine.sendKey(0xFF0D, true);
            else if (event.key === Qt.Key_Left)      engine.sendKey(0xFF51, true);
            else if (event.key === Qt.Key_Right)     engine.sendKey(0xFF53, true);
            else if (event.key === Qt.Key_Up)        engine.sendKey(0xFF52, true);
            else if (event.key === Qt.Key_Down)      engine.sendKey(0xFF54, true);
            else if (event.text.length > 0)          engine.sendText(event.text);
            else return;
            event.accepted = true;
        }
        Keys.onReleased: {
            if (event.key === Qt.Key_Backspace)      engine.sendKey(0xFF08, false);
            else if (event.key === Qt.Key_Return
                     || event.key === Qt.Key_Enter)  engine.sendKey(0xFF0D, false);
            event.accepted = true;
        }

        // The field never holds anything itself; it is only an input target.
        onTextChanged: if (text.length > 0) { engine.sendText(text); text = "" }
    }

    // ---- Address bar suggestions -----------------------------------------
    // History matches, plus an explicit row for what a search would actually
    // do. The stock browser hid searches behind the address bar; naming the
    // engine means a search is never a silent redirect to somewhere unexpected.
    Rectangle {
        id: suggestions
        anchors { top: urlBar.bottom; left: parent.left; right: parent.right }
        height: Math.min(visibleRows * rowHeight, page.height - urlBar.height - 40)
        color: theme.inverted ? "#101010" : "#f4f4f4"
        visible: opacity > 0
        opacity: 0
        z: 10

        property int rowHeight: 68
        property variant matches: []
        property bool searching: !engine.looksLikeUrl(urlField.text) && urlField.text.length > 0
        property int visibleRows: matches.length + (searching ? 1 : 0)

        Behavior on opacity { NumberAnimation { duration: 120 } }

        function refresh() {
            matches = history.suggest(urlField.text, 5);
            opacity = visibleRows > 0 ? 1 : 0;
        }
        function hide() { opacity = 0 }
        function go(url) {
            hide();
            urlField.focus = false;
            // A method of the TextField, not a global: calling it bare from
            // here threw a ReferenceError and silently abandoned the rest of
            // the function, so nothing navigated.
            urlField.platformCloseSoftwareInputPanel();
            engine.load(url);
        }
        // Enter follows the same rule the list shows, so what you see is what
        // you get rather than two different guesses.
        function accept(text) {
            hide();
            urlField.focus = false;
            urlField.platformCloseSoftwareInputPanel();
            page.navigate(text);
        }

        Column {
            anchors.fill: parent

            Repeater {
                model: suggestions.matches
                delegate: Item {
                    width: suggestions.width
                    height: suggestions.rowHeight
                    Column {
                        anchors { left: parent.left; right: parent.right
                                  leftMargin: page.margin; rightMargin: page.margin
                                  verticalCenter: parent.verticalCenter }
                        Label {
                            text: modelData.title
                            font.pixelSize: 22
                            elide: Text.ElideRight
                            width: parent.width
                        }
                        Label {
                            text: modelData.url
                            font.pixelSize: 16
                            color: "#888888"
                            elide: Text.ElideRight
                            width: parent.width
                        }
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: suggestions.go(modelData.url)
                    }
                    Rectangle {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                        height: 1
                        color: theme.inverted ? "#282828" : "#dddddd"
                    }
                }
            }

            Item {
                width: suggestions.width
                height: suggestions.searching ? suggestions.rowHeight : 0
                visible: suggestions.searching
                Column {
                    anchors { left: parent.left; right: parent.right
                              leftMargin: page.margin; rightMargin: page.margin
                              verticalCenter: parent.verticalCenter }
                    Label {
                        text: qsTr("Search %1 for \u201c%2\u201d")
                                .arg(engine.searchEngine).arg(urlField.text)
                        font.pixelSize: 22
                        elide: Text.ElideRight
                        width: parent.width
                    }
                    Label {
                        text: engine.searchUrlFor(urlField.text)
                        font.pixelSize: 16
                        color: "#888888"
                        elide: Text.ElideRight
                        width: parent.width
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: suggestions.go(engine.searchUrlFor(urlField.text))
                }
            }
        }
    }

    // ---- Web view area ----------------------------------------------------
    // The page itself. WebView is a C++ QDeclarativeItem that draws the engine
    // process's X window as a GLES2 texture, so it rotates with the rest of the
    // UI like any other item. objectName is how main.cpp wires it to the engine.
    WebView {
        id: webViewArea
        objectName: "webViewArea"
        // Full height, ALWAYS. Anchoring under the URL bar would resize the
        // surface every time the bar hides or shows, and every resize relayouts
        // the page -- far too expensive here.
        // The negative margin extends this item down over the toolbar, by
        // exactly what PageStackWindow took off the content area for it -- so
        // the two cancel and the height is constant whatever the toolbar does.
        anchors { top: parent.top; left: parent.left; right: parent.right; bottom: parent.bottom
                  bottomMargin: -page.toolBarHeight }
        // ...so the chrome's height comes out of what is DRAWN instead. The page
        // starts below the URL bar and stops above the toolbar rather than
        // running under either, which is what makes those rows tappable. Both
        // are bound to live values, so they follow the hide animations frame by
        // frame.
        topInset: urlBar.y + urlBar.height
        bottomInset: page.toolBarHeight
        // z: -1 puts this UNDER the parent's own painting. Children default to
        // drawing above their parent, which would hide the page entirely.
        Rectangle {
            z: -1
            anchors.fill: parent
            color: theme.inverted ? "#000000" : "#ffffff"
        }
        // ---- Start page / splash -------------------------------------------
        // One overlay, three states: the start page before anything is loaded,
        // a spinner while the engine comes up or a page loads, and a message if
        // the web process died. It disappears entirely once a page is painted.
        Rectangle {
            id: splash
            anchors.fill: parent
            color: theme.inverted ? "#000000" : "#ffffff"

            property bool blank: engine.url === "" || engine.url === "about:blank"
            property bool busy: !blank && !webViewArea.crashed && !webViewArea.painted
            visible: blank || webViewArea.crashed || !webViewArea.painted

            Column {
                anchors.centerIn: parent
                spacing: 16

                Image {
                    anchors.horizontalCenter: parent.horizontalCenter
                    source: "qrc:/icon/webmee192.png"
                    sourceSize.width: 128
                    sourceSize.height: 128
                    smooth: true
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "WebMee"
                    font.pixelSize: 44
                    font.weight: Font.Light
                    color: theme.inverted ? "#ffffff" : "#1c1c1c"
                }
                Label {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: webViewArea.crashed ? qsTr("The page stopped responding")
                                              : qsTr("Search or enter an address to begin")
                    visible: splash.blank || webViewArea.crashed
                    font.pixelSize: 18
                    color: "#888888"
                }
                // Below the logo and title, so it is never what covers them.
                BusyIndicator {
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: splash.busy
                    running: splash.busy
                    platformStyle: BusyIndicatorStyle { size: "medium" }
                }
                Button {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("Reload")
                    visible: webViewArea.crashed
                    onClicked: engine.reload()
                }
            }
        }

        // Shown for later page loads, once the engine is up.
        BusyIndicator {
            anchors.centerIn: parent
            running: webViewArea.painted && engine.loading && engine.loadProgress < 0.1
            visible: running
            platformStyle: BusyIndicatorStyle { size: "large" }
        }
    }
    // ---- Toolbar ----------------------------------------------------------
    tools: ToolBarLayout {
        ToolIcon {
            platformIconId: "toolbar-back"
            enabled: engine.canGoBack
            opacity: enabled ? 1 : 0.4
            onClicked: engine.goBack()
        }
        ToolIcon {
            platformIconId: "toolbar-next"
            enabled: engine.canGoForward
            opacity: enabled ? 1 : 0.4
            onClicked: engine.goForward()
        }
        ToolIcon {
            platformIconId: "toolbar-pages-all"
            onClicked: tabsSheet.open()
        }
        ToolIcon {
            platformIconId: "toolbar-view-menu"
            onClicked: mainMenu.open()
        }
    }
    InfoBanner {
        id: starBanner
        timerShowTime: 1500
    }

    Menu {
        id: mainMenu
        MenuLayout {
            MenuItem {
                text: qsTr("New tab")
                onClicked: appWindow.openTab("about:blank")
            }
            MenuItem {
                text: qsTr("Favourites")
                onClicked: pageStack.push(Qt.resolvedUrl("BookmarksPage.qml"))
            }
            MenuItem {
                text: downloads.activeCount > 0
                      ? qsTr("Downloads (%1)").arg(downloads.activeCount)
                      : qsTr("Downloads")
                onClicked: pageStack.push(Qt.resolvedUrl("DownloadsPage.qml"))
            }
            MenuItem {
                text: qsTr("Share")
                onClicked: engine.share()
            }
            MenuItem {
                text: qsTr("Settings")
                onClicked: pageStack.push(Qt.resolvedUrl("SettingsPage.qml"))
            }
        }
    }
}
