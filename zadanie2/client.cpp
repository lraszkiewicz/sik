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
#include <zlib.h>
#include <set>

#include "siktacka.h"
#include "util.h"

using namespace std;

const bool DEBUG = false;

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
        syserr("getaddrinfo (%s): %s (%d)",
               what.c_str(), strerror(ret), ret);
      } else {
        fatal("getaddrinfo (%s): %s (%d)",
              what.c_str(), strerror(ret), ret);
      }
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
  if (serverAddrInfo->ai_family == AF_INET)
    ((sockaddr_in *) serverAddrInfo->ai_addr)->sin_port = htons(serverPort);
  else
    ((sockaddr_in6 *) serverAddrInfo->ai_addr)->sin6_port = htons(serverPort);

  sockets[0].fd = socket(serverAddrInfo->ai_family, SOCK_DGRAM, 0);
  checkSysError(sockets[0].fd, "socket to server");
  sockets[0].events = POLLIN;
  sockets[0].revents = 0;

  // TCP sockets for GUI connection.
  if (guiAddrInfo->ai_family == AF_INET)
    ((sockaddr_in *) guiAddrInfo->ai_addr)->sin_port = htons(guiPort);
  else if (guiAddrInfo->ai_family == AF_INET6)
    ((sockaddr_in6 *) guiAddrInfo->ai_addr)->sin6_port = htons(guiPort);

  sockets[1].fd = socket(guiAddrInfo->ai_family, SOCK_STREAM, IPPROTO_TCP);
  checkSysError(sockets[1].fd, "socket to GUI");
  // Disable Nagle's algorithm on this socket.
  int flagNagle = 1;
  setsockopt(sockets[1].fd, IPPROTO_TCP, TCP_NODELAY,
             &flagNagle, sizeof(flagNagle));
  sockets[1].events = POLLIN;
  sockets[1].revents = 0;
  checkSysError(connect(sockets[1].fd, guiAddrInfo->ai_addr,
                        guiAddrInfo->ai_addrlen),
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
  bool rightKeyDown = false, leftKeyDown = false;
  uint32_t nextEventNumber = 0;
  uint64_t nextSendToServer = sessionId;
  vector<string> playerNames;
  uint32_t currentGameId = 0, width = 0, height = 0;
  set<pair<int,int>> events; // {gameId, eventNumber}
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
                 serverAddrInfo->ai_addr, serverAddrInfo->ai_addrlen);
      // If sendto would block, don't set the non-blocking flag.
      if (sendtoRet == EAGAIN || sendtoRet == EWOULDBLOCK) {
        fprintf(stderr, "Sendto would block, attempting without flags.\n");
        sendtoRet = sendto(sockets[0].fd, sendBuf, sendBufSize, 0,
                           serverAddrInfo->ai_addr, serverAddrInfo->ai_addrlen);
      }
      if (sendtoRet != (ssize_t) sendBufSize)
        syserr("sendto");

      nextSendToServer += DELAY * 1000;
    } else {
      // Try to recieve some data.
      sockets[0].revents = 0;
      sockets[1].revents = 0;
      int pollRet =
          poll(sockets, 2, (int) ((nextSendToServer - currentTime) / 1000));
      if (pollRet < 0 && errno == EINTR) {
        fprintf(stderr, "Poll interrupted, finishing.\n");
        finish = true;
        break;
      } else {
        checkSysError(pollRet, "poll");
      }
      if (pollRet > 0) {
        if (sockets[0].revents & POLLIN) {
          // Recieve and parse data from server, send the events to GUI.
          char messageToGui[BUF_TO_GUI_SIZE];
          int messageToGuiLength = 0;
          uint8_t buf[MAX_DATAGRAM_SIZE + 5];
          ssize_t eventStart = sizeof(ServerToClientDatagramHeader);

          ssize_t recvSize = recv(sockets[0].fd, buf, sizeof(buf), 0);
          if (DEBUG)
            fprintf(stderr, "Recieved %zd bytes from server.\n", recvSize);

          if (recvSize < (ssize_t) sizeof(ServerToClientDatagramHeader)) {
            fprintf(stderr, "Datagram from server too small, ignoring.\n");
            continue;
          } else if (recvSize > MAX_DATAGRAM_SIZE) {
            fprintf(stderr, "Datagram from server too big, ignoring.\n");
            continue;
          }

          uint32_t gameId = ntohl(*((uint32_t *) buf));
          if (DEBUG)
            fprintf(stderr, "Game ID: %u\n", gameId);
          while (eventStart < recvSize) {
            EventHeader *eventHeader = (EventHeader *) (buf + eventStart);

            if (recvSize - eventStart
                // (len, eventNumber, eventType), crc32
                < (ssize_t) (sizeof(EventHeader) + sizeof(uint32_t))) {
              fprintf(stderr, "Event too small, ignoring.\n");
              break;
            }

            uint32_t eventLen = ntohl(eventHeader->len);

            if (eventStart + eventLen > recvSize) {
              fprintf(stderr,
                  "Declared event len bigger than recieved data, ignoring.");
              break;
            }

            uint32_t crcCalculated = (uint32_t)
                crc32(0, buf + eventStart, eventLen + sizeof(uint32_t));
            uint32_t crcDownloaded =
                ntohl(*((uint32_t *)(buf + eventStart + eventLen
                                     + sizeof(uint32_t))));
            if (DEBUG) {
              fprintf(stderr,
                      "Event:\ncalculated CRC32: %u\ndownloaded CRC32: %u\n",
                      crcCalculated,
                      crcDownloaded);
            }
            if (crcDownloaded != crcCalculated) {
              fprintf(stderr, "Invalid CRC32 checksum, ignoring.\n");
              break;
            }

            if (DEBUG)
              fprintf(stderr, "length: %u\nnumber: %u\n",
                      ntohl(eventHeader->len), ntohl(eventHeader->eventNumber));

            if (ntohl(eventHeader->eventNumber) + 1 > nextEventNumber
                || eventHeader->eventType == NEW_GAME)
              nextEventNumber = ntohl(eventHeader->eventNumber) + 1;

            bool duplicate = false;
            if (events.find({gameId, ntohl(eventHeader->eventNumber)})
                != events.end())
              // Event is a duplicate, don't send it to GUI.
              duplicate = true;

            events.insert({gameId, ntohl(eventHeader->eventNumber)});

            eventStart += sizeof(EventHeader);

            if (eventHeader->eventType == NEW_GAME) {
              fprintf(stderr, "New game.\n");

              // event_no + event_type + event_data
              if (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(NewGameEventData)
                  > eventLen)
                fatal("Declared event len is too short, exiting.");
              if (!duplicate) {
                currentGameId = gameId;
                playerNames.clear();
              }
              NewGameEventData *eventDataHeader =
                  (NewGameEventData *) (buf + eventStart);
              width = ntohl(eventDataHeader->width);
              height = ntohl(eventDataHeader->height);
              if (!duplicate)
                messageToGuiLength +=
                    sprintf(messageToGui + messageToGuiLength,
                            "NEW_GAME %" PRIu32 " %" PRIu32 " ",
                            ntohl(eventDataHeader->width),
                            ntohl(eventDataHeader->height));
              size_t playerListLen = ntohl(eventHeader->len)
                                     - sizeof(EventHeader)
                                     + sizeof(uint32_t) // len itself
                                     - sizeof(NewGameEventData);
              if (!duplicate) {
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
              }
              eventStart += sizeof(NewGameEventData) + playerListLen;

            } else if (eventHeader->eventType == PIXEL) {
              PixelEventData *eventData = (PixelEventData *) (buf + eventStart);
              if (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(PixelEventData)
                  > eventLen)
                fatal("Declared event len is too short, exiting.");
              if (gameId == currentGameId) {
                if (ntohl(eventData->x) > width || ntohl(eventData->y) > height)
                  fatal("Pixel coordinates out of bounds, exiting.");
                if (eventData->playerNumber >= playerNames.size())
                  fatal("Player number doesn't exist, exiting.");
                if (!duplicate)
                  messageToGuiLength +=
                      sprintf(messageToGui + messageToGuiLength,
                              "PIXEL %" PRIu32 " %" PRIu32 " %s\n",
                              ntohl(eventData->x),
                              ntohl(eventData->y),
                              playerNames[eventData->playerNumber].c_str());
              }
              eventStart += sizeof(PixelEventData);

            } else if (eventHeader->eventType == PLAYER_ELIMINATED) {
              if (sizeof(uint32_t) + sizeof(uint8_t)
                  + sizeof(PlayerEliminatedEventData) > eventLen)
                fatal("Declared event len is too short, exiting.");
              PlayerEliminatedEventData *eventData =
                  (PlayerEliminatedEventData *) (buf + eventStart);
              if (eventData->playerNumber >= playerNames.size())
                fatal("Player number doesn't exist, exiting.");
              if (gameId == currentGameId && !duplicate)
                messageToGuiLength +=
                    sprintf(messageToGui + messageToGuiLength,
                            "PLAYER_ELIMINATED %s\n",
                            playerNames[eventData->playerNumber].c_str());
              eventStart += sizeof(PlayerEliminatedEventData);

            } else if (eventHeader->eventType == GAME_OVER) {
              fprintf(stderr, "Game over!\n");
            } else {
              fprintf(stderr, "Unknown event type, ignoring.\n");
            }

            eventStart += sizeof(uint32_t);  // CRC32
          }
          if (DEBUG)
            fprintf(stderr, "%.*s", messageToGuiLength, messageToGui);
          if (messageToGuiLength > 0)
            checkSysError((int) write(sockets[1].fd, messageToGui,
                                      (size_t) messageToGuiLength),
                          "write to GUI");
        }

        if (sockets[1].revents & POLLIN) {
          // Recieve data from GUI.
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
            leftKeyDown = true;
            turnDirection = -1;
          } else if (strncmp(buf, RIGHT_KEY_DOWN,
                             sizeof(RIGHT_KEY_DOWN) - 1) == 0) {
            rightKeyDown = true;
            turnDirection = 1;
          } else if (strncmp(buf, LEFT_KEY_UP, sizeof(LEFT_KEY_UP) - 1) == 0) {
            leftKeyDown = false;
            if (rightKeyDown)
              turnDirection = 1;
            else
              turnDirection = 0;
          } else if (strncmp(buf, RIGHT_KEY_UP,
                             sizeof(RIGHT_KEY_UP) - 1) == 0) {
            rightKeyDown = false;
            if (leftKeyDown)
              turnDirection = -1;
            else
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

  free(sendBuf);
  freeaddrinfo(serverAddrInfo);
  checkSysError(close(sockets[1].fd), "close socket to GUI");

  exit(EXIT_SUCCESS);
}
