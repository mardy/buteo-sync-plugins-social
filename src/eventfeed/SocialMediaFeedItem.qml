import QtQuick 2.0
import Sailfish.Silica 1.0

BackgroundItem {
    property string timestamp: model.timestamp
    property string formattedTime
    property SocialAvatar avatar: _avatar

    onTimestampChanged: formatTime()

    SocialAvatar {
        id: _avatar
        source: model.icon
        width: Theme.itemSizeMedium
        height: Theme.itemSizeMedium
    }

    Connections {
        target: refreshTimer
        onTriggered: formatTime()
    }

    function formatTime() {
        formattedTime = Format.formatDate(timestamp, Formatter.DurationElapsed)
    }

    Component.onCompleted: formatTime()
}