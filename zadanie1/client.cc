#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <cstdint>
#include <cstdlib>
#include <poll.h>
#include <csignal>
#include <arpa/inet.h>
#include <byteswap.h>
#include <ctime>
#include <cinttypes>
#include <netdb.h>

#include "err.h"
#include "datagram.h"

#define PORT_DEFAULT 20160

bool finish = false;

static void catch_int(int sig) {
  finish = true;
  fprintf(stderr, "\nSignal %d catched, closing.\n", sig);
}

int main(int argc, char *argv[]) {

  if (argc < 4 || argc > 5)
    fatal("Usage: %s timestamp c host [port]", argv[0]);

  uint64_t timestamp = strtoull(argv[1], NULL, 10);

  if (strlen(argv[2]) != 1)
    fatal("Invalid 'c' argument");
  char c = argv[2][0];

  uint16_t port = PORT_DEFAULT;
  if (argc == 5) {
    long port_long = strtol(argv[4], NULL, 10);
    if (errno == ERANGE || port_long <= 0 || port_long > UINT16_MAX)
      fatal("\"%s\" is not a valid and positive uint16_t", argv[4]);
    port = (uint16_t) port_long;
  }
  char port_string[10];
  sprintf(port_string, "%d", port);

  struct addrinfo addr_hints;
  struct addrinfo *addr_result;
  (void) memset(&addr_hints, 0, sizeof(struct addrinfo));
  addr_hints.ai_family = AF_INET; // IPv4
  addr_hints.ai_socktype = SOCK_DGRAM;
  addr_hints.ai_protocol = IPPROTO_UDP;
  addr_hints.ai_flags = 0;
  addr_hints.ai_addrlen = 0;
  addr_hints.ai_addr = NULL;
  addr_hints.ai_canonname = NULL;
  addr_hints.ai_next = NULL;
  if (getaddrinfo(argv[3], port_string, &addr_hints, &addr_result) != 0)
    syserr("getaddrinfo");

  struct sockaddr_in my_address;
  my_address.sin_family = AF_INET;
  my_address.sin_addr.s_addr =
      ((struct sockaddr_in*) (addr_result->ai_addr))->sin_addr.s_addr;
  my_address.sin_port = htons(port);

  freeaddrinfo(addr_result);

  int sock = socket(PF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    syserr("socket");

  if (signal(SIGINT, catch_int) == SIG_ERR) {
    syserr("Unable to change signal handler");
    exit(EXIT_FAILURE);
  }

  socklen_t addrlen = sizeof(my_address);
  small_datagram_t send_buffer;
  send_buffer.timestamp = bswap_64(timestamp);
  send_buffer.c = c;
  checkerr((int) sendto(sock, &send_buffer, sizeof(send_buffer), 0,
                        (struct sockaddr*) &my_address, addrlen), "sendto");

  datagram_with_file_t recv_buffer;
  struct sockaddr_in server_address;
  socklen_t server_addrlen = sizeof(server_address);

  struct pollfd recv_pollfd;
  recv_pollfd.fd = sock;
  recv_pollfd.events = POLLIN;
  recv_pollfd.revents = 0;

  while (true) {
    recv_pollfd.revents = 0;

    if (finish) {
      checkerr(close(sock), "close");
      break;
    }

    checkerr(poll(&recv_pollfd, 1, 5000), "poll");

    if (recv_pollfd.revents & POLLIN) {
      ssize_t recv_size = recvfrom(
          sock, &recv_buffer, sizeof(recv_buffer), 0,
          (struct sockaddr *) &server_address, &server_addrlen);
      checkerr((int) recv_size, "recvfrom");

      printf("%" PRIu64 " %c %s\n",
             bswap_64(recv_buffer.timestamp),
             recv_buffer.c,
             recv_buffer.file_content);
    }
  }

  close(sock);

  return 0;
}
