/* Public portable C headers must compile without Arduino or ESP-IDF installed. */
#include "agent_link.h"
#include "agent_link_io.h"
#include "agent_link_ota.h"
#include "agent_link_stream.h"
#include "agent_link_transport.h"

int agent_link_public_headers_compile_as_c(void) {
    agent_link_config_t config = {0};
    agent_transport_t transport = {0};
    agent_stream_opts_t options = {0};
    return (int)config.transport + (transport.impl != 0) + (int)options.encoding;
}
