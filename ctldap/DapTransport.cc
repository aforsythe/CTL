///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "DapTransport.h"

#include <iostream>
#include <vector>

namespace ctldap {

DapTransport::DapTransport (std::istream &in, std::ostream &out)
:
    _in (in), _out (out)
{
}

bool
DapTransport::readMessage (nlohmann::json &out)
{
    // Outer loop: skip past any garbage between messages and keep
    // reading until either (a) we successfully parse one full message
    // (return true) or (b) the stream hits real EOF (return false).
    // Without this, a single un-framed stray line on stdin used to
    // tear the whole server down.
    while (true)
    {
        std::size_t contentLength = 0;
        std::string line;
        bool sawAnyLine = false;
        while (std::getline (_in, line))
        {
            sawAnyLine = true;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty()) break;       // header/body separator
            const std::string prefix = "Content-Length:";
            if (line.compare (0, prefix.size(), prefix) == 0)
            {
                try { contentLength = std::stoul (line.substr (prefix.size())); }
                catch (const std::exception &) { contentLength = 0; }
            }
            // Other headers (Content-Type, etc.) silently ignored.
        }

        if (!sawAnyLine && _in.eof()) return false;     // genuine EOF

        if (contentLength == 0)
        {
            // Got input but no Content-Length header — log + keep going.
            std::cerr << "DapTransport: ignoring frame without Content-Length"
                      << std::endl;
            if (_in.eof()) return false;
            continue;
        }

        std::vector<char> buf (contentLength);
        _in.read (buf.data(), static_cast<std::streamsize> (contentLength));
        if (static_cast<std::size_t> (_in.gcount()) != contentLength)
            return false;     // stream cut off mid-body — treat as EOF

        try
        {
            out = nlohmann::json::parse (buf.begin(), buf.end());
            return true;
        }
        catch (const nlohmann::json::parse_error &e)
        {
            // Bad JSON in the body: log and try the next frame.
            std::cerr << "DapTransport: parse error: " << e.what() << std::endl;
            continue;
        }
    }
}

void
DapTransport::writeMessage (const nlohmann::json &msg)
{
    std::lock_guard<std::mutex> lk (_writeMutex);
    std::string body = msg.dump();
    _out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    _out.flush();
}

} // namespace ctldap
