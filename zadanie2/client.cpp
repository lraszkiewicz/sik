#include <cstdio>
#include <cstdlib>
#include <netdb.h>
#include <arpa/inet.h>
#include <cinttypes>
#include <string>
#include <poll.h>
#include <zconf.h>

#include "siktacka.h"
#include "util.h"

using namespace std;

void incorrectArguments(char *argv0) {
  fprintf(stderr,
          "Usage: %s player_name game_server_host[:port]"
          "[ui_server_host[:port]]\n",
          argv0);
  exit(EXIT_FAILURE);
}

void parseNetworkAddress(char *address, addrinfo **addrResults, uint16_t *port,
                         bool UDP, string what) {
  addrinfo addrHints;
  memset(&addrHints, 0, sizeof(addrinfo));
  if (UDP) {
    addrHints.ai_socktype = SOCK_DGRAM;
    addrHints.ai_protocol = IPPROTO_UDP;
  } else { // TCP
    addrHints.ai_socktype = SOCK_STREAM;
    addrHints.ai_protocol = IPPROTO_TCP;
  }
  // Try to parse with getaddrinfo.
  int ret = getaddrinfo(address, NULL, &addrHints, addrResults);
  if (ret != 0) {
    // If getaddrinfo was not successful, maybe there was also a port number.
    // Cut it and try again.
    size_t colonPos = 0;
    for (size_t i = strlen(address) - 1; i > 0; --i) {
      if (address[i] == ':') {
        address[i] = 0;
        colonPos = i;
        break;
      }
    }
    if (colonPos != 0)
      ret = getaddrinfo(address, NULL, &addrHints, addrResults);
    if (ret != 0) {
      // If it didn't work now, something is wrong with the address.
      if (ret == EAI_SYSTEM) {
        syserr("getaddrinfo (%s): %s (%d)\n",
               what.c_str(), strerror(ret), ret);
      } else {
        fatal("getaddrinfo (%s): %s (%d)\n",
              what.c_str(), strerror(ret), ret);
      }
      exit(EXIT_FAILURE);
    } else {
      // The address was successfully parsed, now parse the port number.
      *port = parseUInt16(address + colonPos + 1);
    }
  }
  if ((*addrResults)->ai_family == AF_INET) {
    char str[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET,
                  &((sockaddr_in*) (*addrResults)->ai_addr)->sin_addr,
                  str, INET_ADDRSTRLEN) == NULL) {
      syserr("inet_ntop");
    }
    fprintf(stderr, "%s address: %s:%" PRIu16 "\n", what.c_str(), str, *port);
  } else {
    char str[INET6_ADDRSTRLEN];
    if (inet_ntop(AF_INET6,
                  &((sockaddr_in6*) (*addrResults)->ai_addr)->sin6_addr,
                  str, INET6_ADDRSTRLEN) == NULL) {
      syserr("inet_ntop");
    }
    fprintf(stderr, "%s address: [%s]:%" PRIu16 "\n", what.c_str(), str, *port);
  }
}

