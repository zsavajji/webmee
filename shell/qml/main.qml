// n9browser shell -- the chrome. The engine renders into a child X window that
// C++ reparents over webViewArea; everything here is ordinary Harmattan QML so
// the browser looks and behaves like the stock one.
import QtQuick 1.1
import com.nokia.meego 1.0

PageStackWindow {
    id: appWindow

    initialPage: browserPage
    showStatusBar: true
    // Hides with the URL bar and comes back with it. BrowserPage clears
    // chromeHidden whenever it is not the active page, so every other page
    // keeps its toolbar.
    showToolBar: !browserPage.chromeHidden

    // The stock browser is dark. theme.inverted drives every platform component
    // and most image://theme graphics, so set it once here.
    Component.onCompleted: theme.inverted = true

    BrowserPage { id: browserPage }

    TabsSheet {
        id: tabsSheet
        onAccepted: browserPage.showTab(selectedIndex)
    }

    // Tabs live here rather than in the page so they survive navigation.
    ListModel {
        id: tabModel
        ListElement { tabUrl: "about:blank"; tabTitle: "New tab" }
    }
    property int currentTab: 0

    function openTab(url) {
        tabModel.append({ "tabUrl": url, "tabTitle": url });
        currentTab = tabModel.count - 1;
        browserPage.navigate(url);
    }
}
