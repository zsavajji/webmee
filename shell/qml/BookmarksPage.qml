import QtQuick 1.1
import com.nokia.meego 1.0
import com.nokia.extras 1.1

Page {
    id: page

    tools: ToolBarLayout {
        ToolIcon { platformIconId: "toolbar-back"; onClicked: pageStack.pop() }
        ToolIcon {
            platformIconId: "toolbar-add"
            // Star the page you are currently on, without leaving this list.
            onClicked: {
                bookmarks.add(engine.url, engine.title);
                addedBanner.show();
            }
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
        model: bookmarks

        header: Item {
            width: list.width
            height: 72
            Label {
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                text: qsTr("Favourites")
                font.pixelSize: 32
            }
        }

        delegate: Item {
            id: row
            width: list.width
            height: 80                   // the platform's standard list row

            Rectangle {
                anchors.fill: parent
                color: "#ffffff"
                opacity: mouse.pressed ? 0.12 : 0
            }

            Image {
                id: star
                anchors { left: parent.left; leftMargin: 16; verticalCenter: parent.verticalCenter }
                width: 32; height: 32
                // Note the platform's own inconsistency: the star is spelled
                // "favorite", the content icon "favourites".
                source: "image://theme/icon-m-common-favorite-mark"
                        + (theme.inverted ? "-inverse" : "")
            }

            Column {
                anchors {
                    left: star.right; leftMargin: 16
                    right: removeIcon.left; rightMargin: 8
                    verticalCenter: parent.verticalCenter
                }
                spacing: 4

                Label {
                    width: parent.width
                    text: title
                    elide: Text.ElideRight
                    font.pixelSize: 24
                }
                Label {
                    width: parent.width
                    text: host
                    elide: Text.ElideRight
                    font.pixelSize: 18
                    color: "#999999"
                }
            }

            ToolIcon {
                id: removeIcon
                anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                platformIconId: "toolbar-close"
                onClicked: bookmarks.removeAt(index)
            }

            MouseArea {
                id: mouse
                anchors { left: parent.left; right: removeIcon.left; top: parent.top; bottom: parent.bottom }
                onClicked: {
                    browserPage.navigate(url);
                    pageStack.pop();
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: theme.inverted ? "#222222" : "#dddddd"
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 12
        opacity: bookmarks.count === 0 ? 1 : 0
        visible: opacity != 0

        Image {
            anchors.horizontalCenter: parent.horizontalCenter
            source: "image://theme/icon-l-content-favourites"
            opacity: 0.4
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("No favourites yet")
            color: "#808080"
            font.pixelSize: 26
        }
        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: qsTr("Tap the star to save a page")
            color: "#606060"
            font.pixelSize: 20
        }
    }

    InfoBanner {
        id: addedBanner
        text: qsTr("Added to favourites")
        timerShowTime: 1500
    }
}
