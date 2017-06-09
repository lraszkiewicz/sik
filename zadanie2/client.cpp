#include <cstdio>
#include <cstdlib>
#include <netdb.h>
#include <arpa/inet.h>
#include <cinttypes>
#include <string>
#include <poll.h>
#include <zconf.h>
#include <vector>
#include <csignal>
#include <netinet/tcp.h>

#include "siktacka.h"
#include "util.h"

using namespace std;

const bool DEBUG = false;

//const int DELAY = 1000; // milliseconds between sending messages to server
const int DELAY = 20; // milliseconds between sending messages to server

const size_t BUF_FROM_GUI_SIZE = 20;
const size_t BUF_TO_GUI_SIZE = 2000;

const char LEFT_KEY_DOWN[] = "LEFT_KEY_DOWN";
const char LEFT_KEY_UP[] = "LEFT_KEY_UP";
const char RIGHT_KEY_DOWN[] = "RIGHT_KEY_DOWN";
const char RIGHT_KEY_UP[] = "RIGHT_KEY_UP";

bool finish = false;

void catchSigInt(int sig) {
  finish = true;
  fprintf(stderr, "Signal %d catched, closing.\n", sig);
}

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
  if (strlen(playerName) > 0)
    fprintf(stderr, "Player name: %s\n", playerName);
  else
    fprintf(stderr, "Player name is empty, joining as observer.\n");

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

  freeaddrinfo(serverAddrInfo);

  // TCP sockets for GUI connection.
  sockaddr_in guiAddr;
  guiAddr.sin_family = (sa_family_t) guiAddrInfo->ai_family;
  guiAddr.sin_addr.s_addr =
      ((sockaddr_in *) guiAddrInfo->ai_addr)->sin_addr.s_addr;
  guiAddr.sin_port = htons(guiPort);

  sockets[1].fd = socket(guiAddrInfo->ai_family, SOCK_STREAM, IPPROTO_TCP);
  checkSysError(sockets[1].fd, "socket to GUI");
  // Disable Nagle's algorithm on this socket.
  int flagNagle = 1;
  setsockopt(sockets[1].fd, IPPROTO_TCP, TCP_NODELAY,
             &flagNagle, sizeof(flagNagle));
  sockets[1].events = POLLIN;
  sockets[1].revents = 0;
  checkSysError(connect(sockets[1].fd, (sockaddr *) &guiAddr, sizeof(guiAddr)),
                "connect to GUI");

  freeaddrinfo(guiAddrInfo);

  if (signal(SIGINT, catchSigInt) == SIG_ERR)
    syserr("changing SIGINT handler");

  size_t sendBufSize = sizeof(ClientToServerDatagram) + strlen(playerName);
  ClientToServerDatagram *sendBuf =
      (ClientToServerDatagram *) malloc(sendBufSize);
  memcpy(sendBuf->playerName, playerName, strlen(playerName));
  uint64_t currentTime = getCurrentTime();
  uint64_t sessionId = currentTime;
  sendBuf->sessionId = htobe64(sessionId);
  int8_t turnDirection = 0;
  uint32_t nextEventNumber = 0;
  uint64_t nextSendToServer = sessionId;
  vector<string> playerNames;
  uint32_t currentGameId = 0;
  while (true) {
    if (finish)
      break;

    currentTime = getCurrentTime();
    if (currentTime >= nextSendToServer) {
      // DELAY ms passed, time to send a message to server.
      sendBuf->turnDirection = turnDirection;
      sendBuf->nextExpectedEventNumer = htonl(nextEventNumber);
      if (DEBUG)
        fprintf(stderr,
                "Sending to server: turnDirection %" PRId8 ", "
                "nextExpectedEventNumber %" PRIu32 "\n",
                turnDirection,
                nextEventNumber);

      // Attempt to do a non-blocking sendto.
      ssize_t sendtoRet =
          sendto(sockets[0].fd, sendBuf, sendBufSize, MSG_DONTWAIT,
                 (sockaddr *) &serverAddr, (socklen_t) sizeof(serverAddr));
      // If sendto would block, don't set the non-blocking flag.
      if (sendtoRet == EAGAIN || sendtoRet == EWOULDBLOCK) {
        fprintf(stderr, "Sendto would block, attempting without flags.\n");
        sendtoRet = sendto(sockets[0].fd, sendBuf, sendBufSize, 0,
                           (sockaddr *) &serverAddr,
                           (socklen_t) sizeof(serverAddr));
      }
      if (sendtoRet != sendBufSize)
        syserr("sendto");

      nextSendToServer += DELAY * 1000;
    } else {
      // Try to recieve some data.
      sockets[0].revents = 0;
      sockets[1].revents = 0;
      int pollRet =
          poll(sockets, 2, (int) ((nextSendToServer - currentTime) / 1000));
      if (pollRet < 0) {
        syserr("poll");
      } else if (pollRet > 0) {
        if (sockets[0].revents & POLLIN) {
          // Recieve and parse data from server, send the events to GUI.
          char messageToGui[BUF_TO_GUI_SIZE];
          int messageToGuiLength = 0;
          uint8_t buf[MAX_DATAGRAM_SIZE];
          ssize_t recvSize = recv(sockets[0].fd, buf, MAX_DATAGRAM_SIZE, 0);
          if (DEBUG)
            fprintf(stderr, "Recieved %zd bytes from server.\n", recvSize);
          uint32_t gameId = ntohl(*((uint32_t *) buf));
          if (DEBUG)
            fprintf(stderr, "Game ID: %u\n", gameId);
          ssize_t eventStart = sizeof(ServerToClientDatagramHeader);
          while (eventStart < recvSize) {
            EventHeader *eventHeader = (EventHeader *) (buf + eventStart);
            if (DEBUG)
              fprintf(stderr, "Event:\nlength: %u\nnumber: %u\n",
                      ntohl(eventHeader->len), ntohl(eventHeader->eventNumber));

            if (ntohl(eventHeader->eventNumber) + 1 > nextEventNumber
                || eventHeader->eventType == NEW_GAME)
              nextEventNumber = ntohl(eventHeader->eventNumber) + 1;

            eventStart += sizeof(EventHeader);

            if (eventHeader->eventType == NEW_GAME) {
              fprintf(stderr, "New game.\n");
              currentGameId = gameId;
              playerNames.clear();
              NewGameEventDataHeader *eventDataHeader =
                  (NewGameEventDataHeader *) (buf + eventStart);
              messageToGuiLength +=
                  sprintf(messageToGui + messageToGuiLength,
                          "NEW_GAME %" PRIu32 " %" PRIu32 " ",
                          ntohl(eventDataHeader->width),
                          ntohl(eventDataHeader->height));
              size_t playerListLen = ntohl(eventHeader->len)
                                     - sizeof(EventHeader)
                                     + sizeof(uint32_t) // len itself
                                     - sizeof(NewGameEventDataHeader);
              string playerNameString;
              for (size_t i = 0; i < playerListLen; ++i) {
                if (eventDataHeader->playerNames[i] == 0) {
                  messageToGui[messageToGuiLength] = ' ';
                  playerNames.push_back(playerNameString);
                  playerNameString.clear();
                } else {
                  messageToGui[messageToGuiLength] =
                      eventDataHeader->playerNames[i];
                  playerNameString += eventDataHeader->playerNames[i];
                }
                ++messageToGuiLength;
              }
              messageToGui[messageToGuiLength] = '\n';
              ++messageToGuiLength;
              eventStart += sizeof(NewGameEventDataHeader) + playerListLen;

            } else if (eventHeader->eventType == PIXEL) {
              PixelEventData *eventData = (PixelEventData *) (buf + eventStart);
              if (gameId == currentGameId)
                messageToGuiLength +=
                    sprintf(messageToGui + messageToGuiLength,
                            "PIXEL %" PRIu32 " %" PRIu32 " %s\n",
                            ntohl(eventData->x),
                            ntohl(eventData->y),
                            playerNames[eventData->playerNumber].c_str());
              eventStart += sizeof(PixelEventData);

            } else if (eventHeader->eventType == PLAYER_ELIMINATED) {
              PlayerEliminatedEventData *eventData =
                  (PlayerEliminatedEventData *) (buf + eventStart);
              if (gameId == currentGameId)
                messageToGuiLength +=
                    sprintf(messageToGui + messageToGuiLength,
                            "PLAYER_ELIMINATED %s\n",
                            playerNames[eventData->playerNumber].c_str());
              eventStart += sizeof(PlayerEliminatedEventData);

            } else if (eventHeader->eventType == GAME_OVER) {
              fprintf(stderr, "Game over!\n");
            }

            if (DEBUG)
              fprintf(stderr, "crc32: %u\n",
                      ntohl(*((uint32_t *)(buf + eventStart))));
            eventStart += sizeof(uint32_t);
          }
          if (DEBUG)
            fprintf(stderr, "%.*s", messageToGuiLength, messageToGui);
          checkSysError((int) write(sockets[1].fd, messageToGui,
                                    (size_t) messageToGuiLength),
                        "write to GUI");
        }
        if (sockets[1].revents & POLLIN) {
          // Revieve data from GUI.
          char buf[BUF_FROM_GUI_SIZE];
          ssize_t readBytes = read(sockets[1].fd, buf, BUF_FROM_GUI_SIZE);
          if (readBytes < 0)
            syserr("read");
          else if (readBytes == 0) {
            fprintf(stderr, "Connection with GUI ended, exiting.\n");
            exit(EXIT_SUCCESS);
          }
          else if (readBytes == BUF_FROM_GUI_SIZE) {
            fprintf(stderr, "Message from GUI too long, ignoring.\n");
            continue;
          }
          buf[readBytes] = 0;

          if (DEBUG)
            fprintf(stderr, "Read %zd bytes from GUI: %.*s",
                    readBytes, (int) readBytes, buf);

          if (strncmp(buf, LEFT_KEY_DOWN, sizeof(LEFT_KEY_DOWN) - 1) == 0) {
            turnDirection = -1;
          } else if (strncmp(buf, RIGHT_KEY_DOWN,
                             sizeof(RIGHT_KEY_DOWN) - 1) == 0) {
            turnDirection = 1;
          } else if (strncmp(buf, LEFT_KEY_UP, sizeof(LEFT_KEY_UP) - 1) == 0) {
            if (turnDirection == -1)
              turnDirection = 0;
          } else if (strncmp(buf, RIGHT_KEY_UP,
                             sizeof(RIGHT_KEY_UP) - 1) == 0) {
            if (turnDirection == 1)
              turnDirection = 0;
          } else {
            fprintf(stderr,
                    "Unknown message from GUI (length %zd), ignoring.\n%.*s\n",
                    readBytes, (int) readBytes, buf);
          }

          if (DEBUG)
            fprintf(stderr, "turnDirection: %d\n", turnDirection);
        }
      }
    }
  }

  checkSysError(close(sockets[1].fd), "close socket to GUI");

  exit(EXIT_SUCCESS);
}