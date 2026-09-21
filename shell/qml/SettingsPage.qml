import QtQuick 1.1
import com.nokia.meego 1.0

Page {
    id: page
    property int margin: 16

    // Settings are applied on the way out, not as they are toggled: changing the
    // layout width or render scale resizes the surface and relayouts the page,
    // so applying half a dozen in a row makes the browser crawl.
    tools: ToolBarLayout {
        ToolIcon {
            platformIconId: "toolbar-back"
            onClicked: { engine.commitSettings(); pageStack.pop() }
        }
    }

    // Backstop for any other way off this page.
    Component.onDestruction: engine.commitSettings()

    // The content is taller than the screen and gets taller every time a
    // setting is added. A bare Column does not scroll and does not clip, so the
    // last rows were simply off the bottom of the page with nothing to say so.
    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: content.height + 2 * page.margin
        flickableDirection: Flickable.VerticalFlick
        // Without this the overflow paints over the toolbar rather than ending
        // at the page edge.
        clip: true

        Column {
            id: content
            x: page.margin
            y: page.margin
            width: flick.width - 2 * page.margin
            spacing: 16

            Label { text: qsTr("Settings"); font.pixelSize: 32 }

            Label {
                text: qsTr("Changes apply when you go back")
                font.pixelSize: 16
                color: "#888888"
            }

            Row {
                spacing: 16
                Switch {
                    checked: engine.javascriptEnabled
                    onCheckedChanged: engine.javascriptEnabled = checked
                }
                Label { text: qsTr("JavaScript"); anchors.verticalCenter: parent.verticalCenter }
            }

            Row {
                spacing: 16
                Switch {
                    checked: engine.imagesEnabled
                    onCheckedChanged: engine.imagesEnabled = checked
                }
                Label { text: qsTr("Load images"); anchors.verticalCenter: parent.verticalCenter }
            }

            Row {
                spacing: 16
                Switch {
                    checked: engine.suggestionsEnabled
                    onCheckedChanged: engine.suggestionsEnabled = checked
                }
                Label {
                    text: qsTr("Address bar suggestions")
                    anchors.verticalCenter: parent.verticalCenter
                }
            }

            Label { text: qsTr("Page width"); font.pixelSize: 24 }

            // Wider lays out more CSS pixels in BOTH directions, so it is also the
            // fix for a dialog whose buttons fall below the fold. Narrower means
            // larger text.
            ButtonRow {
                checkedButton: engine.layoutWidth >= 480 ? w480
                             : (engine.layoutWidth >= 400 ? w400 : w345)
                Button { id: w345; text: qsTr("345"); onClicked: engine.layoutWidth = 345 }
                Button { id: w400; text: qsTr("400"); onClicked: engine.layoutWidth = 400 }
                Button { id: w480; text: qsTr("480"); onClicked: engine.layoutWidth = 480 }
            }

            Label {
                text: qsTr("Viewport: %1 x %2 CSS px")
                      .arg(engine.layoutWidth)
                      .arg(Math.round(engine.layoutWidth * 674 / 480))
                font.pixelSize: 18
                color: "#888888"
            }

            Label { text: qsTr("Rendering"); font.pixelSize: 24 }

            // The layout is 320px wide either way; this only decides how finely it
            // is rasterised. Fast renders 320x449 and upscales.
            ButtonRow {
                checkedButton: engine.renderScale > 1.2 ? sharpBtn : fastBtn
                Button { id: fastBtn;  text: qsTr("Fast");  onClicked: engine.renderScale = 1.0 }
                Button { id: sharpBtn; text: qsTr("Sharp"); onClicked: engine.renderScale = 1.5 }
            }

            Label { text: qsTr("Search engine"); font.pixelSize: 24 }

            // Explicit, and shown in the address bar before you commit to it.
            Button {
                text: qsTr("Search with: %1").arg(engine.searchEngine)
                onClicked: enginePicker.open()
            }

            Label {
                text: qsTr("User agent")
                font.pixelSize: 24
            }
            // A modern UA is often the difference between a usable page and an
            // "unsupported browser" interstitial, so make it switchable.
            ButtonRow {
                Button { text: qsTr("Mobile"); onClicked: engine.userAgentPreset = "mobile" }
                Button { text: qsTr("Desktop"); onClicked: engine.userAgentPreset = "desktop" }
            }

            Label { text: qsTr("Storage"); font.pixelSize: 24 }

            Label {
                // Usage only. It used to read "of 64 MB", and the 64 was a
                // fiction: WebKit's GLib API has no cache-size setter, so
                // nothing ever imposed that ceiling.
                text: qsTr("Cache: %1 MB").arg(engine.cacheUsageMb)
                font.pixelSize: 20
                color: "#999999"
            }

            Button {
                text: qsTr("Clear cache")
                onClicked: engine.clearCache()
            }

            Label {
                text: qsTr("History: %1 entries").arg(history.count)
                font.pixelSize: 20
                color: "#999999"
            }

            // Confirmed, unlike the cache: a cache rebuilds itself by browsing,
            // history does not come back.
            Button {
                text: qsTr("Clear history")
                enabled: history.count > 0
                opacity: enabled ? 1 : 0.4
                onClicked: clearHistoryConfirm.open()
            }

            // Only shown where signond is reachable -- where it is not, nothing
            // can have been saved, so a count of zero would just puzzle.
            Label {
                visible: passwords.available
                text: qsTr("Saved passwords: %1").arg(passwords.count)
                font.pixelSize: 20
                color: "#999999"
            }

            Button {
                visible: passwords.available
                text: qsTr("Forget saved passwords")
                enabled: passwords.count > 0
                opacity: enabled ? 1 : 0.4
                onClicked: forgetPasswordsConfirm.open()
            }

            Label {
                text: qsTr("Free memory: %1 MB").arg(memory.availableMb)
                font.pixelSize: 20
                color: memory.level === 0 ? "#999999" : (memory.level === 1 ? "#ddaa33" : "#ff5555")
            }
        }
    }

    ScrollDecorator { flickableItem: flick }

    QueryDialog {
        id: clearHistoryConfirm
        titleText: qsTr("Clear history")
        message: qsTr("Remove all %1 saved pages? This cannot be undone.")
                 .arg(history.count)
        acceptButtonText: qsTr("Clear")
        rejectButtonText: qsTr("Cancel")
        onAccepted: history.clear()
    }

    QueryDialog {
        id: forgetPasswordsConfirm
        titleText: qsTr("Forget saved passwords")
        message: qsTr("Remove the passwords saved for %1 site(s)? "
                      + "This cannot be undone.").arg(passwords.count)
        acceptButtonText: qsTr("Forget")
        rejectButtonText: qsTr("Cancel")
        onAccepted: {
            // hosts() hands back a copy, so removing as we walk it is safe.
            // The store has no clear-all of its own and does not need one for
            // a list this short.
            var h = passwords.hosts();
            for (var i = 0; i < h.length; ++i)
                passwords.forget(h[i]);
        }
    }

    // Outside the Column deliberately. Dialog takes its width from its parent
    // and centres itself in it, so as a Column child it would be laid out as
    // another row and centred on the scrolling content rather than the screen.
    SelectionDialog {
        id: enginePicker
        titleText: qsTr("Search engine")
        selectedIndex: engine.searchEngines.indexOf(engine.searchEngine)
        model: engine.searchEngines
        onAccepted: engine.searchEngine = engine.searchEngines[selectedIndex]
    }
}
