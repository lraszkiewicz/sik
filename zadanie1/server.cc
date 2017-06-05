#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cstdio>
#include <unistd.h>
#include <cstdint>
#include <poll.h>
#include <csignal>
#include <arpa/inet.h>
#include <byteswap.h>
#include <ctime>
#include <cinttypes>
#include <utility>
#include <list>
#include <map>

#include "err.h"
#include "datagram.h"

using namespace std;

bool finish = false;

static void catch_int(int sig) {
  finish = true;
  fprintf(stderr, "\nSignal %d catched, closing.\n", sig);
}

int main(int argc, char *argv[]) {

  if (argc != 3)
    fatal("Usage: %s port filename", argv[0]);
  long port_long = strtol(argv[1], NULL, 10);
  if (errno == ERANGE || port_long <= 0 || port_long > UINT16_MAX)
    fatal("\"%s\" is not a valid and positive uint16_t", argv[1]);
  uint16_t port = (uint16_t) port_long;

  FILE* input_file = fopen(argv[2], "r");
  if (input_file == NULL)
    fatal("error opening file \"%s\"", argv[2]);

  datagram_with_file_t send_buffer;

  size_t file_length = fread(
      send_buffer.file_content, sizeof(char), MAX_FILE_LENGTH, input_file);
  fclose(input_file);
  send_buffer.file_content[file_length] = 0;
  size_t send_buffer_size = sizeof(uint64_t) + sizeof(char) + file_length;

  send_buffer.timestamp = 42;
  send_buffer.c = 'A';

  small_datagram_t recv_buffer;

  if (signal(SIGINT, catch_int) == SIG_ERR) {
    syserr("Unable to change signal handler");
    exit(EXIT_FAILURE);
  }

	int sock = socket(AF_INET, SOCK_DGRAM, 0); // creating IPv4 UDP socket
	checkerr(sock, "socket");

	struct sockaddr_in server_address;
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	server_address.sin_port = htons(port);
  checkerr(bind(sock, (struct sockaddr *) &server_address,
			(socklen_t) sizeof(server_address)), "bind");

  struct pollfd recv_pollfd;
  recv_pollfd.fd = sock;
  recv_pollfd.events = POLLIN;
  recv_pollfd.revents = 0;

  map<pair<in_addr_t, in_port_t>, time_t> client_map;

  while (true) {
    recv_pollfd.revents = 0;

    if (finish) {
      checkerr(close(sock), "close");
      break;
    }

    checkerr(poll(&recv_pollfd, 1, 5000), "poll");

    if (recv_pollfd.revents & POLLIN) {
      struct sockaddr_in from;
      socklen_t fromlen = sizeof(from);
      ssize_t recv_size = recvfrom(
          recv_pollfd.fd, &recv_buffer, sizeof(recv_buffer), 0,
          (struct sockaddr*) &from, &fromlen);
      checkerr((int) recv_size, "recvfrom");

      char str[INET_ADDRSTRLEN];
      fprintf(stderr, "Received from %s:%d\n",
              inet_ntop(AF_INET, &from.sin_addr, str, INET_ADDRSTRLEN),
              ntohs(from.sin_port));
      printf("%" PRIu64 " %c\n",
             bswap_64(recv_buffer.timestamp),
             recv_buffer.c);

      time_t current_time = time(NULL);
      client_map[make_pair(from.sin_addr.s_addr, from.sin_port)] = current_time;

      send_buffer.c = recv_buffer.c;
      send_buffer.timestamp = bswap_64((uint64_t) current_time);

      for (auto client : client_map) {
        if (client.first != make_pair(from.sin_addr.s_addr, from.sin_port)
            && difftime(current_time, client.second) <= 120.0) {

          struct sockaddr_in to;
          socklen_t to_len = sizeof(to);
          to.sin_family = AF_INET;
          to.sin_addr.s_addr = client.first.first;
          to.sin_port = client.first.second;
          for (int i = 0; i < 8; ++i)
            to.sin_zero[i] = 0;

          char str2[INET_ADDRSTRLEN];
          fprintf(stderr, "Sending to %s:%d\n",
                  inet_ntop(AF_INET, &to.sin_addr, str2, INET_ADDRSTRLEN),
                  ntohs(to.sin_port));

          checkerr((int)sendto(recv_pollfd.fd, &send_buffer, send_buffer_size,
                               0, (struct sockaddr*) &to, to_len), "sendto");
        }
      }
    }
  }

	return 0;
}
