// Tab switcher: a grid of cards, one per tab, tap to go there.
//
// The stock browser showed page thumbnails. There is no cheap way to get one
// here -- the page lives in the engine's X window as a GL texture, so a
// thumbnail means glReadPixels off an SGX tiler (a full pipeline flush) plus a
// held QImage per tab, on the device where memory is the binding constraint.
// The cards carry the title and the address instead. What was here before was
// an Image on "image://tab/<index>", and no such provider was ever registered:
// an unknown provider renders nothing and says nothing, so the cards simply
// looked empty.
import QtQuick 1.1
import com.nokia.meego 1.0

Sheet {
    id: sheet

    // No accept button: a tap on a card is the whole interaction, so an "Open"
    // that acts on some separately-tracked selection is a second way to do the
    // same thing. Sheet hides the button when its text is empty.
    rejectButtonText: qsTr("Close")

    content: Item {
        anchors.fill: parent

        GridView {
            id: grid
            anchors.fill: parent
            anchors.margins: 16
            clip: true                       // required on every QML1 flickable
            cellWidth: 260
            cellHeight: 200
            model: tabModel

            delegate: Item {
                width: grid.cellWidth - 12
                height: grid.cellHeight - 12

                Rectangle {
                    id: frame
                    anchors.fill: parent
                    color: theme.inverted ? "#1c1c1c" : "#f0f0f0"
                    // Bound straight to currentTab rather than to a selection
                    // of the sheet's own: there is only one current tab, and
                    // keeping a second copy of that fact is what lets the two
                    // disagree.
                    border.width: index === appWindow.currentTab ? 3 : 1
                    border.color: index === appWindow.currentTab ? "#4d90fe" : "#555555"
                    radius: 6

                    Column {
                        anchors { left: parent.left; right: parent.right
                                  verticalCenter: parent.verticalCenter
                                  margins: 12 }
                        spacing: 6

                        Text {
                            width: parent.width
                            text: tabTitle
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                            color: theme.inverted ? "#ffffff" : "#000000"
                            font.pixelSize: 22
                        }
                        Text {
                            width: parent.width
                            // about:blank is what the browser calls an empty
                            // tab; it is not an address worth showing.
                            text: tabUrl === "about:blank" ? "" : tabUrl
                            elide: Text.ElideRight
                            color: "#8c8c8c"
                            font.pixelSize: 18
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: { appWindow.showTab(index); sheet.reject() }
                }

                ToolIcon {
                    anchors { top: parent.top; right: parent.right }
                    // Hidden on the last tab: closing it would leave the
                    // browser with no tab at all and nothing to show.
                    visible: tabModel.count > 1
                    platformIconId: "toolbar-close"
                    onClicked: appWindow.closeTab(index)
                }
            }
        }

        Text {
            anchors.centerIn: parent
            text: qsTr("No tabs")
            color: "#808080"
            font.pixelSize: 24
            opacity: tabModel.count === 0 ? 1 : 0
            visible: opacity != 0
        }
    }
}
