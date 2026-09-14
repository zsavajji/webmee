// Tab switcher, in the shape the stock browser uses: a grid of page thumbnails
// with a close affordance on each.
import QtQuick 1.1
import com.nokia.meego 1.0

Sheet {
    id: sheet

    property int selectedIndex: appWindow.currentTab

    acceptButtonText: qsTr("Open")
    rejectButtonText: qsTr("Cancel")

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
                    border.width: index === sheet.selectedIndex ? 3 : 1
                    border.color: index === sheet.selectedIndex ? "#4d90fe" : "#555555"
                    radius: 6

                    // Thumbnails come from the engine once it can snapshot a
                    // tab; the label stands in until then.
                    Image {
                        anchors { fill: parent; margins: 4; bottomMargin: 44 }
                        source: "image://tab/" + index
                        fillMode: Image.PreserveAspectCrop
                        clip: true
                        asynchronous: true
                    }

                    Text {
                        anchors { left: parent.left; right: parent.right; bottom: parent.bottom
                                  margins: 8 }
                        text: tabTitle
                        elide: Text.ElideRight
                        color: theme.inverted ? "#ffffff" : "#000000"
                        font.pixelSize: 20
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: sheet.selectedIndex = index
                    onDoubleClicked: { sheet.selectedIndex = index; sheet.accept() }
                }

                ToolIcon {
                    anchors { top: parent.top; right: parent.right }
                    platformIconId: "toolbar-close"
                    onClicked: {
                        if (tabModel.count > 1) {
                            tabModel.remove(index);
                            if (appWindow.currentTab >= tabModel.count)
                                appWindow.currentTab = tabModel.count - 1;
                        }
                    }
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
