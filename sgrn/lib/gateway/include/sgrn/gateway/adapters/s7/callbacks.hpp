#pragma once

#include <snap7.h>

namespace sgrn::gateway::adapters::s7
{
/**
 * SERVER EVENT HANDLER: Snap7 Protocol Layer
 * -----------------------------------------
 * This callback is triggered by the low-level Snap7 server whenever a PLC
 * connects or disconnects. We use this to:
 * 1. Log the session in the GatewayDatabase (IT Visibility).
 * 2. Update the srvAreaPE (Input) table (OT Visibility).
 * 3. Clear status slots when a connection is dropped to ensure the OT-side
 *    doesn't see stale IPs.
 */

void S7API onServerEvent(void* tp_server_context, PSrvEvent t_event, int /*Size*/);

} // namespace sgrn::gateway::adapters::s7
