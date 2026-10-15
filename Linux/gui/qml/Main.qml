import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root

    required property var controller

    width: 560
    height: 720
    minimumWidth: 460
    minimumHeight: 560
    visible: true
    title: qsTr("OpenDisplay")

    function saved(name, fallback) {
        const value = root.controller.savedSettings[name]
        return value === undefined || value === null ? fallback : value
    }

    function selectSaved(combo, name, fallback) {
        const index = combo.indexOfValue(saved(name, fallback))
        combo.currentIndex = index >= 0 ? index : 0
    }

    function connectionSettings() {
        return {
            "transport": transportBox.currentValue,
            "mode": modeBox.currentValue,
            "encoder": encoderBox.currentValue,
            "compositor": compositorBox.currentValue,
            "host": hostField.text,
            "udid": udidField.text,
            "referenceMonitor": referenceField.text,
            "extendTo": extendBox.currentValue,
            "alignTo": alignBox.currentValue,
            "referenceSizeMm": referenceSizeField.text,
            "receiverSizeMm": receiverSizeField.text,
            "virtualResolution": resolutionField.text,
            "displayScale": scaleField.text,
            "fps": fpsSpin.value,
            "bitrateMbps": bitrateSpin.value,
            "input": inputCheck.checked,
            "verbose": verboseCheck.checked
        }
    }

    onClosing: close => {
        if (root.controller.trayAvailable && !root.controller.quitting) {
            close.accepted = false
            root.hide()
        } else {
            root.controller.quit()
        }
    }

    Connections {
        target: root.controller

        function onShowWindowRequested() {
            root.show()
            root.raise()
            root.requestActivate()
        }
    }

    pageStack.initialPage: Kirigami.ScrollablePage {
        id: page
        title: qsTr("OpenDisplay")

        ColumnLayout {
            width: page.availableWidth
            spacing: Kirigami.Units.largeSpacing

            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: root.controller.connected ? "video-display" : "video-display-symbolic"
                    implicitWidth: Kirigami.Units.iconSizes.huge
                    implicitHeight: implicitWidth
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    Kirigami.Heading {
                        text: root.controller.status
                        level: 2
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: root.controller.detail
                        color: Kirigami.Theme.disabledTextColor
                        wrapMode: Text.Wrap
                    }
                }

                Controls.BusyIndicator {
                    running: root.controller.busy
                    visible: running
                }
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: !root.controller.trayAvailable
                type: Kirigami.MessageType.Information
                text: qsTr("No StatusNotifier tray host was detected. OpenDisplay will remain a normal window and closing it will quit. Plasma's tray and StatusNotifier-compatible bars such as Waybar support background mode.")
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                visible: root.controller.status === "Connection failed"
                         || root.controller.status === "Invalid settings"
                type: Kirigami.MessageType.Error
                text: root.controller.detail
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true

                Controls.ComboBox {
                    id: transportBox
                    Kirigami.FormData.label: qsTr("Connection:")
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { "text": qsTr("Automatic"), "value": "auto" },
                        { "text": qsTr("Wi-Fi"), "value": "wifi" },
                        { "text": qsTr("USB"), "value": "usb" }
                    ]
                    Component.onCompleted: root.selectSaved(this, "transport", "auto")
                }

                Controls.TextField {
                    id: hostField
                    Kirigami.FormData.label: qsTr("Wi-Fi address:")
                    placeholderText: qsTr("Automatic discovery")
                    text: root.saved("host", "")
                    enabled: !root.controller.busy && transportBox.currentValue !== "usb"
                }

                Controls.TextField {
                    id: udidField
                    Kirigami.FormData.label: qsTr("USB device UDID:")
                    placeholderText: qsTr("First connected device")
                    text: root.saved("udid", "")
                    enabled: !root.controller.busy && transportBox.currentValue !== "wifi"
                }

                Controls.ComboBox {
                    id: modeBox
                    Kirigami.FormData.label: qsTr("Display mode:")
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { "text": qsTr("Extend"), "value": "extend" },
                        { "text": qsTr("Mirror"), "value": "mirror" }
                    ]
                    Component.onCompleted: root.selectSaved(this, "mode", "extend")
                }

                Controls.ComboBox {
                    id: encoderBox
                    Kirigami.FormData.label: qsTr("Encoder:")
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { "text": qsTr("Automatic"), "value": "auto" },
                        { "text": qsTr("VA-API"), "value": "vaapi" },
                        { "text": qsTr("NVENC"), "value": "nvenc" },
                        { "text": qsTr("Software"), "value": "software" }
                    ]
                    Component.onCompleted: root.selectSaved(this, "encoder", "auto")
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            Kirigami.Heading {
                text: qsTr("Monitor layout")
                level: 3
            }

            Kirigami.FormLayout {
                Layout.fillWidth: true
                enabled: modeBox.currentValue === "extend" && !root.controller.busy

                Controls.ComboBox {
                    id: compositorBox
                    Kirigami.FormData.label: qsTr("Compositor:")
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { "text": qsTr("Automatic"), "value": "auto" },
                        { "text": qsTr("KDE Plasma"), "value": "kde" },
                        { "text": qsTr("Hyprland"), "value": "hyprland" },
                        { "text": qsTr("GNOME Wayland"), "value": "gnome" }
                    ]
                    Component.onCompleted: root.selectSaved(this, "compositor", "auto")
                }

                Controls.TextField {
                    id: referenceField
                    Kirigami.FormData.label: qsTr("Reference monitor:")
                    placeholderText: qsTr("Automatic when only one is active")
                    text: root.saved("referenceMonitor", "")
                }

                Controls.ComboBox {
                    id: extendBox
                    Kirigami.FormData.label: qsTr("Extend to:")
                    textRole: "text"
                    valueRole: "value"
                    model: [
                        { "text": qsTr("Left"), "value": "left" },
                        { "text": qsTr("Right"), "value": "right" },
                        { "text": qsTr("Top"), "value": "top" },
                        { "text": qsTr("Bottom"), "value": "bottom" }
                    ]
                    Component.onCompleted: root.selectSaved(this, "extendTo", "right")
                    onCurrentValueChanged: {
                        const savedAlignment = root.saved("alignTo", "bottom")
                        const index = alignBox.indexOfValue(savedAlignment)
                        alignBox.currentIndex = index >= 0 ? index : 1
                    }
                }

                Controls.ComboBox {
                    id: alignBox
                    Kirigami.FormData.label: qsTr("Align to:")
                    textRole: "text"
                    valueRole: "value"
                    model: extendBox.currentValue === "left" || extendBox.currentValue === "right"
                           ? [
                               { "text": qsTr("Top"), "value": "top" },
                               { "text": qsTr("Bottom"), "value": "bottom" },
                               { "text": qsTr("Center"), "value": "center" }
                             ]
                           : [
                               { "text": qsTr("Left"), "value": "left" },
                               { "text": qsTr("Right"), "value": "right" },
                               { "text": qsTr("Center"), "value": "center" }
                             ]
                }

                Controls.TextField {
                    id: referenceSizeField
                    Kirigami.FormData.label: qsTr("Reference size (mm):")
                    placeholderText: qsTr("e.g. 366.2x228.9")
                    text: root.saved("referenceSizeMm", "")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }

                Controls.TextField {
                    id: receiverSizeField
                    Kirigami.FormData.label: qsTr("Receiver size (mm):")
                    placeholderText: qsTr("e.g. 232.8x160.5")
                    text: root.saved("receiverSizeMm", "")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }

                Controls.TextField {
                    id: resolutionField
                    Kirigami.FormData.label: qsTr("Virtual resolution:")
                    placeholderText: qsTr("Automatic, or WIDTHxHEIGHT")
                    text: root.saved("virtualResolution", "")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }

                Controls.TextField {
                    id: scaleField
                    Kirigami.FormData.label: qsTr("Display scale:")
                    placeholderText: qsTr("Automatic")
                    text: root.saved("displayScale", "")
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
            }

            Kirigami.Separator { Layout.fillWidth: true }

            Kirigami.FormLayout {
                Layout.fillWidth: true

                Controls.SpinBox {
                    id: fpsSpin
                    Kirigami.FormData.label: qsTr("Frame rate:")
                    from: 1
                    to: 240
                    value: root.saved("fps", 30)
                }

                Controls.SpinBox {
                    id: bitrateSpin
                    Kirigami.FormData.label: qsTr("Bitrate (Mbps):")
                    from: 1
                    to: 200
                    value: root.saved("bitrateMbps", 18)
                }

                Controls.CheckBox {
                    id: inputCheck
                    Kirigami.FormData.label: qsTr("Input:")
                    text: qsTr("Forward touch and scrolling")
                    checked: root.saved("input", true)
                }

                Controls.CheckBox {
                    id: verboseCheck
                    Kirigami.FormData.label: qsTr("Diagnostics:")
                    text: qsTr("Write verbose logs to the terminal")
                    checked: root.saved("verbose", false)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing

                Item { Layout.fillWidth: true }

                Controls.Button {
                    text: qsTr("Disconnect")
                    enabled: root.controller.connected || root.controller.busy
                    onClicked: root.controller.disconnectDevice()
                }

                Controls.Button {
                    text: qsTr("Connect")
                    icon.name: "network-connect"
                    highlighted: true
                    enabled: !root.controller.connected && !root.controller.busy
                    onClicked: root.controller.connectDevice(root.connectionSettings())
                }
            }
        }
    }
}
