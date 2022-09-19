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

//! Dependencies Headers
#include <nlohmann/json.hpp>

//! Project Headers
#include "utxo.merge.params.hpp"

namespace mm2::api
{
    void
    to_json(nlohmann::json& j, const utxo_merge_params& cfg)
    {
        j["merge_at"] = cfg.merge_at;
        j["check_every"] = cfg.check_every;
        j["max_merge_at_once"] = cfg.max_merge_at_once;
    }
} // namespace mm2::api