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

    TabsSheet { id: tabsSheet }

    // Tabs live here rather than in the page so they survive navigation.
    //
    // A row is all a tab IS: its address and its title. The engine has one web
    // view and can only have one -- WPEWebProcess peaks at 116-127 MB against
    // the ~150 MB free -- so switching re-loads rather than revealing a page
    // that was still running. Parking each tab's WebKit session state was tried
    // and reverted: repeatedly restoring one live view fights the engine, spawns
    // a web process per switch, and wedges it. See docs/protocol.md.
    ListModel {
        id: tabModel
        ListElement { tabUrl: "about:blank"; tabTitle: "New tab" }
    }
    property int currentTab: 0      // an index into tabModel

    // Nothing wrote these back before, so every tab stayed at the url it was
    // created with and switching to one loaded about:blank. The engine is the
    // only thing that knows where a page actually went -- a redirect, a link,
    // a form post -- so the row follows it rather than the address bar.
    Connections {
        target: engine
        onUrlChanged: {
            if (currentTab >= 0 && currentTab < tabModel.count)
                tabModel.setProperty(currentTab, "tabUrl", engine.url);
        }
        onTitleChanged: {
            if (currentTab >= 0 && currentTab < tabModel.count && engine.title !== "")
                tabModel.setProperty(currentTab, "tabTitle", engine.title);
        }
    }

    function openTab(url) {
        tabModel.append({ "tabUrl": url,
                          "tabTitle": (url === "" || url === "about:blank")
                                      ? qsTr("New tab") : url });
        currentTab = tabModel.count - 1;
        engine.load(url);
    }

    function showTab(index) {
        if (index < 0 || index >= tabModel.count || index === currentTab)
            return;
        currentTab = index;
        // currentTab first: load() reports the url straight back, and the sync
        // above must write it to the row being switched TO.
        engine.load(tabModel.get(index).tabUrl);
    }

    // Closing the tab being looked at has to move the view somewhere, or the
    // browser keeps showing a page that no longer has a tab.
    function closeTab(index) {
        if (index < 0 || index >= tabModel.count || tabModel.count < 2)
            return;
        var wasCurrent = (index === currentTab);
        tabModel.remove(index);
        if (wasCurrent) {
            // The rows after this one have already shifted up, so the same
            // index is now the next tab along. currentTab is set aside first:
            // it still names the row that was removed, and showTab would take
            // that for "already there" and do nothing.
            var next = index < tabModel.count ? index : tabModel.count - 1;
            currentTab = -1;
            showTab(next);
        } else if (index < currentTab) {
            currentTab = currentTab - 1;
        }
    }
}
