import QtQuick 2.12

import "../Components"
import "../Constants"
import App 1.0
import Dex.Themes 1.0 as Dex

MouseArea
{
    hoverEnabled: true

    Connections
    {
        target: parent.parent

        function onIsExpandedChanged()
        {
            if (isExpanded) waitForSidebarExpansionTimer.start();
            else
            {
                versionLabel.opacity = 0;
                dexLogo.source = Dex.CurrentTheme.logoPath;
                dexLogo.scale = .5;
            }
        }
    }

    Timer
    {
        id: waitForSidebarExpansionTimer
        interval: 200
        onTriggered:
        {
            fadeInTextVerAnimation.start();
            dexLogo.source = Dex.CurrentTheme.bigLogoPath;
            dexLogo.scale = .8;
        }
    }

    NumberAnimation
    {
        id: fadeInTextVerAnimation
        target: versionLabel
        properties: "opacity"
        duration: 350
        to: 1
    }

    NumberAnimation
    {
        id: fadeOutDexLogoAnimation
        target: dexLogo
        properties: "opacity"
        duration: 200
        to: 0.2
    }

    NumberAnimation
    {
        id: fadeInDexLogoAnimation
        target: dexLogo
        properties: "opacity"
        duration: 200
        to: 0.2
    }

    Image
    {
        id: dexLogo
        anchors.horizontalCenter: parent.horizontalCenter
        scale: .8
        source: Dex.CurrentTheme.bigLogoPath

        DefaultText
        {
            id: versionLabel
            visible: isExpanded
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.bottom
            anchors.topMargin: 35

            scale: 1.1
            text_value: General.version_string
            font: DexTypo.caption
            color: Dex.CurrentTheme.sidebarVersionTextColor
        }
    }
}
