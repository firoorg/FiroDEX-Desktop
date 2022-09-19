/******************************************************************************
 * Copyright © 2013-2022 The Komodo Platform Developers.                      *
 *                                                                            *
 * See the AUTHORS, DEVELOPER-AGREEMENT and LICENSE files at                  *
 * the top-level directory of this distribution for the individual copyright  *
 * holder information and the developer policies on copyright and licensing.  *
 *                                                                            *
 * Unless otherwise agreed in a custom licensing agreement, no part of the    *
 * Komodo Platform software, including this file may be copied, modified,     *
 * propagated or distributed except according to the terms contained in the   *
 * LICENSE file                                                               *
 *                                                                            *
 * Removal or modification of this copyright notice is prohibited.            *
 *                                                                            *
 ******************************************************************************/

//! STD
#include <unordered_set>
#include <iostream>
#include <boost/thread/thread.hpp>
#include <sstream>

//! Ranges
#include <range/v3/algorithm/any_of.hpp>

///! Qt
#include <QException>
#include <QFile>
#include <QProcess>

//! Project Headers
#include "atomicdex/api/mm2/mm2.constants.hpp"
#include "atomicdex/api/mm2/rpc.electrum.hpp"
#include "atomicdex/api/mm2/rpc.enable.hpp"
#include "atomicdex/api/mm2/rpc.min.volume.hpp"
#include "atomicdex/api/mm2/rpc.tx.history.hpp"
#include "atomicdex/api/mm2/rpc2.z_coin_tx_history.hpp"
#include "atomicdex/api/mm2/rpc2.init_z_coin.hpp"
#include "atomicdex/api/mm2/rpc2.init_z_coin_status.hpp"
#include "atomicdex/config/mm2.cfg.hpp"
#include "atomicdex/constants/dex.constants.hpp"
#include "atomicdex/managers/qt.wallet.manager.hpp"
#include "atomicdex/pages/qt.portfolio.page.hpp"
#include "atomicdex/services/internet/internet.checker.service.hpp"
#include "atomicdex/services/mm2/mm2.service.hpp"
#include "atomicdex/utilities/kill.hpp" ///< no delete
#include "atomicdex/utilities/qt.utilities.hpp"
#include "atomicdex/utilities/stacktrace.prerequisites.hpp"

//! Anonymous functions
namespace
{
    namespace ag = antara::gaming;

    void
    check_for_reconfiguration(const std::string& wallet_name)
    {
        try
        {
            using namespace std::string_literals;
            SPDLOG_DEBUG("checking for reconfiguration");

            fs::path    cfg_path                   = atomic_dex::utils::get_atomic_dex_config_folder();
            std::string filename                   = std::string(atomic_dex::get_precedent_raw_version()) + "-coins." + wallet_name + ".json";
            fs::path    precedent_version_cfg_path = cfg_path / filename;

            if (fs::exists(precedent_version_cfg_path))
            {
                //! There is a precedent configuration file
                SPDLOG_INFO("There is a precedent configuration file, upgrading the new one with precedent settings");

                //! Old cfg to ifs
                LOG_PATH("opening file: {}", precedent_version_cfg_path);
                QFile ifs;
                ifs.setFileName(atomic_dex::std_path_to_qstring(precedent_version_cfg_path));
                ifs.open(QIODevice::Text | QIODevice::ReadOnly);
                nlohmann::json precedent_config_json_data;
                precedent_config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

                //! New cfg to ifs
                fs::path actual_version_filepath = cfg_path / (std::string(atomic_dex::get_raw_version()) + "-coins."s + wallet_name + ".json"s);
                LOG_PATH("opening file: {}", actual_version_filepath);
                QFile actual_version_ifs;
                actual_version_ifs.setFileName(atomic_dex::std_path_to_qstring(actual_version_filepath));
                actual_version_ifs.open(QIODevice::Text | QIODevice::ReadOnly);
                nlohmann::json actual_config_data = nlohmann::json::parse(QString(actual_version_ifs.readAll()).toStdString());

                //! Iterate through new config
                for (auto& [key, value]: actual_config_data.items())
                {
                    //! If the coin in new config is present in the old one, copy the contents
                    if (precedent_config_json_data.contains(key))
                    {
                        actual_config_data.at(key)["active"] = precedent_config_json_data.at(key).at("active").get<bool>();
                    }
                }

                for (auto& [key, value]: precedent_config_json_data.items())
                {
                    if (value.contains("is_custom_coin") && value.at("is_custom_coin").get<bool>())
                    {
                        SPDLOG_INFO("{} is a custom coin, copying to new cfg", key);
                        actual_config_data[key] = value;
                    }
                }

                LOG_PATH("closing file: {}", precedent_version_cfg_path);
                ifs.close();
                LOG_PATH("closing file: {}", actual_version_filepath);
                actual_version_ifs.close();

                //! Write contents
                LOG_PATH("opening file: {}", actual_version_filepath);
                QFile ofs;
                ofs.setFileName(atomic_dex::std_path_to_qstring(actual_version_filepath));
                ofs.open(QIODevice::Text | QIODevice::WriteOnly);
                ofs.write(QString::fromStdString(actual_config_data.dump()).toUtf8());

                //! Delete old cfg
                fs_error_code ec;
                fs::remove(precedent_version_cfg_path, ec);
                if (ec)
                {
                    SPDLOG_ERROR("error: {}", ec.message());
                }
                LOG_PATH("closing file: {}", actual_version_filepath);
                ofs.close();
            }
        }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("Exception caught: {}", error.what());
        }
    }

    void
    update_coin_status(
        const std::string& wallet_name, const std::vector<std::string>& tickers, auto status, atomic_dex::t_coins_registry& registry,
        std::shared_mutex& registry_mtx, std::string field_name = "active")
    {
        if (wallet_name == "")
        {
            return;
        }
        SPDLOG_INFO("Update coins status to: {} - field_name: {} - tickers: {}", status, field_name, fmt::join(tickers, ", "));
        fs::path    cfg_path               = atomic_dex::utils::get_atomic_dex_config_folder();
        std::string filename               = std::string(atomic_dex::get_raw_version()) + "-coins." + wallet_name + ".json";
        std::string custom_tokens_filename = "custom-tokens." + wallet_name + ".json";
        fs::path    custom_tokens_filepath = cfg_path / custom_tokens_filename;

        QFile ifs;
        ifs.setFileName(atomic_dex::std_path_to_qstring((cfg_path / filename)));
        ifs.open(QIODevice::ReadOnly | QIODevice::Text);

        nlohmann::json config_json_data;
        nlohmann::json custom_cfg_data;

        if (fs::exists(custom_tokens_filepath.c_str()))
        {
            QFile ifs_custom;
            ifs_custom.setFileName(atomic_dex::std_path_to_qstring(custom_tokens_filepath));
            ifs_custom.open(QIODevice::ReadOnly | QIODevice::Text);
            custom_cfg_data = nlohmann::json::parse(QString(ifs_custom.readAll()).toStdString());
            ifs_custom.close();
        }

        config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());
        {
            std::shared_lock lock(registry_mtx);
            for (auto&& ticker: tickers)
            {
                if (registry[ticker].is_custom_coin)
                {
                    custom_cfg_data.at(ticker)[field_name] = status;
                }
                else
                {
                    config_json_data.at(ticker)[field_name] = status;
                }
                if (field_name == "active")
                {
                    SPDLOG_INFO("ticker: {} status active: {}", ticker, status);
                    registry[ticker].active = status;
                }
                else if (field_name == "is_segwit_on")
                {
                    registry[ticker].is_segwit_on = status;
                }
            }
        }

        ifs.close();

        //! Write contents
        QFile ofs;
        ofs.setFileName(atomic_dex::std_path_to_qstring((cfg_path / filename)));
        ofs.open(QIODevice::Text | QIODevice::WriteOnly);
        ofs.write(QString::fromStdString(config_json_data.dump()).toUtf8());
        ofs.close();

        //! Write contents
        if (!custom_cfg_data.empty())
        {
            //! Write contents
            QFile ofs_custom;
            ofs_custom.setFileName(atomic_dex::std_path_to_qstring(custom_tokens_filepath));
            ofs_custom.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate);
            ofs_custom.write(QString::fromStdString(custom_cfg_data.dump()).toUtf8());
            ofs_custom.close();
        }
    }
} // namespace

namespace atomic_dex
{
    std::vector<atomic_dex::coin_config>
    mm2_service::retrieve_coins_informations()
    {
        std::vector<atomic_dex::coin_config> cfg;
        SPDLOG_DEBUG("retrieve_coins_informations");

        check_for_reconfiguration(m_current_wallet_name);
        const auto  cfg_path               = atomic_dex::utils::get_atomic_dex_config_folder();
        std::string filename               = std::string(atomic_dex::get_raw_version()) + "-coins." + m_current_wallet_name + ".json";
        std::string custom_tokens_filename = "custom-tokens." + m_current_wallet_name + ".json";

        LOG_PATH("Retrieving Wallet information of {}", (cfg_path / filename));
        auto retrieve_cfg_functor = [](fs::path path) -> std::unordered_map<std::string, atomic_dex::coin_config>
        {
            if (exists(path))
            {
                try
                {
                    QFile ifs;
                    ifs.setFileName(atomic_dex::std_path_to_qstring(path));
                    ifs.open(QIODevice::ReadOnly | QIODevice::Text);
                    nlohmann::json config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());
                    auto           res              = config_json_data.get<std::unordered_map<std::string, atomic_dex::coin_config>>();
                    return res;
                }
                catch (const std::exception& error)
                {
                    SPDLOG_ERROR("exception caught: {}", error.what());
                }
            }
            return {};
        };

        auto official_cfg = retrieve_cfg_functor(cfg_path / filename);
        if (!official_cfg.empty())
        {
            cfg.reserve(official_cfg.size());
            for (auto&& [key, value]: official_cfg) { cfg.emplace_back(value); }
            {
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations = std::move(official_cfg);
            }
        }

        auto custom_cfg = retrieve_cfg_functor(cfg_path / custom_tokens_filename);
        if (!custom_cfg.empty())
        {
            SPDLOG_INFO("Custom coins detected, adding them to the runtime configuration");
            for (auto&& [key, value]: custom_cfg) { cfg.emplace_back(value); }
            {
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations.insert(custom_cfg.begin(), custom_cfg.end());
            }
        }

