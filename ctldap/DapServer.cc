///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "DapServer.h"

#include <exception>
#include <iostream>

namespace ctldap {

DapServer::DapServer (DapTransport &transport)
:
    _transport (transport),
    _seq (1),
    _running (true)
{
}

void
DapServer::on (const std::string &command, Handler handler)
{
    _handlers[command] = std::move (handler);
}

int
DapServer::nextSeq ()
{
    return _seq.fetch_add (1);
}

void
DapServer::sendEvent (const std::string &name, const nlohmann::json &body)
{
    nlohmann::json evt = {
        {"seq",    nextSeq()},
        {"type",   "event"},
        {"event",  name},
        {"body",   body}
    };
    _transport.writeMessage (evt);
}

void
DapServer::sendErrorResponse (int requestSeq, const std::string &command,
                              const std::string &message)
{
    nlohmann::json resp = {
        {"seq",         nextSeq()},
        {"type",        "response"},
        {"request_seq", requestSeq},
        {"command",     command},
        {"success",     false},
        {"message",     message}
    };
    _transport.writeMessage (resp);
}

int
DapServer::run ()
{
    while (_running)
    {
        nlohmann::json msg;
        if (!_transport.readMessage (msg)) return 0;     // EOF

        if (msg.value ("type", "") != "request")
        {
            std::cerr << "ctldap: ignoring non-request: "
                      << msg.dump() << std::endl;
            continue;
        }

        const std::string command = msg.value ("command", "");
        const int requestSeq = msg.value ("seq", 0);

        auto it = _handlers.find (command);
        if (it == _handlers.end())
        {
            sendErrorResponse (requestSeq, command,
                               "no handler for command: " + command);
            continue;
        }

        nlohmann::json body;
        bool success = true;
        std::string failMsg;
        try
        {
            body = it->second (msg.value ("arguments", nlohmann::json::object()));
        }
        catch (const std::exception &e)
        {
            success = false;
            failMsg = e.what();
        }

        nlohmann::json resp = {
            {"seq",         nextSeq()},
            {"type",        "response"},
            {"request_seq", requestSeq},
            {"command",     command},
            {"success",     success}
        };
        if (success && !body.is_null()) resp["body"] = body;
        if (!success) resp["message"] = failMsg;
        _transport.writeMessage (resp);

        if (command == "disconnect") _running = false;
    }
    return 0;
}

} // namespace ctldap
