///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDAP_SERVER_H
#define CTLDAP_SERVER_H

#include "DapTransport.h"
#include "json.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>

namespace ctldap {

// Owns the DAP request/response/event state machine.  Dispatches incoming
// requests to registered handlers; sends correlated responses; emits
// asynchronous events.
class DapServer
{
  public:
    using Handler = std::function<nlohmann::json (const nlohmann::json &args)>;

    explicit DapServer (DapTransport &transport);

    // Register a handler for a DAP request command (e.g. "initialize",
    // "launch", "setBreakpoints").  Handler receives request.arguments
    // and returns the response.body (or empty json for no body).
    void on (const std::string &command, Handler handler);

    // Run the dispatch loop until EOF or `disconnect` request.
    int run ();

    // Emit a DAP event from any thread (mutex-protected via transport).
    void sendEvent (const std::string &name,
                    const nlohmann::json &body = nlohmann::json::object());

    // Send a generic error response (used by handlers that throw).
    void sendErrorResponse (int requestSeq, const std::string &command,
                            const std::string &message);

    // Returns the next sequence number (event seq, response seq, etc.).
    int nextSeq ();

  private:
    DapTransport                       &_transport;
    std::map<std::string, Handler>      _handlers;
    std::atomic<int>                    _seq;
    std::atomic<bool>                   _running;
};

} // namespace ctldap

#endif
