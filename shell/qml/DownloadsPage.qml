import QtQuick 1.1
import com.nokia.meego 1.0

Page {
    id: page

    tools: ToolBarLayout {
        ToolIcon { platformIconId: "toolbar-back"; onClicked: pageStack.pop() }
        ToolIcon {
            platformIconId: "toolbar-delete"
            onClicked: downloads.clearFinished()
        }
    }

    Rectangle {
        anchors.fill: parent
        color: theme.inverted ? "#000000" : "#ffffff"
    }

    ListView {
        id: list
        anchors.fill: parent
        clip: true                       // required on every QML1 flickable
        model: downloads

        header: Item {
            width: list.width
            height: 72
            Label {
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                text: qsTr("Downloads")
                font.pixelSize: 32
            }
        }

        delegate: Item {
            width: list.width
            height: 100                  // taller than the 80px standard row: two lines + a bar

            Column {
                anchors {
                    left: parent.left; leftMargin: 16
                    right: actionIcon.left; rightMargin: 8
                    verticalCenter: parent.verticalCenter
                }
                spacing: 6

                Label {
                    width: parent.width
                    text: name
                    elide: Text.ElideMiddle    // keep the extension visible
                    font.pixelSize: 24
                }

                ProgressBar {
                    width: parent.width
                    minimumValue: 0
                    maximumValue: 1
                    value: progress
                    // state 0 Queued, 1 Running
                    opacity: (state === 0 || state === 1) ? 1 : 0
                    visible: opacity != 0
                    height: 6
                }

                Label {
                    text: statusText
                    font.pixelSize: 18
                    color: state === 3 ? "#ff5555" : "#999999"   // 3 = Failed
                }
            }

            ToolIcon {
                id: actionIcon
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                // Running -> cancel; anything settled -> remove from the list.
                platformIconId: (state === 0 || state === 1) ? "toolbar-cancle" : "toolbar-close"
                onClicked: {
                    if (state === 0 || state === 1) downloads.cancel(index);
                    else downloads.removeEntry(index);
                }
            }

            MouseArea {
                anchors { left: parent.left; right: actionIcon.left; top: parent.top; bottom: parent.bottom }
                onClicked: if (state === 2) downloads.openFile(index)   // 2 = Finished
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.inverted ? "#222222" : "#dddddd"
            }
        }
    }

    Label {
        anchors.centerIn: parent
        text: qsTr("No downloads")
        color: "#808080"
        font.pixelSize: 26
        opacity: downloads.count === 0 ? 1 : 0
        visible: opacity != 0
    }
}
