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

    // End an address-bar editing session.
    //
    // `urlField.focus = false` is the obvious way and does NOT work: the
    // platform TextField keeps activeFocus, and the suggestion list is bound to
    // exactly that, so it stayed open over the page that had just loaded. QML
    // focus has to GO somewhere -- handing it to the bar itself is enough. It
    // is a plain Item, it wants nothing, and it opens no input panel.
    function endUrlEditing() {
        urlBar.forceActiveFocus();
        // A method of the TextField, not a global: calling it bare from another
        // object's function throws a ReferenceError, which silently abandons
        // the rest of the handler.
        urlField.platformCloseSoftwareInputPanel();
    }

    // Both halves of the chrome hide together. main.qml drives the platform
    // toolbar off this; the URL bar is ours.
    property alias chromeHidden: urlBar.hidden

    // The platform toolbar's LIVE height -- it animates to 0 as it hides, and
    // PageStackWindow takes exactly that much off the content area while it
    // does. The web view adds it straight back as a negative bottom margin, so
    // its own height never changes and the surface is never resized.
    property real toolBarHeight: (pageStack && pageStack.toolBar) ? pageStack.toolBar.height : 0

    // Something is covering the page and expecting the taps. See the comment on
    // webViewArea.enabled for why this has to be said explicitly -- the menu
    // and the tab sheet have always had the same hole, they just happen to sit
    // over parts of the page where hitting a link is less likely.
    property bool modalUp: authDialog.status !== DialogStatus.Closed
                           || mainMenu.status !== DialogStatus.Closed
                           || linkMenu.status !== DialogStatus.Closed
                           || imageMenu.status !== DialogStatus.Closed
                           || tabsSheet.status !== DialogStatus.Closed

    // How much the platform has taken off the window for the on-screen keyboard.
    // Window.qml shrinks windowContent by exactly this and everything inside it
    // follows, which resizes the page area and relayouts the page -- the one
    // thing this design avoids everywhere else. The web view adds it back as a
    // negative margin, so the keyboard ends up floating over a page that never
    // changed size.
    //
    // The portrait branch is the platform's, not a guess: the panel rectangle is
    // in unrotated framebuffer coordinates, so its WIDTH is what covers the
    // bottom of a portrait screen. Read it from the same source and the same way
    // or the two will not cancel.
    property real sipHeight: (inputContext && inputContext.softwareInputPanelVisible)
                             ? (appWindow.inPortrait
                                ? inputContext.softwareInputPanelRect.width
                                : inputContext.softwareInputPanelRect.height)
                             : 0

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
        if (webViewArea.width <= 0 || engine.caretH <= 0 || page.sipHeight <= 0)
            return;

        // The web view now reaches the bottom of the window and the keyboard is
        // anchored there, so the keyboard's top edge is simply that much up from
        // the item's bottom -- no mapping between coordinate systems, and no
        // dependence on which way the panel rectangle is oriented.
        var cssPerItemPx = engine.layoutWidth / webViewArea.width;
        // The page's first row is drawn at topInset, not at the item's top.
        var visibleBottomCss = (webViewArea.height - page.sipHeight - webViewArea.topInset)
                               * cssPerItemPx;
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
            // Any navigation ends the address bar's editing session, including
            // ones that never touched it -- a link tap or a form submit moves no
            // focus by itself, so the field kept it and the suggestions stayed
            // up over the page that was loading.
            onLoadingChanged: if (engine.loading && urlField.activeFocus)
                page.endUrlEditing()
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
            // Show the live URL unless the user is editing it. about:blank is
            // how a blank tab is spelt internally, not an address anyone typed
            // -- showing it just means clearing it before you can type, so it
            // reads as empty and the placeholder does the talking.
            text: engine.url === "about:blank" ? "" : engine.url
            onActiveFocusChanged: if (activeFocus) {
                selectAllTimer.restart();
                suggestions.refresh();
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
        // NoAutoUppercase because this buffer is not the page's field and its
        // contents are not the page's contents -- so the keyboard's idea of
        // "start of a sentence" is derived from the wrong text. It cannot guess
        // right, so it should not guess. Shift still works.
        inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase

        Connections {
            target: engine
            onInputFocusChanged: {
                if (focused) {
                    pageInput.forceActiveFocus();
                    pageInput.openSoftwareInputPanel();
                } else {
                    pageInput.closeSoftwareInputPanel();
                    pageInput.focus = false;
                    // Only now, with nothing focused and no input method
                    // listening, is it safe to empty the buffer -- see below.
                    pageInput.text = "";
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

        // Send only what was appended, and do NOT clear between keystrokes.
        //
        // Clearing was the cause of both of this field's symptoms. Setting text
        // programmatically moves the cursor and calls updateMicroFocus(), which
        // is an IPC round trip to the input-method server -- once per letter, on
        // top of the one the insert already costs. That is the lag. And it hands
        // the server an EMPTY field, which is precisely the state in which a
        // keyboard turns auto-capitalisation back on: hence the shift flipping
        // up again after every single letter.
        //
        // The buffer therefore only grows, and is emptied on focus out.
        // Backspace never shrinks it -- Keys.onPressed consumes it and sends a
        // keysym to the page instead -- so the buffer is not a mirror of the
        // page's field and is not meant to be. It is a capture device, and only
        // the delta is ever forwarded.
        property string sentText: ""
        onTextChanged: {
            if (text.length > sentText.length
                    && text.substring(0, sentText.length) === sentText)
                engine.sendText(text.substring(sentText.length));
            // Anything else -- the clear on focus out, or the method rewriting
            // what it already committed -- resyncs without sending.
            sentText = text;
        }
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
        // Open if and only if the address bar has focus and there is something
        // to show. A BINDING, not an assignment: urlField.text is bound to
        // engine.url, so every navigation fires onTextChanged -> refresh(), and
        // an assigned opacity was therefore free to re-open the list behind the
        // user on a page they had already navigated to.
        opacity: engine.suggestionsEnabled && urlField.activeFocus && visibleRows > 0 ? 1 : 0
        z: 10

        property int rowHeight: 68
        property variant matches: []
        property bool searching: !engine.looksLikeUrl(urlField.text) && urlField.text.length > 0
        property int visibleRows: matches.length + (searching ? 1 : 0)

        Behavior on opacity { NumberAnimation { duration: 120 } }

        // Only the contents: whether the list is SHOWN is the binding above,
        // and assigning opacity here would quietly break it.
        function refresh() {
            matches = history.suggest(urlField.text, 5);
        }
        function go(url) {
            page.endUrlEditing();
            engine.load(url);
        }
        // Enter follows the same rule the list shows, so what you see is what
        // you get rather than two different guesses.
        function accept(text) {
            page.endUrlEditing();
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
                  bottomMargin: -(page.toolBarHeight + page.sipHeight) }
        // ...so the chrome's height comes out of what is DRAWN instead. The page
        // starts below the URL bar and stops above the toolbar rather than
        // running under either, which is what makes those rows tappable. Both
        // are bound to live values, so they follow the hide animations frame by
        // frame.
        topInset: urlBar.y + urlBar.height
        bottomInset: page.toolBarHeight
        // A popup over the page does NOT stop touches reaching it. Only this
        // item sets acceptTouchEvents, so QGraphicsScene delivers a touch
        // through every plain QML item above it -- the same fall-through the
        // URL bar needed inPage() for, except a dialog is in the middle of the
        // screen where inPage() cannot help. Tapping a field in the prompt
        // would otherwise also tap whatever the page has underneath it. A
        // disabled item receives no events at all, and enabled has no effect
        // on painting, so the page keeps rendering behind the dialog.
        enabled: !page.modalUp
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

    // ---- HTTP authentication ----------------------------------------------
    // The engine parks the challenge and loads nothing until it gets exactly
    // one answer, so every way out of this dialog has to send one. accepted
    // and rejected cover all of them: the two buttons, and the tap-outside
    // dismissal the platform's Dialog wires to reject().
    Dialog {
        id: authDialog
        objectName: "authDialog"        // findChild handle, as webViewArea has
        property string where: ""
        property string realm: ""
        property bool retry: false

        // A saved credential is being fetched for `where`, and the prompt is
        // held back until that answers -- a remembered site should not flash a
        // dialog on its way to loading.
        property bool awaiting: false

        // Every challenge arrives here. A saved password answers it without the
        // prompt ever opening; everything else falls through to ask().
        // What was last submitted for `where`, so a retry can start from it.
        // Not the text left in the field: an unrelated prompt in between
        // overwrites that, and offering one site's username for another is
        // both confusing and a small leak.
        property string lastUser: ""

        function begin(host, area, isRetry) {
            if (host !== where)
                lastUser = "";
            where = host;
            realm = area;
            retry = isRetry;
            // A retry means whatever was last sent for this host was rejected,
            // whoever it came from. Keeping it would auto-fill the same wrong
            // password on the next visit and prompt one round trip later every
            // time; dropping it makes the store self-correcting.
            if (isRetry)
                passwords.forget(host);
            if (!isRetry && passwords.available && passwords.hasCredentials(host)) {
                awaiting = true;
                lookupTimer.restart();
                passwords.requestCredentials(host);
                return;
            }
            ask();
        }

        function ask() {
            awaiting = false;
            lookupTimer.stop();
            // The username is not the secret, and the store keeps it exactly so
            // a prompt need not start from nothing. A retry starts from what
            // was last tried instead -- far more often a mistyped password than
            // the wrong account. The password field was emptied by finish().
            userField.text = retry ? lastUser : passwords.usernameFor(where);
            open();
        }

        title: Item {
            width: authDialog.width
            height: authTitle.height + 24
            Label {
                id: authTitle
                anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
                horizontalAlignment: Text.AlignHCenter
                font.pixelSize: 32
                font.bold: true
                color: "white"
                text: authDialog.retry ? qsTr("Sign in failed") : qsTr("Sign in")
            }
        }

        content: Item {
            width: authDialog.width
            height: authColumn.height + 42
            Column {
                id: authColumn
                y: 21
                anchors { left: parent.left; right: parent.right }
                spacing: 12
                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    font.pixelSize: 22
                    color: "white"
                    // The realm is the server's own name for the protected
                    // area and is often the only clue which login is wanted.
                    text: authDialog.retry
                          ? qsTr("Wrong username or password for %1").arg(authDialog.where)
                          : (authDialog.realm === ""
                             ? qsTr("%1 requires a username and password").arg(authDialog.where)
                             : qsTr("%1 requires a username and password for \"%2\"")
                               .arg(authDialog.where).arg(authDialog.realm))
                }
                TextField {
                    id: userField
                    objectName: "userField"
                    width: parent.width
                    placeholderText: qsTr("Username")
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    Keys.onReturnPressed: passField.forceActiveFocus()
                }
                TextField {
                    id: passField
                    objectName: "passField"
                    width: parent.width
                    placeholderText: qsTr("Password")
                    echoMode: TextInput.Password
                    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    Keys.onReturnPressed: authDialog.accept()
                }
                CheckBox {
                    id: rememberBox
                    // Hidden, not disabled, when signond cannot be reached:
                    // an offer that cannot be kept is worse than no offer.
                    visible: passwords.available
                    text: qsTr("Remember password")
                    checked: true
                }
            }
        }

        buttons: Item {
            width: authDialog.width
            height: authButtons.height + 38
            Column {
                id: authButtons
                y: 38
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 8
                Button {
                    text: qsTr("Sign in")
                    __dialogButton: true
                    platformStyle: ButtonStyle { inverted: true }
                    onClicked: authDialog.accept()
                }
                Button {
                    text: qsTr("Cancel")
                    __dialogButton: true
                    platformStyle: ButtonStyle { inverted: true }
                    onClicked: authDialog.reject()
                }
            }
        }

        // The panel has to be closed by hand -- the dialog goes away with the
        // keyboard still up otherwise, since nothing else takes the focus
        // back. And the password does not outlive the prompt that asked for
        // it: WebKit has its own copy by now, and a retry re-asks for it.
        function finish() {
            userField.platformCloseSoftwareInputPanel();
            passField.platformCloseSoftwareInputPanel();
            passField.text = "";
        }
        onAccepted: {
            // The page is waiting on this, so answer first and file the
            // password after. Saved before it is known to be right, because
            // nothing tells us it was -- a wrong one comes back as a retry,
            // which forgets it again.
            lastUser = userField.text;
            engine.authenticate(userField.text, passField.text);
            // passwords.available, NOT rememberBox.visible: in QtQuick 1
            // Item.visible is EFFECTIVE visibility, so a child of the dialog
            // reads false whenever the dialog itself is hidden -- which is
            // exactly what it is by the time this runs. The condition wanted
            // here is "the store works and the user asked", anyway.
            if (passwords.available && rememberBox.checked)
                passwords.save(where, userField.text, passField.text);
            finish();
        }
        onRejected: { engine.cancelAuthentication(); finish() }
    }

    Timer {
        id: lookupTimer
        interval: 4000
        // signond is a D-Bus service on its own schedule. Its failure paths do
        // answer immediately, but a wedged one would otherwise leave the page
        // waiting behind a prompt that never opens.
        onTriggered: if (authDialog.awaiting) authDialog.ask()
    }

    Connections {
        target: passwords
        onCredentialsFound: {
            // `host` is the store's normalised key, not what the server said,
            // so compare like with like. A second challenge can arrive while
            // this lookup is in flight -- the engine answers the first with
            // nothing and moves on -- and handing one site's password to
            // another is the one failure here that matters.
            if (!authDialog.awaiting || host !== passwords.hostKey(authDialog.where))
                return;
            authDialog.awaiting = false;
            lookupTimer.stop();
            // Kept for the prompt a rejection would bring up.
            authDialog.lastUser = username;
            engine.authenticate(username, password);
        }
        onCredentialsNotFound: {
            if (authDialog.awaiting && host === passwords.hostKey(authDialog.where))
                authDialog.ask();
        }
    }

    Connections {
        target: engine
        onAuthenticationRequested: authDialog.begin(where, realm, retry)
        // The challenge died with the process, so there is nothing left to
        // answer -- including a lookup still in flight for it. close() rather
        // than reject(): rejecting would send an authcancel for a request the
        // engine has already dropped.
        onWebProcessCrashed: { authDialog.awaiting = false; authDialog.close() }
    }

    // ---- Long press on a link ---------------------------------------------
    // The platform Menu, which is the slide-up panel the stock browser uses for
    // this -- not ContextMenu, which anchors itself to an item and is the wrong
    // shape. The engine reports the link under the finger (WPE turns a press
    // held past 500 ms into a context-menu request); everything here works off
    // that url alone, which is why none of it needs the engine.
    Menu {
        id: linkMenu
        property string url: ""

        MenuLayout {
            MenuItem {
                text: qsTr("Open in a new tab")
                onClicked: appWindow.openTab(linkMenu.url)
            }
            MenuItem {
                text: qsTr("Share link")
                onClicked: engine.shareLink(linkMenu.url)
            }
            MenuItem {
                text: qsTr("Copy link")
                onClicked: {
                    engine.copyToClipboard(linkMenu.url);
                    linkBanner.text = qsTr("Link copied");
                    linkBanner.show();
                }
            }
        }
    }

    // Long press on an image. Saving goes through the engine for the same
    // reason downloads do: Qt 4.7.4 cannot fetch https at all.
    Menu {
        id: imageMenu
        property string url: ""

        MenuLayout {
            MenuItem {
                text: qsTr("Save image")
                onClicked: {
                    engine.saveLink(imageMenu.url);
                    linkBanner.text = qsTr("Saving to Downloads");
                    linkBanner.show();
                }
            }
            MenuItem {
                text: qsTr("Open in a new tab")
                onClicked: appWindow.openTab(imageMenu.url)
            }
            MenuItem {
                text: qsTr("Share picture")
                onClicked: engine.shareLink(imageMenu.url)
            }
            MenuItem {
                text: qsTr("Copy image address")
                onClicked: {
                    engine.copyToClipboard(imageMenu.url);
                    linkBanner.text = qsTr("Link copied");
                    linkBanner.show();
                }
            }
        }
    }

    InfoBanner {
        id: linkBanner
        timerShowTime: 1500
    }

    Connections {
        target: engine
        onLinkLongPressed: { linkMenu.url = url; linkMenu.open() }
        onImageLongPressed: { imageMenu.url = url; imageMenu.open() }
        // The Downloads page is where progress lives; this is just so a
        // transfer that starts from a menu is not silent.
        onDownloadFinished: { linkBanner.text = qsTr("Download finished"); linkBanner.show() }
        onDownloadFailed: { linkBanner.text = qsTr("Download failed"); linkBanner.show() }
    }

    Menu {
        id: mainMenu
        MenuLayout {
            MenuItem {
                text: qsTr("New tab")
                // Land on the address bar, focused. A blank page with nothing
                // asking for input is where the old "New tab" left you, and
                // there was no obvious next move from there.
                onClicked: { appWindow.openTab("about:blank"); urlField.forceActiveFocus() }
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
