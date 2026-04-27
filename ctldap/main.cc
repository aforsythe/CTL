///////////////////////////////////////////////////////////////////////////
// Copyright Contributors to the CTL project.
// SPDX-License-Identifier: BSD-3-Clause
///////////////////////////////////////////////////////////////////////////

#include "DapHandlers.h"
#include "DapServer.h"
#include "DapTransport.h"

#include <iostream>

int main (int, char **)
{
    ctldap::DapTransport transport (std::cin, std::cout);
    ctldap::DapServer    server    (transport);
    ctldap::HandlerSet   handlers  (server);
    return server.run();
}
