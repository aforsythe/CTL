///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#ifndef CTLDAP_TRANSPORT_H
#define CTLDAP_TRANSPORT_H

#include "json.hpp"

#include <istream>
#include <mutex>
#include <ostream>
#include <string>

namespace ctldap {

// Reads/writes Content-Length-framed JSON messages over std::iostream
// pairs.  Construct with stdin/stdout for production use.
//
// readMessage() blocks until a complete message arrives or EOF.  Returns
// false on EOF, true on success (and writes the parsed JSON to `out`).
//
// writeMessage() is mutex-protected so multiple emitter threads (e.g. the
// dispatch thread sending responses + the debugger thread emitting events)
// don't interleave.
class DapTransport
{
  public:
    DapTransport (std::istream &in, std::ostream &out);

    bool readMessage  (nlohmann::json &out);
    void writeMessage (const nlohmann::json &msg);

  private:
    std::istream &_in;
    std::ostream &_out;
    std::mutex    _writeMutex;
};

} // namespace ctldap

#endif