int main(int argc, char *argv[]) {
  int ret;

  // Parse command line arguments.
  if (argc < 3 || argc > 4) {
    fprintf(stderr, "Incorrect amount of command line arguments.\n");
    incorrectArguments(argv[0]);
  }

  // player_name
  char *playerName = argv[1];
  if (strlen(playerName) > PLAYER_NAME_MAX_LENGTH) {
    fprintf(stderr, "Player name \"%s\" is too long (max. 64 characters).\n",
            playerName);
    incorrectArguments(argv[0]);
  }
  for (size_t i = 0; i < strlen(playerName); ++i) {
    if (playerName[i] < 33 || playerName[i] > 126) {
      fprintf(stderr,
              "Player name contains illegal character '%c' (ASCII %d). "
              "All characters in a player name have to be in range of "
              "ASCII 33-126.\n",
              playerName[i], playerName[i]);
      incorrectArguments(argv[0]);
    }
  }
  fprintf(stderr, "player name: \"%s\"\n", playerName);

  // game_server_host
  addrinfo *serverAddrInfo;
  uint16_t serverPort = 12345;
  parseNetworkAddress(argv[2], &serverAddrInfo, &serverPort, true, "server");

  // ui_server_host
  addrinfo *guiAddrInfo;
  uint16_t guiPort = 12346;
  char defaultgui[10] = "localhost";
  if (argc == 4)
    parseNetworkAddress(argv[3], &guiAddrInfo, &guiPort, true, "GUI");
  else
    parseNetworkAddress(defaultgui, &guiAddrInfo, &guiPort, true, "GUI");

  pollfd sockets[2]; // 0 is to read from server, 1 to read from gui

  // UDP sockets for server connection.
  sockaddr_in serverAddr;
  serverAddr.sin_family = (sa_family_t) serverAddrInfo->ai_family;
  serverAddr.sin_addr.s_addr =
      ((sockaddr_in *) serverAddrInfo->ai_addr)->sin_addr.s_addr;
  serverAddr.sin_port = htons(serverPort);

  sockets[0].fd = socket(serverAddrInfo->ai_family, SOCK_DGRAM, 0);
  checkSysError(sockets[0].fd, "socket to server");
  sockets[0].events = POLLIN;
  sockets[0].revents = 0;
  checkSysError(bind(sockets[0].fd, (sockaddr *) &serverAddr,
                     sizeof(serverAddr)),
                "bind server socket");

  freeaddrinfo(serverAddrInfo);

  // TCP sockets for GUI connection.
  sockaddr_in guiAddr;
  guiAddr.sin_family = (sa_family_t) guiAddrInfo->ai_family;
  guiAddr.sin_addr.s_addr =
      ((sockaddr_in *) guiAddrInfo->ai_addr)->sin_addr.s_addr;
  guiAddr.sin_port = htons(guiPort);

  sockets[1].fd = socket(guiAddrInfo->ai_family, SOCK_STREAM, IPPROTO_TCP);
  checkSysError(sockets[0].fd, "socket to GUI");
  sockets[1].events = POLLIN;
  sockets[1].revents = 0;
  checkSysError(connect(sockets[1].fd, (sockaddr *) &guiAddr, sizeof(guiAddr)),
                "connect to GUI");

  freeaddrinfo(guiAddrInfo);

//  char s[100] = "NEW_GAME 200 200 abc def\n";
//  checkSysError((int) write(sockets[1].fd, s, strlen(s)), "write to GUI");
//  while (true) {
//    usleep(20 * 1000);
//  }

  size_t sendBufSize = sizeof(ClientToServerDatagram) + strlen(playerName);
  ClientToServerDatagram *sendBuf =
      (ClientToServerDatagram *) malloc(sendBufSize);
  memcpy(sendBuf->playerName, playerName, strlen(playerName));
  uint64_t currentTime = getCurrentTime();
  uint64_t sessionId = currentTime;
  sendBuf->sessionId = htobe64(sessionId);
  int8_t turnDirection = 0;
  uint32_t lastEventNumber = 0;
  uint64_t nextSendToServer = sessionId + 20 * 1000;
  while (true) {
    currentTime = getCurrentTime();
    if (currentTime >= nextSendToServer) {
      // 20 ms passed, time to send a message to server.
      sendBuf->turnDirection = turnDirection;
      sendBuf->nextExpectedEventNumer = lastEventNumber + 1;
      if (sendto(sockets[0].fd, sendBuf, sendBufSize, 0,
                 (sockaddr *)&serverAddr, sizeof(serverAddr)) < sendBufSize) {
        syserr("sendto");
      }
      nextSendToServer += 20 * 1000;
    } else {
      // Try to recieve some data.
      sockets[0].revents = 0;
      sockets[1].revents = 0;
      checkSysError(poll(sockets, 2,
                         (int) ((nextSendToServer - currentTime) / 1000)),
                    "poll");
      
    }
  }

  exit(EXIT_SUCCESS);
}