        return cfg;
    }

    mm2_service::mm2_service(entt::registry& registry, ag::ecs::system_manager& system_manager) : system(registry), m_system_manager(system_manager)
    {
        m_orderbook_clock = std::chrono::high_resolution_clock::now();
        m_info_clock      = std::chrono::high_resolution_clock::now();
        dispatcher_.sink<zhtlc_enter_enabling>().connect<&mm2_service::on_zhtlc_enter_enabling>(*this);
        dispatcher_.sink<zhtlc_leave_enabling>().connect<&mm2_service::on_zhtlc_leave_enabling>(*this);
        dispatcher_.sink<gui_enter_trading>().connect<&mm2_service::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().connect<&mm2_service::on_gui_leave_trading>(*this);
        dispatcher_.sink<orderbook_refresh>().connect<&mm2_service::on_refresh_orderbook>(*this);
        SPDLOG_INFO("mm2_service created");
    }

    void
    mm2_service::update()
    {
        using namespace std::chrono_literals;

        if (not m_mm2_running)
        {
            return;
        }

        const auto now    = std::chrono::high_resolution_clock::now();
        const auto s      = std::chrono::duration_cast<std::chrono::seconds>(now - m_orderbook_clock);
        const auto s_info = std::chrono::duration_cast<std::chrono::seconds>(now - m_info_clock);

        if (s >= 5s)
        {
            if (m_nb_update_required > 0)
            {
                auto                     coins = this->get_enabled_coins();
                std::vector<std::string> tickers;
                for (auto&& coin: coins)
                {
                    if (!coin.active)
                    {
                        tickers.push_back(coin.ticker);
                    }
                }
                if (!tickers.empty())
                {
                    SPDLOG_INFO("coin_status_update required, {}", m_nb_update_required);
                    update_coin_status(this->m_current_wallet_name, tickers, true, m_coins_informations, m_coin_cfg_mutex);
                }
                m_nb_update_required -= 1;
            }
            fetch_current_orderbook_thread(false);
            batch_fetch_orders_and_swap();
            m_orderbook_clock = std::chrono::high_resolution_clock::now();
        }

        if (s_info >= 30s)
        {
            fetch_infos_thread();
            m_info_clock = std::chrono::high_resolution_clock::now();
        }
    }

    mm2_service::~mm2_service()
    {
        SPDLOG_INFO("destroying mm2 service...");
        dispatcher_.sink<gui_enter_trading>().disconnect<&mm2_service::on_gui_enter_trading>(*this);
        dispatcher_.sink<gui_leave_trading>().disconnect<&mm2_service::on_gui_leave_trading>(*this);
        dispatcher_.sink<orderbook_refresh>().disconnect<&mm2_service::on_refresh_orderbook>(*this);
        dispatcher_.sink<zhtlc_enter_enabling>().disconnect<&mm2_service::on_zhtlc_enter_enabling>(*this);
        dispatcher_.sink<zhtlc_leave_enabling>().disconnect<&mm2_service::on_zhtlc_leave_enabling>(*this);
        SPDLOG_INFO("mm2 signals successfully disconnected");
        bool mm2_stopped = false;
        if (m_mm2_running)
        {
            SPDLOG_INFO("preparing mm2 stop batch request");
            nlohmann::json stop_request = ::mm2::api::template_request("stop");
            nlohmann::json batch        = nlohmann::json::array();
            batch.push_back(stop_request);
            SPDLOG_INFO("processing mm2 stop batch request");
            pplx::task<web::http::http_response> resp_task = m_mm2_client.async_rpc_batch_standalone(batch);
            web::http::http_response             resp      = resp_task.get();
            SPDLOG_INFO("mm2 stop batch answer received");
            auto answers = ::mm2::api::basic_batch_answer(resp);
            if (answers[0].contains("result"))
            {
                mm2_stopped = answers[0].at("result").get<std::string>() == "success";
                SPDLOG_INFO("mm2 successfully stopped with rpc stop");
            }
        }
        m_mm2_running = false;
        // m_token_source.cancel();
        m_mm2_client.stop();

        if (!mm2_stopped)
        {
            SPDLOG_INFO("mm2 didn't stop yet with rpc stop, stopping process manually");
#if defined(_WIN32) || defined(WIN32)
            atomic_dex::kill_executable(atomic_dex::g_dex_api);
#else
            /*const reproc::stop_actions stop_actions = {
                {reproc::stop::terminate, reproc::milliseconds(2000)},
                {reproc::stop::kill, reproc::milliseconds(5000)},
                {reproc::stop::wait, reproc::milliseconds(2000)}};*/

            /*const auto ec = m_mm2_instance.stop(stop_actions).second;

            if (ec)
            {
                SPDLOG_ERROR("error when stopping mm2 by process: {}", ec.message());
                // std::cerr << "error: " << ec.message() << std::endl;
            }*/
#endif
        }

        if (m_mm2_init_thread.joinable())
        {
            m_mm2_init_thread.join();
            SPDLOG_INFO("mm2 init thread destroyed");
        }
        SPDLOG_INFO("mm2 service fully destroyed");
    }

    const std::atomic_bool&
    mm2_service::is_mm2_running() const
    {
        return m_mm2_running;
    }

    t_coins
    mm2_service::get_enabled_coins() const
    {
        t_coins destination;

        std::shared_lock lock(m_coin_cfg_mutex);
        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.currently_enabled)
            {
                destination.push_back(value);
            }
        }

        return destination;
    }

    t_coins
    mm2_service::get_active_coins() const
    {
        t_coins destination;

        std::shared_lock lock(m_coin_cfg_mutex);
        for (auto&& [key, value]: m_coins_informations)
        {
            if (value.active)
            {
                destination.emplace_back(value);
            }
        }

        return destination;
    }

    bool
    mm2_service::disable_coin(const std::string& ticker, std::error_code& ec)
    {
        coin_config coin_info = get_coin_info(ticker);

        if (not coin_info.currently_enabled)
        {
            return true;
        }

        t_disable_coin_request request{.coin = ticker};

        auto                   answer = m_mm2_client.rpc_disable_coin(std::move(request));
        SPDLOG_INFO("mm2_service::disable_coin: {} result: {}", ticker, answer.raw_result);

        if (answer.error.has_value())
        {
            std::string error = answer.error.value();
            SPDLOG_INFO("mm2_service::disable_coin: {} error {}", ticker, error);
            if (error.find("such coin") != std::string::npos)
            {
                ec = dextop_error::disable_unknown_coin;
                return false;
            }
            if (error.find("active swaps") != std::string::npos)
            {
                ec = dextop_error::active_swap_is_using_the_coin;
                return false;
            }
            if (error.find("matching orders") != std::string::npos)
            {
                ec = dextop_error::order_is_matched_at_the_moment;
                return false;
            }
            return false;
        }

        coin_info.currently_enabled = false;

        {
            std::unique_lock lock(m_coin_cfg_mutex);
            m_coins_informations[ticker].currently_enabled = false;
        }

        dispatcher_.trigger<coin_disabled>(ticker);
        return true;
    }

    bool
    mm2_service::enable_default_coins()
    {
        std::atomic<std::size_t> result{1};
        auto                     coins = get_active_coins();

        std::vector<std::string>                                              second_tickers;
        std::vector<std::string>                                              tickers;
        std::unordered_map<CoinType, std::array<std::vector<coin_config>, 2>> enable_registry;
        const std::size_t                                                     testnet_idx = 0;
        const std::size_t                                                     mainnet_idx = 1;
        std::unordered_set<std::string>                                       visited;
        for (auto&& coin: coins)
        {
            if (visited.contains(coin.ticker))
            {
                SPDLOG_INFO("already visited: {} - skipping", coin.ticker);
                continue;
            }
            auto&      coin_info = coin;
            const bool is_tesnet = coin_info.is_testnet.value_or(false);
            if (coin_info.has_parent_fees_ticker && coin_info.ticker != coin_info.fees_ticker)
            {
                auto coin_parent_info = get_coin_info(coin_info.fees_ticker);
                if (!coin_parent_info.currently_enabled && !coin_parent_info.active && visited.insert(coin_parent_info.ticker).second)
                {
                    SPDLOG_INFO("Adding extra coin: {} to enable", coin_parent_info.ticker);
                    const auto coin_type = (coin_parent_info.ticker == "BCH" || coin_parent_info.ticker == "tBCH") ? CoinType::SLP : coin_parent_info.coin_type;
                    enable_registry[coin_type][is_tesnet ? testnet_idx : mainnet_idx].push_back(coin_parent_info);
                }
            }
            const auto coin_type = (coin_info.ticker == "BCH" || coin_info.ticker == "tBCH") ? CoinType::SLP : coin_info.coin_type;
            enable_registry[coin_type][is_tesnet ? testnet_idx : mainnet_idx].push_back(coin_info);
            visited.insert(coin_info.ticker);
        }

        enable_multiple_coins_v2(enable_registry);

        batch_fetch_orders_and_swap();

        return result.load() == 1;
    }

    void
    mm2_service::disable_multiple_coins(const std::vector<std::string>& tickers)
    {
        for (const auto& ticker: tickers)
        {
            std::error_code ec;
            disable_coin(ticker, ec);
            if (ec)
            {
                SPDLOG_WARN("{}", ec.message());
            }
        }

        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
    }

    auto
    mm2_service::batch_balance_and_tx(bool is_a_reset, std::vector<std::string> tickers, bool is_during_enabling, bool only_tx)
    {
        (void)tickers;
        (void)is_during_enabling;
        auto&& [batch_array, tickers_idx, tokens_to_fetch] = prepare_batch_balance_and_tx(only_tx);
        return m_mm2_client.async_rpc_batch_standalone(batch_array)
            .then(
                [this, tokens_to_fetch = tokens_to_fetch, is_a_reset, tickers](web::http::http_response resp)
                {
                    try
                    {
                        auto answers = ::mm2::api::basic_batch_answer(resp);
                        if (not answers.contains("error"))
                        {
                            for (auto&& answer: answers)
                            {
                                const std::string error = answer.dump(4);
                                if (answer.contains("balance"))
                                {
                                    this->process_balance_answer(answer);
                                }
                                else if (answer.contains("result"))
                                {
                                    this->process_tx_answer(answer);
                                }
                                else
                                {
                                    SPDLOG_ERROR("error answer for tx or my_balance: {}", error);
                                    this->dispatcher_.trigger<tx_fetch_finished>(true);
                                    if (error.find("future timed out") != std::string::npos)
                                    {
                                        SPDLOG_WARN("Future timed out error detected, probably a connection issue");
                                        //! Emit error for UI Change
                                    }
                                }
                            }

                            for (auto&& coin: tokens_to_fetch) { process_tx_tokenscan(coin, is_a_reset); }
                        }
                    }
                    catch (const std::exception& error)
                    {
                        SPDLOG_ERROR("exception in batch_balance_and_tx: {}", error.what());
                        this->dispatcher_.trigger<tx_fetch_finished>(true);
                    }
                })
            .then([this, batch = batch_array](pplx::task<void> previous_task)
                  { this->handle_exception_pplx_task(previous_task, "batch_balance_and_tx", batch); });
    }

    std::tuple<nlohmann::json, std::vector<std::string>, std::vector<std::string>>
    mm2_service::prepare_batch_balance_and_tx(bool only_tx) const
    {
        const auto&              enabled_coins = get_enabled_coins();
        nlohmann::json           batch_array   = nlohmann::json::array();
        std::vector<std::string> tickers_idx;
        std::vector<std::string> tokens_to_fetch;
        const auto&              ticker    = get_current_ticker();
        auto                     coin_info = get_coin_info(ticker);

        if (coin_info.is_erc_family)
        {
            tokens_to_fetch.push_back(ticker);
        }
        else
        {
            std::string     method = "my_tx_history";
            std::size_t     limit =  5000;
            bool            requires_v2 = false;
            if (coin_info.is_zhtlc_family)
            {
                requires_v2 = true;
                limit = 50;
                method = "z_coin_tx_history";
            }
            else if (coin_info.coin_type == CoinTypeGadget::SLP || coin_info.ticker == "tBCH" || coin_info.ticker == "BCH")
            {
                requires_v2 = true;
            }

            t_tx_history_request request{.coin = ticker, .limit = limit};
            nlohmann::json       j = ::mm2::api::template_request(method, requires_v2);
            ::mm2::api::to_json(j, request);
            batch_array.push_back(j);
        }

        if (not only_tx)
        {
            for (auto&& coin: enabled_coins)
            {
                if (is_pin_cfg_enabled())
                {
                    std::shared_lock lock(m_balance_mutex); ///< shared_lock
                    if (m_balance_informations.find(coin.ticker) != m_balance_informations.cend())
                    {
                        continue;
                    }
                }
                SPDLOG_WARN("Getting balance for {} ", coin.ticker);
                t_balance_request balance_request{.coin = coin.ticker};
                nlohmann::json    j = ::mm2::api::template_request("my_balance");
                ::mm2::api::to_json(j, balance_request);
                batch_array.push_back(j);
                tickers_idx.push_back(coin.ticker);
            }
        }
        return std::make_tuple(batch_array, tickers_idx, tokens_to_fetch);
    }

    std::pair<bool, std::string>
    mm2_service::process_batch_enable_answer(const json& answer)
    {
        std::string error = answer.dump(4);

        if (answer.contains("error") || answer.contains("Error") || error.find("error") != std::string::npos || error.find("Error") != std::string::npos)
        {
            SPDLOG_DEBUG("error: bad answer json for enable/electrum details: {}", error);
            return {false, error};
        }

        if (answer.contains("coin"))
        {
            auto ticker = answer.at("coin").get<std::string>();
            {
                std::unique_lock lock(m_coin_cfg_mutex);
                m_coins_informations[ticker].currently_enabled = true;
            }
            return {true, ""};
        }

        if (answer.contains("result"))
        {
            if (answer["result"].contains("task_id"))
            {
                return {true, ""};
            }
        }
        SPDLOG_DEBUG("bad answer json for enable/electrum details: {}", error);

        return {false, error};
    }

    void
    mm2_service::process_enable_zhtlc(std::vector<coin_config> coins)
    {
        dispatcher_.trigger<zhtlc_enter_enabling>();
        auto request_functor = [this](coin_config coin_info) -> std::pair<nlohmann::json, std::vector<std::string>>
        {
            t_init_z_coin_request request{
                .coin_name            = coin_info.ticker,
                .servers              = coin_info.electrum_urls.value_or(get_electrum_server_from_token(coin_info.ticker)),
                .z_urls               = coin_info.z_urls.value_or(std::vector<std::string>{}),
                .coin_type            = coin_info.coin_type,
                .is_testnet           = coin_info.is_testnet.value_or(false),
                .with_tx_history      = false}; // Tx history not yet ready for ZHTLC

            nlohmann::json j = ::mm2::api::template_request("init_z_coin", true);
            ::mm2::api::to_json(j, request);
            nlohmann::json batch = nlohmann::json::array();
            batch.push_back(j);
            return {batch, {coin_info.ticker}};
        };

        auto answer_functor = [this](nlohmann::json batch, std::vector<std::string> tickers)
        {
            auto rpc_answer_functor = [this, tickers](web::http::http_response resp) mutable { this->batch_enable_answer_legacy(resp, tickers); };

            auto error_rpc_functor = [this, tickers, batch](pplx::task<void> previous_task)
            { this->handle_exception_pplx_task(previous_task, "batch_enable_coins legacy electrum", batch); };

            m_mm2_client.async_rpc_batch_standalone(batch)
                .then(
                    [this, tickers](web::http::http_response resp) mutable
                    {
                        try
                        {
                            auto answers = ::mm2::api::basic_batch_answer(resp);

                            if (answers.count("error") == 0)
                            {
                                std::size_t                     idx = 0;
                                std::unordered_set<std::string> to_remove;
                                for (auto&& answer: answers)
                                {
                                    auto [res, error] = this->process_batch_enable_answer(answer);

                                    if (!res)
                                    {
                                        SPDLOG_DEBUG(
                                            "bad answer for: [{}] -> removing it from enabling, idx: {}, tickers size: {}, answers size: {}", tickers[idx], idx,
                                            tickers.size(), answers.size());
                                        this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], error);
                                        to_remove.emplace(tickers[idx]);
                                        if (error.find("already initialized") == std::string::npos)
                                        {
                                            SPDLOG_WARN("Should set to false the active field in cfg for: {} - reason: {}", tickers[idx], error);
                                        }
                                    }
                                    else if (answer.contains("result"))
                                    {
                                        if (answer["result"].contains("task_id"))
                                        {
                                            auto task_id = answer.at("result").at("task_id").get<std::int8_t>();
                                            {
                                                using namespace std::chrono_literals;

                                                static std::size_t z_nb_try      = 0;
                                                nlohmann::json     z_error       = nlohmann::json::array();
                                                nlohmann::json     z_batch_array = nlohmann::json::array();
                                                t_init_z_coin_status_request z_request{.task_id = task_id};

                                                SPDLOG_DEBUG("{} init_z_coin Task ID: {}", tickers[idx], task_id);

                                                nlohmann::json j = ::mm2::api::template_request("init_z_coin_status", true);
                                                ::mm2::api::to_json(j, z_request);
                                                z_batch_array.push_back(j);
                                                std::string last_event = "none";
                                                std::string event = "none";

                                                do {
                                                    pplx::task<web::http::http_response> z_resp_task = m_mm2_client.async_rpc_batch_standalone(z_batch_array);
                                                    web::http::http_response             z_resp      = z_resp_task.get();
                                                    auto                                 z_answers   = ::mm2::api::basic_batch_answer(z_resp);
                                                    z_error                                          = z_answers;
                                                    std::vector<std::string>             this_ticker;
                                                    this_ticker.push_back(tickers[idx]);
                                                    // SPDLOG_DEBUG("z_answer: {}", z_answers[0].dump(4));
                                                    std::string status = z_answers[0].at("result").at("status").get<std::string>();

                                                    if (status == "Ready")
                                                    {
                                                        m_coins_informations[tickers[idx]].activation_status = z_answers[0];
                                                        if (z_answers[0].at("result").at("details").contains("error"))
                                                        {
                                                            event = z_answers[0].at("result").at("details").at("error").get<std::string>();
                                                            SPDLOG_DEBUG("Enabling [{}] error: {}", tickers[idx], event);
                                                            break;
                                                        }
                                                        SPDLOG_DEBUG("{} activation complete!", tickers[idx]);
                                                        std::unique_lock lock(m_coin_cfg_mutex);
                                                        m_coins_informations[tickers[idx]].currently_enabled = true;

                                                        dispatcher_.trigger<coin_fully_initialized>(this_ticker);
                                                        this->m_nb_update_required += 1;
                                                        break;
                                                    }
                                                    else
                                                    {
                                                        if (z_answers[0].at("result").at("details").contains("UpdatingBlocksCache"))
                                                        {
                                                            event = "UpdatingBlocksCache";
                                                            std::size_t current_scanned_block = z_answers[0].at("result").at("details").at("UpdatingBlocksCache").at("current_scanned_block");
                                                            std::size_t latest_block = z_answers[0].at("result").at("details").at("UpdatingBlocksCache").at("latest_block");
                                                            // SPDLOG_DEBUG("Waiting for {} to enable [{}: {}] {}/{} blocks scanned", tickers[idx], status, event, current_scanned_block, latest_block);
                                                        }
                                                        else if (z_answers[0].at("result").at("details").contains("BuildingWalletDb"))
                                                        {
                                                            event = "BuildingWalletDb";
                                                            std::size_t current_scanned_block = z_answers[0].at("result").at("details").at("BuildingWalletDb").at("current_scanned_block");
                                                            std::size_t latest_block = z_answers[0].at("result").at("details").at("BuildingWalletDb").at("latest_block");
                                                            // SPDLOG_DEBUG("Waiting for {} to enable [{}: {}] {}/{} blocks scanned", tickers[idx], status, event, current_scanned_block, latest_block);
                                                        }
                                                        else
                                                        {
                                                            event = z_answers[0].at("result").at("details").get<std::string>();
                                                            // SPDLOG_DEBUG("Waiting for {} to enable [{}: {}]...", tickers[idx], status, event);
                                                        }

                                                        if (event != last_event)
                                                        {
                                                            SPDLOG_DEBUG("Waiting for {} to enable [{}: {}]...", tickers[idx], status, event);
                                                            // After an event change, full activation is just a matter of time (earlier it might fail).
                                                            // We tag it as activated, so it shows up in portfolio and not enable list.
                                                            if (!m_coins_informations[tickers[idx]].currently_enabled && event != "ActivatingCoin")
                                                            {
                                                                std::unique_lock lock(m_coin_cfg_mutex);
                                                                m_coins_informations[tickers[idx]].currently_enabled = true;

                                                                dispatcher_.trigger<coin_fully_initialized>(this_ticker);
                                                                this->m_nb_update_required += 1;
                                                            }
                                                            this->dispatcher_.trigger<enabling_z_coin_status>(tickers[idx], event);
                                                            last_event = event;
                                                        }
                                                        std::this_thread::sleep_for(2s);
                                                    }
                                                    m_coins_informations[tickers[idx]].activation_status = z_answers[0];
                                                    z_nb_try += 1;

                                                } while (z_nb_try < 1000);

                                                try {
                                                    if (z_error[0].at("result").at("details").contains("error"))
                                                    {
                                                        std::string zhtlc_error   = z_error[0].at("result").at("details").at("error").get<std::string>();
                                                        SPDLOG_DEBUG("Error enabling {}: {} ", tickers[idx], zhtlc_error);
                                                        SPDLOG_DEBUG(
                                                            "Removing zhtlc from enabling, idx: {}, tickers size: {}, answers size: {}",
                                                            tickers[idx], idx, tickers.size(), answers.size()
                                                        );

                                                        this->dispatcher_.trigger<enabling_z_coin_status>(tickers[idx], event);
                                                        this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], z_error[0].dump(4));
                                                        to_remove.emplace(tickers[idx]);

                                                        if (error.find("already initialized") == std::string::npos)
                                                        {
                                                            SPDLOG_WARN("Should set to false the active field in cfg for: {} - reason: {}", tickers[idx], zhtlc_error);
                                                        }
                                                        else
                                                        {
                                                            update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                                                        }
                                                    }
                                                    else if (z_nb_try == 1000)
                                                    {
                                                        // TODO: Handle this case.
                                                        // There could be no error message if scanning takes too long.
                                                        // Either we force disable here, or schedule to check on it later
                                                        // If this happens, address will be "Invalid" and balance will be zero.
                                                        // We could save this ticker in a list to try `init_z_coin_status` again on it periodically until complete.

                                                        SPDLOG_DEBUG("Exited zhtlc enable loop after 1000 tries");
                                                        SPDLOG_DEBUG(
                                                            "Bad answer for zhtlc_error: [{}] -> idx: {}, tickers size: {}, answers size: {}", tickers[idx], idx,
                                                            tickers.size(), answers.size()
                                                        );
                                                        this->dispatcher_.trigger<enabling_coin_failed>(tickers[idx], z_error[0].dump(4));
                                                        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                                                        to_remove.emplace(tickers[idx]);
                                                    }
                                                    else
                                                    {
                                                        this->dispatcher_.trigger<enabling_z_coin_status>(tickers[idx], "Complete!");
                                                    }
                                                }
                                                catch (const std::exception& error)
                                                {
                                                    SPDLOG_ERROR("exception caught in zhtlc batch_enable_coins: {}", error.what());
                                                }
                                            }
                                        }
                                    }
                                    idx += 1;
                                }

                                for (auto&& t: to_remove) { tickers.erase(std::remove(tickers.begin(), tickers.end(), t), tickers.end()); }

                                if (!tickers.empty())
                                {
                                    if (tickers == g_default_coins)
                                    {
                                        this->dispatcher_.trigger<default_coins_enabled>();
                                        batch_balance_and_tx(false, tickers, true);
                                    }
                                    dispatcher_.trigger<coin_fully_initialized>(tickers);
                                    if (tickers.size() == 1)
                                    {
                                        fetch_single_balance(get_coin_info(tickers[0]));
                                    }
                                    // batch_balance_and_tx(false, tickers, true);
                                }
                            }
                        }
                        catch (const std::exception& error)
                        {
                            SPDLOG_ERROR("exception caught in batch_enable_coins: {}", error.what());
                            update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                            //! Emit event here
                        }
                    })
                .then(
                    [this, tickers, batch](pplx::task<void> previous_task)
                    {
                        this->handle_exception_pplx_task(previous_task, "batch_enable_coins", batch);
                        update_coin_status(this->m_current_wallet_name, tickers, false, m_coins_informations, m_coin_cfg_mutex);
                    });
            dispatcher_.trigger<zhtlc_leave_enabling>();
        };

        for (auto&& coin: coins)
        {
            auto&& [request, coins_to_enable] = request_functor(coin);
            // SPDLOG_INFO("{} {}", request.dump(4), coins_to_enable[0]);
            answer_functor(request, coins_to_enable);
        }
    }

    void
    mm2_service::batch_enable_answer_legacy(web::http::http_response resp, std::vector<std::string> tickers)
    {
        try
        {
            auto answers = ::mm2::api::basic_batch_answer(resp);

            if (answers.count("error") == 0 && answers.size() == 1)
            {
                const auto answer = answers[0];
                auto [res, error] = this->process_batch_enable_answer(answer);
                if (res)
                {
                    this->process_balance_answer(answer);
                    if (tickers == g_default_coins)
                    {
                        SPDLOG_INFO("Trigger default_coins_enabled");
                        this->dispatcher_.trigger<default_coins_enabled>();
                        this->batch_balance_and_tx(false, tickers, true);
                    }
                    this->dispatcher_.trigger<coin_fully_initialized>(tickers);
                    this->m_nb_update_required += 1;
                }
                else
                {
                    this->dispatcher_.trigger<enabling_coin_failed>(tickers[0], error);
                    SPDLOG_WARN("{}", error);
                }
            }
        }
        catch (const std::exception& error)
        {
            SPDLOG_ERROR("exception caught in batch_enable_coins: {}", error.what());
            this->dispatcher_.trigger<enabling_coin_failed>(tickers[0], error.what());
        }
    }

    bool mm2_service::is_zhtlc_coin_ready(const std::string coin) const
    {
        const auto coin_info       = get_coin_info(coin);
        if (coin_info.is_zhtlc_family)
        {
            if (coin_info.activation_status.contains("result"))
            {
                if (coin_info.activation_status.at("result").contains("status"))
                {
                    if (coin_info.activation_status.at("result").at("status") == "Ready")
                    {
                        if (coin_info.activation_status.at("result").contains("details"))
                        {
                            if (!coin_info.activation_status.at("result").at("details").contains("error"))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
            return false;
        }
        return true;
    }

    void
    mm2_service::process_enable_legacy(std::vector<coin_config> coins)
    {
        auto request_functor = [](coin_config coin_info) -> std::pair<nlohmann::json, std::vector<std::string>>
        {
            t_enable_request request{
                .coin_name       = coin_info.ticker,
                .urls            = coin_info.urls.value_or(std::vector<std::string>{}),
                .coin_type       = coin_info.coin_type,
                .is_testnet      = coin_info.is_testnet.value_or(false),
                .with_tx_history = false};
            nlohmann::json j = ::mm2::api::template_request("enable");
            ::mm2::api::to_json(j, request);
            nlohmann::json batch = nlohmann::json::array();
            batch.push_back(j);
            return {batch, {coin_info.ticker}};
        };

        auto answer_functor = [this](nlohmann::json batch, std::vector<std::string> tickers)
        {
            auto rpc_answer_functor = [this, tickers](web::http::http_response resp) mutable { this->batch_enable_answer_legacy(resp, tickers); };

            auto error_rpc_functor = [this, tickers, batch](pplx::task<void> previous_task)
            { this->handle_exception_pplx_task(previous_task, "batch_enable_coins legacy electrum", batch); };

            m_mm2_client.async_rpc_batch_standalone(batch).then(rpc_answer_functor).then(error_rpc_functor);
        };

        for (auto&& coin: coins)
        {
            auto&& [request, coins_to_enable] = request_functor(coin);
            // SPDLOG_INFO("{} {}", request.dump(4), coins_to_enable[0]);
            answer_functor(request, coins_to_enable);
        }
    }

    void
    mm2_service::process_electrum_legacy(std::vector<coin_config> coins)
    {
        auto request_functor = [this](coin_config coin_info) -> std::pair<nlohmann::json, std::vector<std::string>>
        {
            t_electrum_request request{
                .coin_name             = coin_info.ticker,
                .servers               = coin_info.electrum_urls.value_or(get_electrum_server_from_token(coin_info.ticker)),
                .coin_type             = coin_info.coin_type,
                .is_testnet            = coin_info.is_testnet.value_or(false),
                .with_tx_history       = true,
                .bchd_urls             = coin_info.bchd_urls,
                .allow_slp_unsafe_conf = coin_info.allow_slp_unsafe_conf};
            if (coin_info.segwit && coin_info.is_segwit_on)
            {
                request.address_format                   = nlohmann::json::object();
                request.address_format.value()["format"] = "segwit";
            }
            nlohmann::json j = ::mm2::api::template_request("electrum");
            ::mm2::api::to_json(j, request);
            nlohmann::json batch = nlohmann::json::array();
            batch.push_back(j);
            return {batch, {coin_info.ticker}};
        };

        auto answer_functor = [this](nlohmann::json batch, std::vector<std::string> tickers)
        {
            auto rpc_answer_functor = [this, tickers](web::http::http_response resp) mutable { this->batch_enable_answer_legacy(resp, tickers); };

            auto error_rpc_functor = [this, tickers, batch](pplx::task<void> previous_task)
            { this->handle_exception_pplx_task(previous_task, "batch_enable_coins legacy electrum", batch); };

            m_mm2_client.async_rpc_batch_standalone(batch).then(rpc_answer_functor).then(error_rpc_functor);
        };

        for (auto&& coin: coins)
        {
            auto&& [request, coins_to_enable] = request_functor(coin);
            // SPDLOG_INFO("{} {}", request.dump(4), coins_to_enable[0]);
            answer_functor(request, coins_to_enable);
        }
    }

    void
    mm2_service::batch_enable_coins_v2(CoinType type_to_enable, std::vector<coin_config> coins_to_enable)
    {
        switch (type_to_enable)
        {
        case CoinTypeGadget::UTXO:
        case CoinTypeGadget::SmartChain:
        case CoinTypeGadget::QRC20:
            atomic_dex::print_coins(coins_to_enable);
            this->process_electrum_legacy(coins_to_enable);
            break;
        //case CoinTypeGadget::SLP:
        case CoinTypeGadget::ERC20:
        case CoinTypeGadget::BEP20:
        case CoinTypeGadget::Matic:
        case CoinTypeGadget::Optimism:
        case CoinTypeGadget::Arbitrum:
        case CoinTypeGadget::AVX20:
        case CoinTypeGadget::FTM20:
        case CoinTypeGadget::HRC20:
        case CoinTypeGadget::Ubiq:
        case CoinTypeGadget::KRC20:
        case CoinTypeGadget::Moonriver:
        case CoinTypeGadget::Moonbeam:
        case CoinTypeGadget::HecoChain:
        case CoinTypeGadget::SmartBCH:
        case CoinTypeGadget::EthereumClassic:
        case CoinTypeGadget::RSK:
            this->process_enable_legacy(coins_to_enable);
            break;
        case CoinTypeGadget::ZHTLC:
            this->process_enable_zhtlc(coins_to_enable);
            break;
        case CoinTypeGadget::Disabled:
        case CoinTypeGadget::All:
        case CoinTypeGadget::Size:
            break;
        }
    }

    void
    mm2_service::enable_multiple_coins_v2(const t_coins_enable_registry& coins_to_enable)
    {
        SPDLOG_INFO("enable_multiple_coins_v2");
        for (auto&& [coin_type, networks]: coins_to_enable)
        {
            SPDLOG_INFO("treating: coin_type: {}", coin_type);
            for (auto&& network: networks)
            {
                if (!network.empty())
                {
                    batch_enable_coins_v2(coin_type, network);
                }
            }
        }
    }

    coin_config
    mm2_service::get_coin_info(const std::string& ticker) const
    {
        std::shared_lock lock(m_coin_cfg_mutex);
        if (m_coins_informations.find(ticker) == m_coins_informations.cend())
        {
            return {};
        }
        return m_coins_informations.at(ticker);
    }

    t_orderbook_answer
    mm2_service::get_orderbook(t_mm2_ec& ec) const
    {
        auto&& [base, rel]          = this->m_synchronized_ticker_pair.get();
        const std::string pair      = base + "/" + rel;
        auto              orderbook = m_orderbook.get();
        if (orderbook.base.empty() && orderbook.rel.empty())
        {
            ec = dextop_error::orderbook_empty;
            return {};
        }
        if (pair != orderbook.base + "/" + rel)
        {
            ec = dextop_error::orderbook_ticker_not_found;
            return {};
        }
        return orderbook;
    }

    nlohmann::json
    mm2_service::prepare_batch_orderbook(bool is_a_reset)
    {
        // SPDLOG_INFO("is_a_reset: {}", is_a_reset);
        auto&& [base, rel] = m_synchronized_ticker_pair.get();
        if (rel.empty())
            return nlohmann::json::array();
        nlohmann::json batch = nlohmann::json::array();

        auto generate_req = [&batch](std::string request_name, auto request, bool is_v2=false)
        {
            nlohmann::json current_request = ::mm2::api::template_request(std::move(request_name), is_v2);
            ::mm2::api::to_json(current_request, request);
            batch.push_back(current_request);
        };

        generate_req("orderbook", t_orderbook_request{.base = base, .rel = rel}, true);
        if (is_a_reset)
        {
            generate_req("max_taker_vol", ::mm2::api::max_taker_vol_request{.coin = base});
            generate_req("max_taker_vol", ::mm2::api::max_taker_vol_request{.coin = rel});
            generate_req("min_trading_vol", t_min_volume_request{.coin = base});
            generate_req("min_trading_vol", t_min_volume_request{.coin = rel});
        }
        // SPDLOG_INFO("batch max: {}", batch.dump(4));
        return batch;
    }

    void
    mm2_service::process_orderbook(bool is_a_reset)
    {
        auto batch = prepare_batch_orderbook(is_a_reset);
        if (batch.empty())
            return;
        // SPDLOG_DEBUG("Prep batch orderbook: {}", batch.dump(4));
        // auto&& [base, rel] = m_synchronized_ticker_pair.get();

        auto answer_functor = [this, is_a_reset](web::http::http_response resp)
        {
            auto&& [base, rel] = m_synchronized_ticker_pair.get();
            auto answer        = ::mm2::api::basic_batch_answer(resp);
            //  SPDLOG_INFO("answer {}... ", answer[0].dump(4));
            if (answer.is_array())
            {
                auto orderbook_answer = ::mm2::api::rpc_process_answer_batch<t_orderbook_answer>(answer[0], "orderbook");

                if (is_a_reset)
                {
                    auto base_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[1], "max_taker_vol");
                    if (base_max_taker_vol_answer.rpc_result_code == 200)
                    {
                        if (base == base_max_taker_vol_answer.result->coin)
                        {
                            this->m_synchronized_max_taker_vol->first = base_max_taker_vol_answer.result.value();
                        }
                        // t_float_50 base_res                               = t_float_50(this->m_synchronized_max_taker_vol->first.decimal) * m_balance_factor;
                        // this->m_synchronized_max_taker_vol->first.decimal = base_res.str(8);
                        // SPDLOG_INFO("base max_taker_vol: {}", answer[1].dump(4));
                    }

                    auto rel_max_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<::mm2::api::max_taker_vol_answer>(answer[2], "max_taker_vol");
                    if (rel_max_taker_vol_answer.rpc_result_code == 200)
                    {
                        if (rel == rel_max_taker_vol_answer.result->coin)
                        {
                            this->m_synchronized_max_taker_vol->second = rel_max_taker_vol_answer.result.value();
                        }
                        // t_float_50 rel_res                                 = t_float_50(this->m_synchronized_max_taker_vol->second.decimal) *
                        // m_balance_factor; this->m_synchronized_max_taker_vol->second.decimal = rel_res.str(8);
                        // SPDLOG_INFO("rel max_taker_vol: {}", answer[1].dump(4));
                    }

                    auto base_min_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<t_min_volume_answer>(answer[3], "min_trading_vol");
                    if (base_min_taker_vol_answer.rpc_result_code == 200)
                    {
                        m_synchronized_min_taker_vol->first = base_min_taker_vol_answer.result.value();
                        // SPDLOG_INFO("base min_taker_vol: {}", answer[1].dump(4));

                    }

                    auto rel_min_taker_vol_answer = ::mm2::api::rpc_process_answer_batch<t_min_volume_answer>(answer[4], "min_trading_vol");
                    if (rel_min_taker_vol_answer.rpc_result_code == 200)
                    {
                        m_synchronized_min_taker_vol->second = rel_min_taker_vol_answer.result.value();
                       //  SPDLOG_INFO("rel min_taker_vol: {}", answer[1].dump(4));
                    }
                }

                if (orderbook_answer.rpc_result_code == 200)
                {
                    m_orderbook = orderbook_answer;
                    this->dispatcher_.trigger<process_orderbook_finished>(is_a_reset);
                }
            }
        };

        m_mm2_client.async_rpc_batch_standalone(batch)
            .then(answer_functor)
            .then([this, batch](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "process_orderbook", batch); });
    }

    void
    mm2_service::fetch_current_orderbook_thread(bool is_a_reset)
    {
        //! m_orderbook_thread_active ? SPDLOG_WARN("Nothing to achieve, sleeping") : SPDLOG_INFO("Fetch current orderbook");

        //! If thread is not active ex: we are not on the trading page anymore, we continue sleeping.
        if (!m_orderbook_thread_active)
        {
            return;
        }

        process_orderbook(is_a_reset);
    }

    void
    mm2_service::fetch_single_balance(const coin_config& cfg_infos)
    {
        nlohmann::json batch_array = nlohmann::json::array();
        if (is_pin_cfg_enabled())
        {
            std::shared_lock lock(m_balance_mutex); ///< shared_lock
            if (m_balance_informations.find(cfg_infos.ticker) != m_balance_informations.cend())
            {
                return;
            }
        }

        t_balance_request balance_request{.coin = cfg_infos.ticker};
        nlohmann::json    j = ::mm2::api::template_request("my_balance");
        ::mm2::api::to_json(j, balance_request);
        batch_array.push_back(j);

        auto answer_functor = [this](web::http::http_response resp)
        {
            try
            {
                auto answers = ::mm2::api::basic_batch_answer(resp);
                if (!answers.contains("error") && !answers[0].contains("error"))
                {
                    this->process_balance_answer(answers[0]);
                }
            }
            catch (const std::exception& error)
            {
                SPDLOG_ERROR("exception in fetch_single_balance: {}", error.what());
            }
        };

        auto error_functor = [this, batch = batch_array](pplx::task<void> previous_task)
        { this->handle_exception_pplx_task(previous_task, "fetch_single_balance", batch); };
        m_mm2_client.async_rpc_batch_standalone(batch_array).then(answer_functor).then(error_functor);
    }

    void
    mm2_service::fetch_infos_thread(bool is_a_refresh, bool only_tx)
    {
        SPDLOG_INFO("fetch_infos_thread");
        if (only_tx)
        {
            batch_balance_and_tx(is_a_refresh, {}, false, only_tx);
        }
        else
        {
            const auto& enabled_coins = get_enabled_coins();
            for (auto&& coin: enabled_coins) { fetch_single_balance(coin); }
            batch_balance_and_tx(is_a_refresh, {}, false, true);
        }
    }

    void
    mm2_service::spawn_mm2_instance(std::string wallet_name, std::string passphrase, bool with_pin_cfg)
    {
        this->m_balance_factor = utils::determine_balance_factor(with_pin_cfg);
        SPDLOG_DEBUG("balance factor is: {}", m_balance_factor);
        SPDLOG_DEBUG("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        this->m_current_wallet_name = std::move(wallet_name);
        this->dispatcher_.trigger<coin_cfg_parsed>(this->retrieve_coins_informations());
        this->dispatcher_.trigger<force_update_providers>();
        mm2_config cfg{.passphrase = std::move(passphrase), .rpc_password = atomic_dex::gen_random_password()};
        ::mm2::api::set_system_manager(m_system_manager);
        ::mm2::api::set_rpc_password(cfg.rpc_password);
        json       json_cfg;
        const auto tools_path = ag::core::assets_real_path() / "tools/mm2/";

        nlohmann::to_json(json_cfg, cfg);
        fs::path mm2_cfg_path = (fs::temp_directory_path() / "MM2.json");

        QFile ofs;
        ofs.setFileName(std_path_to_qstring(mm2_cfg_path));
        ofs.open(QIODevice::WriteOnly | QIODevice::Text);
        ofs.write(QString::fromStdString(json_cfg.dump()).toUtf8());
        ofs.close();

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("MM_CONF_PATH", std_path_to_qstring(mm2_cfg_path));
        env.insert("MM_LOG", std_path_to_qstring(utils::get_mm2_atomic_dex_current_log_file()));
        env.insert("MM_COINS_PATH", std_path_to_qstring((utils::get_current_configs_path() / "coins.json")));
        QProcess mm2_instance;
        mm2_instance.setProgram(std_path_to_qstring((tools_path / atomic_dex::g_dex_api)));
        mm2_instance.setWorkingDirectory(std_path_to_qstring(tools_path));
        mm2_instance.setProcessEnvironment(env);
        bool started = mm2_instance.startDetached();

        if (!started)
        {
            SPDLOG_ERROR("Couldn't start mm2");
            std::exit(EXIT_FAILURE);
        }

        m_mm2_init_thread = std::thread(
            [this, mm2_cfg_path]()
            {
                // std::this_thread::
                using namespace std::chrono_literals;
                auto               check_mm2_alive = []() { return ::mm2::api::rpc_version() != "error occured during rpc_version"; };
                static std::size_t nb_try          = 0;

                while (not check_mm2_alive())
                {
                    nb_try += 1;
                    if (nb_try == 30)
                    {
                        SPDLOG_ERROR("MM2 not started correctly");
                        //! TODO: emit mm2_failed_initialization
                        fs::remove(mm2_cfg_path);
                        return;
                    }
                    std::this_thread::sleep_for(1s);
                }

                // m_mm2_client.connect_client();
                fs::remove(mm2_cfg_path);
                SPDLOG_INFO("mm2 is initialized");
                dispatcher_.trigger<mm2_initialized>();
                enable_default_coins();
                m_mm2_running = true;
                dispatcher_.trigger<mm2_started>();
            });
    }

    t_float_50
    mm2_service::get_balance(const std::string& ticker) const
    {
        std::error_code ec;
        t_float_50      balance = safe_float(my_balance(ticker, ec));
        return balance;
    }

    std::pair<t_transactions, t_tx_state>
    mm2_service::get_tx(t_mm2_ec& ec) const
    {
        const auto& ticker = get_current_ticker();
        // SPDLOG_DEBUG("asking history of ticker: {}", ticker);
        const auto underlying_tx_history_map = m_tx_informations.synchronize();
        const auto coin_info                 = get_coin_info(ticker);
        const auto it                        = !(coin_info.is_erc_family) ? underlying_tx_history_map->find("result") : underlying_tx_history_map->find(ticker);
        if (it == underlying_tx_history_map->cend())
        {
            ec = dextop_error::tx_history_of_a_non_enabled_coin;
            return {};
        }
        return it->second;
    }

    t_tx_state
    mm2_service::get_tx_state(t_mm2_ec& ec) const
    {
        return get_tx(ec).second;
    }

    t_transactions
    mm2_service::get_tx_history(t_mm2_ec& ec) const
    {
        return get_tx(ec).first;
    }

    std::string
    mm2_service::my_balance(const std::string& ticker, t_mm2_ec& ec) const
    {
        std::shared_lock lock(m_balance_mutex); ///! read
        auto             it = m_balance_informations.find(ticker);
        if (it == m_balance_informations.cend())
        {
            ec = dextop_error::balance_of_a_non_enabled_coin;
            return "0";
        }

        return it->second.balance;
    }

    void
    mm2_service::batch_fetch_orders_and_swap(bool after_manual_reset)
    {
        nlohmann::json batch             = nlohmann::json::array();
        nlohmann::json my_orders_request = ::mm2::api::template_request("my_orders");
        batch.push_back(my_orders_request);


        //! Swaps preparation
        std::size_t       total           = 0;
        std::size_t       nb_active_swaps = 0;
        std::size_t       current_page    = 0;
        std::size_t       limit           = 0;
        t_filtering_infos filter_infos;
        {
            auto value_ptr  = m_orders_and_swaps.synchronize();
            total           = value_ptr->total_swaps;
            nb_active_swaps = value_ptr->active_swaps;
            current_page    = value_ptr->current_page;
            limit           = value_ptr->limit;
            filter_infos    = value_ptr->filtering_infos;
        }

        //! First time fetch or current page
        nlohmann::json            my_swaps = ::mm2::api::template_request("my_recent_swaps");
        t_my_recent_swaps_request request{
            .limit          = limit,
            .page_number    = current_page,
            .my_coin        = filter_infos.my_coin,
            .other_coin     = filter_infos.other_coin,
            .from_timestamp = filter_infos.from_timestamp,
            .to_timestamp   = filter_infos.to_timestamp,
        };
        to_json(my_swaps, request);
        batch.push_back(my_swaps);

        //! Active swaps
        nlohmann::json         active_swaps = ::mm2::api::template_request("active_swaps");
        t_active_swaps_request active_swaps_request{.statuses = true};
        to_json(active_swaps, active_swaps_request);
        batch.push_back(active_swaps);

        auto answer_functor = [this, limit, filter_infos, after_manual_reset](web::http::http_response resp)
        {
            spdlog::stopwatch stopwatch;

            //! Parsing Resp
            orders_and_swaps result;
            auto             answers = ::mm2::api::basic_batch_answer(resp);

            //! Extract
            const auto orders_answers      = ::mm2::api::rpc_process_answer_batch<t_my_orders_answer>(answers[0], "my_orders");
            const auto swap_answer         = ::mm2::api::rpc_process_answer_batch<t_my_recent_swaps_answer>(answers[1], "my_recent_swaps");
            const auto active_swaps_answer = ::mm2::api::rpc_process_answer_batch<t_active_swaps_answer>(answers[2], "active_swaps");

            result.orders_and_swaps.reserve(orders_answers.orders.size() + limit);
            result.nb_orders        = orders_answers.orders.size();
            result.orders_and_swaps = std::move(orders_answers.orders);
            result.orders_registry  = std::move(orders_answers.orders_id);
            result.limit            = limit;
            result.filtering_infos  = filter_infos;

            //! Recent swaps
            result.active_swaps = active_swaps_answer.uuids.size();
            for (auto&& cur: active_swaps_answer.swaps)
            {
                const auto uuid = cur.order_id.toStdString();
                result.swaps_registry.emplace(uuid);
                result.orders_and_swaps.emplace_back(std::move(cur));
            }

            //! Swaps
            if (swap_answer.result.has_value())
            {
                const auto& swap_success_answer = swap_answer.result.value();
                result.total_swaps              = swap_success_answer.total;
                result.total_finished_swaps     = result.total_swaps - active_swaps_answer.uuids.size();
                result.current_page             = swap_success_answer.page_number;
                result.nb_pages                 = swap_success_answer.total_pages;
                for (auto&& cur: swap_success_answer.swaps)
                {
                    const auto uuid = cur.order_id.toStdString();
                    if (!result.swaps_registry.contains(uuid))
                    {
                        result.swaps_registry.emplace(uuid);
                        result.orders_and_swaps.emplace_back(std::move(cur));
                    }
                }
                result.average_events_time = std::move(swap_success_answer.average_events_time);
            }

            //! Post Metrics
            /*SPDLOG_INFO(
                "Metrics -> [total_swaps: {}, "
                "active_swaps: {}, "
                "nb_orders: {}, "
                "nb_pages: {}, "
                "current_page: {}, "
                "total_finished_swaps: {}]",
                result.total_swaps, result.active_swaps, result.nb_orders, result.nb_pages, result.current_page, result.total_finished_swaps);*/

            //! Compute everything
            m_orders_and_swaps = std::move(result);

            // SPDLOG_INFO("Time elasped for batch_orders_and_swaps: {} seconds", stopwatch);
            this->dispatcher_.trigger<process_swaps_and_orders_finished>(after_manual_reset);
        };

        // SPDLOG_INFO("batch request:{}", batch.dump(4));
        m_mm2_client.async_rpc_batch_standalone(batch)
            .then(answer_functor)
            .then([this, batch](pplx::task<void> previous_task) { this->handle_exception_pplx_task(previous_task, "batch_fetch_orders_and_swap", batch); });
    }

    void
    mm2_service::process_tx_tokenscan(const std::string& ticker, [[maybe_unused]] bool is_a_refresh)
    {
        SPDLOG_DEBUG("process_tx ticker: {}", ticker);
        std::error_code ec;
        using namespace std::string_literals;
        auto construct_url_functor = [this](
                                         const std::string& main_ticker, const std::string& test_ticker, const std::string& url, const std::string& token_url,
                                         const std::string& ticker, const std::string& address)
        {
            std::string out;
            if (ticker == main_ticker || ticker == test_ticker)
            {
                out = "/api/v1/" + url + "/" + address;
            }
            else
            {
                const std::string contract_address = get_raw_mm2_ticker_cfg(ticker).at("protocol").at("protocol_data").at("contract_address");
                out                                = "/api/v2/" + token_url + "/" + contract_address + "/" + address;
            }
            return out;
        };
        auto retrieve_api_functor = [this, construct_url_functor](const std::string& ticker, const std::string& address) -> std::string
        {
            const auto  coin_info = this->get_coin_info(ticker);
            std::string out;
            switch (coin_info.coin_type)
            {
            case CoinTypeGadget::ERC20:
                out = construct_url_functor("ETH", "ETHR", "eth_tx_history", "erc_tx_history", ticker, address);
                break;
            case CoinTypeGadget::BEP20:
                out = construct_url_functor("BNB", "BNBT", "bnb_tx_history", "bep_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Matic:
                out = construct_url_functor("MATIC", "MATICTEST", "plg_tx_history", "plg_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Moonriver:
                out = construct_url_functor("MOVR", "MOVRT", "moonriver_tx_history", "moonriver_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Moonbeam:
                out = construct_url_functor("GLMR", "GLMRT", "moonbeam_tx_history", "moonbeam_tx_history", ticker, address);
                break;
            case CoinTypeGadget::FTM20:
                out = construct_url_functor("FTM", "FTMT", "ftm_tx_history", "ftm_tx_history", ticker, address);
                break;
            case CoinTypeGadget::HecoChain:
                out = construct_url_functor("HT", "HTT", "heco_tx_history", "heco_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Arbitrum:
                out = construct_url_functor("ETH-ARB20", "ETHR-ARB20", "arbitrum_tx_history", "arbitrum_tx_history", ticker, address);
                break;
            case CoinTypeGadget::Optimism:
                out = construct_url_functor("ETH-OPT20", "ETHK-OPT20", "optimism_tx_history", "optimism_tx_history", ticker, address);
                break;
            case CoinTypeGadget::EthereumClassic:
                out = construct_url_functor("ETC", "ETCT", "etc_tx_history", "etc_tx_history", ticker, address);
                break;
            case CoinTypeGadget::RSK:
                out = construct_url_functor("RBTC", "RBTCT", "rsk_tx_history", "rsk_tx_history", ticker, address);
                break;
            case CoinTypeGadget::AVX20:
                out = construct_url_functor("AVAX", "AVAXT", "avx_tx_history", "avx_tx_history", ticker, address);
                break;
            default:
                break;
            }
            if (coin_info.is_testnet.value_or(false))
            {
                out += "&testnet=true";
            }
            return out;
        };
        std::string url = retrieve_api_functor(ticker, address(ticker, ec));
        SPDLOG_INFO("url scan: {}", url);
        ::mm2::api::async_process_rpc_get(::mm2::api::g_etherscan_proxy_http_client, "tx_history", url)
            .then(
                [this, ticker](web::http::http_response resp)
                {
                    auto answer = m_mm2_client.rpc_process_answer<::mm2::api::tx_history_answer>(resp, "tx_history");

                    if (answer.rpc_result_code != 200)
                    {
                        SPDLOG_ERROR("{}", answer.raw_result);
                        this->dispatcher_.trigger<tx_fetch_finished>();
                    }
                    else if (answer.rpc_result_code not_eq -1 and answer.result.has_value())
                    {
                        t_tx_state state;
                        state.state             = "Finished";
                        state.current_block     = 0;
                        state.blocks_left       = 0;
                        state.transactions_left = 0;

                        if (answer.result.value().sync_status.additional_info.has_value())
                        {
                            if (answer.result.value().sync_status.additional_info.value().erc_infos.has_value())
                            {
                                state.blocks_left = answer.result.value().sync_status.additional_info.value().erc_infos.value().blocks_left;
                            }
                            if (answer.result.value().sync_status.additional_info.value().regular_infos.has_value())
                            {
                                state.transactions_left = answer.result.value().sync_status.additional_info.value().regular_infos.value().transactions_left;
                            }
                        }

                        t_transactions out;
                        out.reserve(answer.result.value().transactions.size());

                        const auto& transactions = answer.result.value().transactions;
                        std::for_each(
                            rbegin(transactions), rend(transactions),
                            [&out, this](auto&& current)
                            {
                                tx_infos current_info{
                                    .am_i_sender       = current.my_balance_change[0] == '-',
                                    .confirmations     = current.confirmations.has_value() ? current.confirmations.value() : 0,
                                    .from              = current.from,
                                    .to                = current.to,
                                    .date              = current.timestamp_as_date,
                                    .timestamp         = current.timestamp,
                                    .tx_hash           = current.tx_hash,
                                    .fees              = current.fee_details.normal_fees.has_value() ? current.fee_details.normal_fees.value().amount
                                                                                                     : current.fee_details.qrc_fees.has_value()
                                                                                                     ? current.fee_details.qrc_fees.value().miner_fee
                                                                                                     : current.fee_details.erc_fees.value().total_fee,
                                    .my_balance_change = current.my_balance_change,
                                    .total_amount      = current.total_amount,
                                    .block_height      = current.block_height,
                                    .ec                = dextop_error::success,
                                };

                                const auto& wallet_manager    = this->m_system_manager.get_system<qt_wallet_manager>();
                                current_info.transaction_note = wallet_manager.retrieve_transactions_notes(current_info.tx_hash);
                                out.push_back(std::move(current_info));
                            });

                        //! History
                        SPDLOG_INFO("{} tx size {}", ticker, out.size());
                        m_tx_informations->insert_or_assign(ticker, std::make_pair(out, state));

                        //! Dispatch
                        this->dispatcher_.trigger<tx_fetch_finished>();
                    }
                })
            .then(
                [this](pplx::task<void> previous_task)
                {
                    this->dispatcher_.trigger<tx_fetch_finished>();
                    this->handle_exception_pplx_task(previous_task, "process_tx_tokenscan", {});
                });
    }

    void
    mm2_service::on_refresh_orderbook(const orderbook_refresh& evt)
    {
        SPDLOG_DEBUG("on_refresh_orderbook");

        // SPDLOG_INFO("refreshing orderbook pair: [{} / {}]", evt.base, evt.rel);
        this->m_synchronized_ticker_pair = std::make_pair(evt.base, evt.rel);

        if (this->m_mm2_running)
        {
            SPDLOG_INFO("process_orderbook(true)");
            process_orderbook(true);
        }
    }

    void
    mm2_service::on_zhtlc_enter_enabling([[maybe_unused]] const zhtlc_enter_enabling& evt)
    {
        SPDLOG_DEBUG("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        m_zhtlc_enable_thread_active = true;
    }

    void
    mm2_service::on_zhtlc_leave_enabling([[maybe_unused]] const zhtlc_leave_enabling& evt)
    {
        SPDLOG_DEBUG("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        m_zhtlc_enable_thread_active = false;
    }

    void
    mm2_service::on_gui_enter_trading([[maybe_unused]] const gui_enter_trading& evt)
    {
        SPDLOG_DEBUG("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());

        m_orderbook_thread_active = true;
    }

    void
    mm2_service::on_gui_leave_trading([[maybe_unused]] const gui_leave_trading& evt)
    {
        SPDLOG_DEBUG("{} l{} f[{}]", __FUNCTION__, __LINE__, fs::path(__FILE__).filename().string());
        m_orderbook_thread_active = false;
    }

    bool
    mm2_service::do_i_have_enough_funds(const std::string& ticker, const t_float_50& amount) const
    {
        t_float_50 funds = get_balance(ticker);
        return funds >= amount;
    }

    std::string
    mm2_service::address(const std::string& ticker, t_mm2_ec& ec) const
    {
        std::shared_lock lock(m_balance_mutex);
        auto             it = m_balance_informations.find(ticker);

        if (it == m_balance_informations.cend())
        {
            ec = dextop_error::unknown_ticker;
            SPDLOG_INFO("Invalid Ticker {}", ticker);
            return "Invalid Ticker";
        }

        return it->second.address;
    }

    bool
    mm2_service::is_orderbook_thread_active() const
    {
        return this->m_orderbook_thread_active.load();
    }

    bool
    mm2_service::is_zhtlc_enable_thread_active() const
    {
        return this->m_zhtlc_enable_thread_active.load();
    }

    nlohmann::json
    mm2_service::get_raw_mm2_ticker_cfg(const std::string& ticker) const
    {
        nlohmann::json out;

        std::shared_lock lock(m_raw_coin_cfg_mutex);
        const auto       it = m_mm2_raw_coins_cfg.find(ticker);
        if (it != m_mm2_raw_coins_cfg.end())
        {
            atomic_dex::coin_element element = it->second;
            to_json(out, element);
            return out;
        }
        return nlohmann::json::object();
    }

    mm2_service::t_pair_max_vol
    mm2_service::get_taker_vol() const
    {
        return m_synchronized_max_taker_vol.get();
    }

    mm2_service::t_pair_min_vol
    mm2_service::get_min_vol() const
    {
        return m_synchronized_min_taker_vol.get();
    }

    bool
    mm2_service::is_pin_cfg_enabled() const
    {
        return m_balance_factor != 1.0;
    }

    void
    mm2_service::reset_fake_balance_to_zero(const std::string& ticker)
    {
        {
            std::unique_lock lock(m_balance_mutex);
            m_balance_informations.at(ticker).balance = "0";
        }
        this->dispatcher_.trigger<ticker_balance_updated>(std::vector<std::string>{ticker});
    }

    void
    mm2_service::decrease_fake_balance(const std::string& ticker, const std::string& amount)
    {
        t_float_50 balance = get_balance(ticker);
        t_float_50 amount_f(amount);
        t_float_50 result = balance - amount_f;
        SPDLOG_DEBUG(
            "decreasing {} - {} = {}", balance.str(8, std::ios_base::fixed), amount_f.str(8, std::ios_base::fixed), result.str(8, std::ios_base::fixed));
        if (result < 0)
        {
            reset_fake_balance_to_zero(ticker);
        }
        else
        {
            {
                std::unique_lock lock(m_balance_mutex); //! Write
                m_balance_informations.at(ticker).balance = result.str(8, std::ios_base::fixed);
            }
            this->dispatcher_.trigger<ticker_balance_updated>(std::vector<std::string>{ticker});
        }
    }

    void // Legacy method for all coins except tokens, ZHTLC & BCH/SLP
    mm2_service::process_tx_answer(const nlohmann::json& answer_json)
    {
        ::mm2::api::tx_history_answer answer;
        ::mm2::api::from_json(answer_json, answer);
        t_tx_state state;
        state.state             = answer.result.value().sync_status.state;
        state.current_block     = answer.result.value().current_block;
        state.blocks_left       = 0;
        state.transactions_left = 0;

        if (answer.result.value().sync_status.additional_info.has_value())
        {
            if (answer.result.value().sync_status.additional_info.value().erc_infos.has_value())
            {
                state.blocks_left = answer.result.value().sync_status.additional_info.value().erc_infos.value().blocks_left;
            }
            if (answer.result.value().sync_status.additional_info.value().regular_infos.has_value())
            {
                state.transactions_left = answer.result.value().sync_status.additional_info.value().regular_infos.value().transactions_left;
            }
        }
        t_transactions out;
        out.reserve(answer.result.value().transactions.size());

        for (auto&& current: answer.result.value().transactions)
        {
            tx_infos current_info{

                .am_i_sender       = current.my_balance_change[0] == '-',
                .confirmations     = current.confirmations.has_value() ? current.confirmations.value() : 0,
                .from              = current.from,
                .to                = current.to,
                .date              = current.timestamp_as_date,
                .timestamp         = current.timestamp,
                .tx_hash           = current.tx_hash,
                .my_balance_change = current.my_balance_change,
                .total_amount      = current.total_amount,
                .block_height      = current.block_height,
                .ec                = dextop_error::success,
            };
            if (current.fee_details.normal_fees.has_value())
            {
                current_info.fees = current.fee_details.normal_fees.value().amount;
            }
            else if (current.fee_details.erc_fees.has_value())
            {
                current_info.fees = current.fee_details.erc_fees.value().total_fee;
            }
            else if (current.fee_details.qrc_fees.has_value())
            {
                current_info.fees = current.fee_details.qrc_fees->miner_fee;
            }
            else if (current.transaction_fee.has_value())
            {
                current_info.fees = current.transaction_fee.value();
            }
            if (current_info.timestamp == 0)
            {
                using namespace std::chrono;
                current_info.timestamp   = duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
                current_info.date        = utils::to_human_date<std::chrono::seconds>(current_info.timestamp, "%e %b %Y, %H:%M");
                current_info.unconfirmed = true;
            }

            const auto& wallet_manager    = this->m_system_manager.get_system<qt_wallet_manager>();
            current_info.transaction_note = wallet_manager.retrieve_transactions_notes(current_info.tx_hash);

            out.push_back(std::move(current_info));
        }

        //! History
        m_tx_informations->insert_or_assign("result", std::make_pair(out, state));
        this->dispatcher_.trigger<tx_fetch_finished>();
    }


    void
    mm2_service::process_balance_answer(const nlohmann::json& answer)
    {
        t_balance_answer answer_r;
        ::mm2::api::from_json(answer, answer_r);
        SPDLOG_INFO("Successfully fetched ticker: {} balance: {} address: {}", answer_r.coin, answer_r.balance, answer_r.address);
        if (is_pin_cfg_enabled())
        {
            std::shared_lock lock(m_balance_mutex);

            if (m_balance_informations.find(answer_r.coin) != m_balance_informations.end())
            {
                return;
            }
        }

        t_float_50 result = t_float_50(answer_r.balance) * m_balance_factor;
        answer_r.balance  = result.str(8, std::ios_base::fixed);
        // auto copy_coin = answer_r.coin;
        {
            std::unique_lock lock(m_balance_mutex);
            m_balance_informations[answer_r.coin] = std::move(answer_r);
        }
        // m_system_manager.get_system<portfolio_page>().get_portfolio()->update_balance_values({copy_coin});
    }

    mm2_client&
    mm2_service::get_mm2_client()
    {
        return m_mm2_client;
    }

    std::string
    mm2_service::get_current_ticker() const
    {
        return m_current_ticker.get();
    }

    bool
    mm2_service::set_current_ticker(const std::string& ticker)
    {
        if (ticker != get_current_ticker())
        {
            m_current_ticker = ticker;
            return true;
        }
        return false;
    }

    void
    mm2_service::add_new_coin(const nlohmann::json& coin_cfg_json, const nlohmann::json& raw_coin_cfg_json)
    {
        //! Normal cfg part
        SPDLOG_DEBUG("[{}], [{}]", coin_cfg_json.dump(4), raw_coin_cfg_json.dump(4));
        if (not coin_cfg_json.empty() && not is_this_ticker_present_in_normal_cfg(coin_cfg_json.begin().key()))
        {
            SPDLOG_DEBUG("Adding entry : {} to adex current wallet coins file", coin_cfg_json.dump(4));
            fs::path       cfg_path  = utils::get_atomic_dex_config_folder();
            std::string    filename  = "custom-tokens." + m_current_wallet_name + ".json";
            fs::path       file_path = cfg_path / filename;
            nlohmann::json config_json_data;

            if (fs::exists(file_path))
            {
                SPDLOG_DEBUG("reading contents of custom tokens cfg");
                QFile ifs;
                ifs.setFileName(std_path_to_qstring(file_path));
                ifs.open(QIODevice::Text | QIODevice::ReadOnly);

                //! Read Contents
                config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());
                ifs.close();
            }

            //! Modify contents
            config_json_data[coin_cfg_json.begin().key()] = coin_cfg_json.at(coin_cfg_json.begin().key());

            //! Write contents
            SPDLOG_DEBUG("writing contents of custom tokens cfg");
            QFile ofs;
            ofs.setFileName(std_path_to_qstring(file_path));
            ofs.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
            ofs.write(QString::fromStdString(config_json_data.dump()).toUtf8());
        }
        if (not raw_coin_cfg_json.empty() && not is_this_ticker_present_in_raw_cfg(raw_coin_cfg_json.at("coin").get<std::string>()))
        {
            const fs::path mm2_cfg_path{atomic_dex::utils::get_current_configs_path() / "coins.json"};
            SPDLOG_DEBUG("Adding entry : {} to mm2 coins file {}", raw_coin_cfg_json.dump(4), mm2_cfg_path.string());
            QFile ifs;
            ifs.setFileName(std_path_to_qstring(mm2_cfg_path));
            ifs.open(QIODevice::ReadOnly | QIODevice::Text);
            nlohmann::json config_json_data;

            //! Read Contents
            config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

            //! Modify contents
            config_json_data.push_back(raw_coin_cfg_json);

            //! Close
            ifs.close();

            //! Write contents
            QFile ofs;
            ofs.setFileName(std_path_to_qstring(mm2_cfg_path));
            ofs.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
            ofs.write(QString::fromStdString(config_json_data.dump()).toUtf8());
            ofs.close();
        }
    }

    bool
    mm2_service::is_this_ticker_present_in_raw_cfg(const std::string& ticker) const
    {
        std::shared_lock lock(m_raw_coin_cfg_mutex);
        return m_mm2_raw_coins_cfg.find(ticker) != m_mm2_raw_coins_cfg.end();
    }

    bool
    mm2_service::is_this_ticker_present_in_normal_cfg(const std::string& ticker) const
    {
        std::shared_lock lock(m_coin_cfg_mutex);
        return m_coins_informations.find(ticker) != m_coins_informations.end();
    }

    void
    mm2_service::remove_custom_coin(const std::string& ticker)
    {
        //! Coin need to be disabled to be removed
        assert(not get_coin_info(ticker).currently_enabled);

        //! Remove from our cfg
        if (is_this_ticker_present_in_normal_cfg(ticker))
        {
            SPDLOG_DEBUG("remove it from custom cfg: {}", ticker);
            fs::path    cfg_path = utils::get_atomic_dex_config_folder();
            std::string filename = "custom-tokens." + m_current_wallet_name + ".json";
            QFile       ifs;
            ifs.setFileName(std_path_to_qstring((cfg_path / filename)));
            ifs.open(QIODevice::ReadOnly | QIODevice::Text);
            nlohmann::json config_json_data;


            //! Read Contents
            config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

            {
                std::unique_lock lock(m_coin_cfg_mutex);
                this->m_coins_informations.erase(ticker);
            }

            config_json_data.erase(config_json_data.find(ticker));

            //! Close
            ifs.close();

            //! Write contents
            QFile ofs;
            ofs.setFileName(std_path_to_qstring((cfg_path / filename)));
            ofs.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
            ofs.write(QString::fromStdString(config_json_data.dump()).toUtf8());
            ofs.close();
        }

        if (is_this_ticker_present_in_raw_cfg(ticker))
        {
            SPDLOG_DEBUG("remove it from mm2 cfg: {}", ticker);
            fs::path mm2_cfg_path{atomic_dex::utils::get_current_configs_path() / "coins.json"};
            QFile    ifs;
            ifs.setFileName(std_path_to_qstring(mm2_cfg_path));
            ifs.open(QIODevice::ReadOnly | QIODevice::Text);
            nlohmann::json config_json_data;

            //! Read Contents
            config_json_data = nlohmann::json::parse(QString(ifs.readAll()).toStdString());

            config_json_data.erase(std::find_if(
                begin(config_json_data), end(config_json_data),
                [ticker](nlohmann::json current_elem) { return current_elem.at("coin").get<std::string>() == ticker; }));

            //! Close
            ifs.close();

            //! Write contents
            QFile ofs;
            ofs.setFileName(std_path_to_qstring(mm2_cfg_path));
            ofs.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
            ofs.write(QString::fromStdString(config_json_data.dump()).toUtf8());
            ofs.close();
        }
    }

    std::vector<electrum_server>
    mm2_service::get_electrum_server_from_token(const std::string& ticker)
    {
        std::vector<electrum_server> servers;
        const coin_config            cfg = this->get_coin_info(ticker);
        if (cfg.coin_type == CoinType::QRC20)
        {
            if (cfg.is_testnet.value())
            {
                SPDLOG_INFO("{} is from testnet picking tQTUM electrum", ticker);
                servers = std::move(get_coin_info("tQTUM").electrum_urls.value());
            }
            else
            {
                SPDLOG_INFO("{} is from mainnet picking QTUM electrum", ticker);
                servers = std::move(get_coin_info("QTUM").electrum_urls.value());
            }
        }
        return servers;
    }

    orders_and_swaps
    mm2_service::get_orders_and_swaps() const
    {
        return m_orders_and_swaps.get();
    }

    void
    mm2_service::set_orders_and_swaps_pagination_infos(std::size_t current_page, std::size_t limit, t_filtering_infos filter_infos)
    {
        {
            m_orders_and_swaps = orders_and_swaps{.current_page = current_page, .limit = limit, .filtering_infos = std::move(filter_infos)};
        }
        this->batch_fetch_orders_and_swap(true);
    }

    void
    mm2_service::handle_exception_pplx_task(pplx::task<void> previous_task, const std::string& from, nlohmann::json request)
    {
        try
        {
            previous_task.wait();
        }
        catch (const std::exception& e)
        {
            if (std::string(e.what()).find("mutex lock failed") != std::string::npos)
            {
                return;
            }
            for (auto&& cur: request) cur["userpass"] = "";
            SPDLOG_ERROR("pplx task error: {} from: {}, request: {}", e.what(), from, request.dump(4));
            // this->dispatcher_.trigger<batch_failed>(from, e.what());

            //#if defined(linux) || defined(__APPLE__)
            // SPDLOG_ERROR("stacktrace: {}", boost::stacktrace::to_string(boost::stacktrace::stacktrace()));
            //#endif
            if (std::string(e.what()).find("Failed to read HTTP status line") != std::string::npos ||
                std::string(e.what()).find("WinHttpReceiveResponse: 12002: The operation timed out") != std::string::npos)
            {
                const auto& internet_service = this->m_system_manager.get_system<internet_service_checker>();
                if (!internet_service.is_internet_alive())
                {
                    SPDLOG_WARN("We should reset connection here");
                    this->dispatcher_.trigger<fatal_notification>("connection dropped");
                }
            }
        }
    }

    void
    mm2_service::change_segwit_status(std::string ticker, bool status)
    {
        update_coin_status(this->m_current_wallet_name, {ticker}, status, m_coins_informations, m_coin_cfg_mutex, "is_segwit_on");
    }
} // namespace atomic_dex
