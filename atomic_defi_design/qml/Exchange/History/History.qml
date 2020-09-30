import QtQuick 2.14
import QtQuick.Layouts 1.12
import QtQuick.Controls 2.12

import "../../Components"
import "../../Constants"
import ".."

Item {
    id: exchange_history

    function inCurrentPage() {
        return  exchange.inCurrentPage() &&
                exchange.current_page === General.idx_exchange_history
    }

    function reset() {
    }

    function onOpened() {
        API.app.orders_mdl.orders_proxy_mdl.setFilterFixedString("")
        API.app.orders_mdl.orders_proxy_mdl.is_history = true
        API.app.refresh_orders_and_swaps()
    }

    property string recover_funds_result: '{}'

    function onRecoverFunds(order_id) {
        const result = API.app.recover_fund(order_id)
        console.log("Refund result: ", result)
        recover_funds_result = result
        recover_funds_modal.open()
    }

    ColumnLayout {
        anchors.horizontalCenter: parent.horizontalCenter

        width: parent.width
        height: parent.height
        spacing: 15

        OrderList {
            title: API.app.settings_pg.empty_string + (qsTr("Recent Swaps"))
            empty_text: API.app.settings_pg.empty_string + (qsTr("You don't have recent orders."))
            items: API.app.orders_mdl
        }
    }

    OrderModal {
        id: order_modal
    }

    LogModal {
        id: recover_funds_modal

        header: API.app.settings_pg.empty_string + (qsTr("Recover Funds Result"))
        field.text: General.prettifyJSON(recover_funds_result)

        onClosed: recover_funds_result = "{}"
    }
}










/*##^##
Designer {
    D{i:0;autoSize:true;height:480;width:640}
}
##^##*/