import QtQuick 1.1
import com.nokia.meego 1.0

Page {
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

    Column {
        anchors { top: parent.top; left: parent.left; right: parent.right; margins: 16 }
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

        SelectionDialog {
            id: enginePicker
            titleText: qsTr("Search engine")
            selectedIndex: engine.searchEngines.indexOf(engine.searchEngine)
            model: engine.searchEngines
            onAccepted: engine.searchEngine = engine.searchEngines[selectedIndex]
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
            text: qsTr("Cache: %1 MB of %2 MB").arg(engine.cacheUsageMb).arg(engine.diskCacheMb)
            font.pixelSize: 20
            color: "#999999"
        }

        Button {
            text: qsTr("Clear cache")
            onClicked: engine.clearCache()
        }

        Label {
            text: qsTr("Free memory: %1 MB").arg(memory.availableMb)
            font.pixelSize: 20
            color: memory.level === 0 ? "#999999" : (memory.level === 1 ? "#ddaa33" : "#ff5555")
        }
    }
}
