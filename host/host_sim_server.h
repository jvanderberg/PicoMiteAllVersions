/*
 * host_sim_server.h -- Start/stop API for the --sim HTTP+WS server.
 */

#ifndef HOST_SIM_SERVER_H
#define HOST_SIM_SERVER_H

int host_sim_server_start(const char *listen_addr, int port, const char *web_root);
void host_sim_server_stop(void);

#endif
