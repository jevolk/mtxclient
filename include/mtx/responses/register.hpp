#pragma once

#include <string>

#if __has_include(<nlohmann/json_fwd.hpp>)
#include <nlohmann/json_fwd.hpp>
#else
#include <nlohmann/json.hpp>
#endif

#include "mtx/identifiers.hpp"

namespace mtx {
namespace responses {

//! Response from the `POST /_matrix/client/r0/register` endpoint.
struct Register
{
        //! The fully-qualified Matrix user ID that has been registered.
        mtx::identifiers::User user_id;
        //! An access token for the account. This access token can then be used to
        //! authorize other requests.
        std::string access_token;
        //! ID of the registered device. Will be the same as the corresponding
        //! parameter in the request, if one was specified.
        std::string device_id;
};

void
from_json(const nlohmann::json &obj, Register &response);
}
}