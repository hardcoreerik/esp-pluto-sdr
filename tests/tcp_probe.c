#include "esp_pluto_sdr.h"
#include "esp_pluto_tcp.h"
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc != 2)
    return 10;
  pluto_tcp tcp;
  pluto_io io;
  if (pluto_tcp_connect(&tcp, "127.0.0.1", (uint16_t)atoi(argv[1]), 500, &io))
    return 11;
  pluto_client c;
  pluto_init(&c, io);
  char xml[64];
  size_t n;
  int r = pluto_context(&c, xml, sizeof xml, &n);
  pluto_tcp_disconnect(&tcp);
  return r || n != 4 || strcmp(xml, "<x/>");
}
