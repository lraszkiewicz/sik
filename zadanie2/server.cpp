#include <ctime>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>
#include <cmath>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <arpa/inet.h>
#include <cinttypes>
#include <map>
#include <set>
#include <algorithm>
#include <zlib.h>

#include "siktacka.h"
#include "util.h"

using namespace std;

const bool DEBUG = true;

uint32_t WIDTH = 800,
         HEIGHT = 600,
         ROUNDS_PER_SEC = 50,
         TURNING_SPEED = 6;
uint16_t PORT = 12345;

const uint64_t randConst1 = 279470273, randConst2 = 4294967291;
uint32_t lastRandom;

uint32_t getRandom() {
  lastRandom = (uint32_t) ((((uint64_t) lastRandom) * randConst1) % randConst2);
  return (uint32_t) lastRandom;
}

class Event {
public:
  Event(uint32_t _eventNumber, EventType _eventType, uint32_t _x,
        uint32_t _y, uint8_t _playerNumber, vector<string> _playerNames)
      : eventNumber(_eventNumber), eventType(_eventType), x(_x),
        y(_y), playerNumber(_playerNumber), playerNames(_playerNames) {};

  uint32_t eventNumber;
  EventType eventType;
  uint32_t x;  // maxx from NEW_GAME, x from PIXEL
  uint32_t y;  // maxy from NEW_GAME, y from PIXEL
  uint8_t playerNumber;  // PIXEL, PLAYER_ELIMINATED;
  vector<string> playerNames;  // NEW_GAME
};

vector<Event> events;

void putNewGameEvent(uint32_t maxx, uint32_t maxy, vector<string> playerNames) {
  fprintf(stderr, "New game: %u %u", maxx, maxy);
  for (string &s : playerNames)
    fprintf(stderr, " %s", s.c_str());
  fprintf(stderr, "\n");
  events.push_back(Event((uint32_t) events.size(), NEW_GAME,
                         maxx, maxy, 0, playerNames));
}

void putPixelEvent(uint8_t playerNumber, uint32_t x, uint32_t y) {
  if (DEBUG)
    fprintf(stderr, "Pixel: %u %u %u\n", playerNumber, x, y);
  events.push_back(Event((uint32_t) events.size(), PIXEL,
                         x, y, playerNumber, vector<string>()));
}

void putPlayerEliminatedEvent(uint8_t playerNumber) {
  fprintf(stderr, "Player eliminated: %u\n", playerNumber);
  events.push_back(Event((uint32_t) events.size(), PLAYER_ELIMINATED,
                         0, 0, playerNumber, vector<string>()));
}

void putGameOverEvent() {
  fprintf(stderr, "Game over\n");
  events.push_back(Event((uint32_t) events.size(), GAME_OVER,
                         0, 0, 0, vector<string>()));
}

pollfd sock;

// Compares two IPv6 addresses, returns true if equal.
bool compareAddr(in6_addr *addr1, in6_addr *addr2) {
  for (size_t i = 0; i < sizeof(addr1->s6_addr); ++i)
    if (addr1->s6_addr[i] != addr2->s6_addr[i])
      return false;
  return true;
}

// Copy in6_addr from src to dest.
void copyAddr(in6_addr *dest, const in6_addr *src) {
  memcpy(dest->s6_addr, src->s6_addr, sizeof(src->s6_addr));
}


class Snake {
public:
  uint8_t number; // players without snakes don't need a number
  bool alive;
  long double x;
  long double y;
  long double angle;
  int turnDirection;  // -1: left, 0: straight, 1: right
};

class Player {
public:
  string name;

  bool ready;
  bool hasSnake;
  Snake snake;
  uint64_t lastReceiveTime;
  bool disconnected; // true when same client connects with a higher session ID

  uint64_t sessionId;
  uint32_t nextExpectedEvent;
  in6_addr addr;
  in_port_t port;

  bool operator < (const Player &p) const {
    if (name == p.name)
      return sessionId < p.sessionId;
    return name < p.name;
  }
};

uint8_t alivePlayers;

// A pair {x,y} is in the set when the pixel was already taken by a player.
set<pair<int,int>> board;

bool isPixelTaken(int x, int y) {
  return board.find({x,y}) != board.end();
}

// Puts a pixel on the player's current position or eliminates them.
void createPixel(Snake &snake) {
  uint32_t x = (uint32_t) snake.x, y = (uint32_t) snake.y;
  if (snake.x < 0 || x >= WIDTH || snake.y < 0 || y >= HEIGHT
      || isPixelTaken(x,y)) {
    // Out of bounds or pixel already taken - eliminate the player.
    snake.alive = false;
    --alivePlayers;
    putPlayerEliminatedEvent(snake.number);
  } else {
    board.insert({x,y});
    putPixelEvent(snake.number, x, y);
  }
}

void moveSnake(Snake &snake) {
  snake.angle += snake.turnDirection * ((long double) TURNING_SPEED);
  uint32_t oldX = (uint32_t) snake.x, oldY = (uint32_t) snake.y;
  snake.x -= cos(snake.angle * M_PIl / 180.0);
  snake.y -= sin(snake.angle * M_PIl / 180.0);
  if ((uint32_t) snake.x != oldX || (uint32_t) snake.y != oldY)
    createPixel(snake);
}

vector<Player> players;
vector<Snake> snakes;

// Returns true if all (and at least two) players with a unique name are ready.
// If a name is duplicated, only first one on the list is checked and can play.
bool isEveryoneReady() {
  uint32_t readyPlayers = 0;
  set<string> usedNames;
  for (Player &p : players) {
    if (!p.name.empty() && usedNames.find(p.name) == usedNames.end()) {
      usedNames.insert(p.name);
      if (!p.ready) {
        return false;
      }
      ++readyPlayers;
    }
  }
  return readyPlayers > 1;
}

// Initialize snakes when a new game starts.
void onGameStart() {
  if (DEBUG)
    fprintf(stderr, "Starting new game.\n");
  events.clear();
  board.clear();
  set<string> usedNames;
  vector<string> playerNames;
  size_t totalPlayerNameLength = 0;
  size_t totalNameLengthLimit =
      MAX_DATAGRAM_SIZE - sizeof(ServerToClientDatagramHeader)
      - sizeof(EventHeader) - sizeof(NewGameEventData) - sizeof(uint32_t);
  snakes.clear();
  alivePlayers = 0;
  bool moreAllowed = true;
  for (Player &p : players) {
    if (!p.name.empty()
        && usedNames.find(p.name) == usedNames.end()
        && moreAllowed) {
      totalPlayerNameLength += p.name.length() + 1;
      if (totalPlayerNameLength <= totalNameLengthLimit) {
        usedNames.insert(p.name);
        playerNames.push_back(p.name);
        p.nextExpectedEvent = 0;
        p.hasSnake = true;
        p.snake.x = ((long double) (getRandom() % WIDTH)) + 0.5;
        p.snake.y = ((long double) (getRandom() % HEIGHT)) + 0.5;
        p.snake.angle = getRandom() % 360;
        p.snake.number = alivePlayers;
        p.snake.alive = true;
        ++alivePlayers;
      } else {
        // Total player name length too big - don't allow more players.
        p.hasSnake = false;
        moreAllowed = false;
      }
    } else {
      p.hasSnake = false;
    }
  }
  putNewGameEvent(WIDTH, HEIGHT, playerNames);
  for (Player &p : players)
    if (p.hasSnake)
      createPixel(p.snake);
}

void onGameOver() {
  for (Player &p : players) {
    p.hasSnake = false;
    p.ready = false;
  }
  putGameOverEvent();
}

// Delete inactive players who don't have snakes.
void deleteInactive() {
  for (auto p = players.begin(); p != players.end(); ) {
    if ((!p->hasSnake)
        && (getCurrentTime() - p->lastReceiveTime > 2'000'000
            || p->disconnected)) {
      p = players.erase(p);
      continue;
    }
    ++p;
  }
}

// Send events to a player/observer according to their nextExpectedEvent.
void sendEventsToPlayer(Player &player, uint32_t gameId) {
  if (player.nextExpectedEvent >= events.size())
    // Nothing to send.
    return;

  if (DEBUG)
    fprintf(stderr, "Sending events to player: %d %s\n",
            player.hasSnake ? player.snake.number : -1,
            player.name.c_str());

  uint8_t datagram[MAX_DATAGRAM_SIZE];
  size_t datagramLen = sizeof(ServerToClientDatagramHeader);
  ((ServerToClientDatagramHeader *) &datagram)->gameId = htonl(gameId);
  for (int i = player.nextExpectedEvent; i < events.size(); ++i) {
    Event *event = &events[i];

    if (DEBUG) {
      fprintf(stderr, "Event %u - ", event->eventNumber);
      switch (event->eventType) {
        case NEW_GAME:
          fprintf(stderr, "new game %u %u", event->x, event->y);
          for (string &s : event->playerNames)
            fprintf(stderr, " %s", s.c_str());
          fprintf(stderr, "\n");
          break;
        case PIXEL:
          fprintf(stderr, "pixel %u %u %u\n",
                  event->playerNumber, event->x, event->y);
          break;
        case PLAYER_ELIMINATED:
          fprintf(stderr, "player eliminated %u\n", event->playerNumber);
          break;
        case GAME_OVER:
          fprintf(stderr, "game over\n");
          break;
      }
    }

    size_t thisEventSize = sizeof(EventHeader) + sizeof(uint32_t); // + crc32
    switch (event->eventType) {
      case NEW_GAME:
        thisEventSize += sizeof(NewGameEventData);
        for (string &s : event->playerNames)
          thisEventSize += s.length() + 1;
        break;
      case PIXEL:
        thisEventSize += sizeof(PixelEventData); break;
      case PLAYER_ELIMINATED:
        thisEventSize += sizeof(PlayerEliminatedEventData); break;
      default:
        break;
    }
    if (datagramLen + thisEventSize > MAX_DATAGRAM_SIZE)
      // Event won't fit, don't add it.
      break;

    // Now the event can be safely added to the datagram.
    size_t eventStart = datagramLen;
    EventHeader *header = (EventHeader *)(datagram + datagramLen);
    header->len = htonl((uint32_t) thisEventSize - 2 * sizeof(uint32_t));
    header->eventNumber = htonl(event->eventNumber);
    header->eventType = event->eventType;
    datagramLen += sizeof(EventHeader);

    switch (event->eventType) {
      case NEW_GAME: {
        NewGameEventData *data = (NewGameEventData *) (datagram + datagramLen);
        data->width = htonl(event->x);
        data->height = htonl(event->y);
        datagramLen += sizeof(NewGameEventData);
        size_t j = 0;
        for (string &s : event->playerNames) {
          for (size_t k = 0; k < s.length(); ++k) {
            fprintf(stderr, "data->playernames[%zu] = %c\n", j, s[k]);
            data->playerNames[j] = s[k];
            ++j;
          }
          data->playerNames[j] = 0;
          ++j;
        }
        datagramLen += j;
        break;
      }
      case PIXEL: {
        PixelEventData *data = (PixelEventData *) (datagram + datagramLen);
        data->playerNumber = event->playerNumber;
        data->x = htonl(event->x);
        data->y = htonl(event->y);
        datagramLen += sizeof(PixelEventData);
        break;
      }
      case PLAYER_ELIMINATED: {
        PlayerEliminatedEventData *data =
            (PlayerEliminatedEventData *) (datagram + datagramLen);
        data->playerNumber = event->playerNumber;
        datagramLen += sizeof(PlayerEliminatedEventData);
        break;
      }
      default:
        break;
    }
    *((uint32_t *)(datagram + datagramLen)) = htonl((uint32_t)
        crc32(0, datagram + eventStart, (uint32_t) (datagramLen - eventStart)));
    datagramLen += sizeof(uint32_t);

    ++player.nextExpectedEvent;
  }

  sockaddr_in6 toAddr;
  toAddr.sin6_addr = player.addr;
  toAddr.sin6_port = player.port;
  toAddr.sin6_scope_id = 0;
  toAddr.sin6_flowinfo = 0;
  toAddr.sin6_family = AF_INET6;

  ssize_t sentBytes = sendto(sock.fd, datagram, datagramLen, 0,
                             (sockaddr *) &toAddr, sizeof(toAddr));

  if (DEBUG)
    fprintf(stderr, "Sent %zd bytes to port %u\n",
            sentBytes, ntohs(toAddr.sin6_port));
  if (sentBytes < datagramLen) {
    fprintf(stderr, "Sending events not successful.\n");
    checkNonFatal(-1, "");
  }

  // If all events didn't fit in this datagram, just call this function again.
  // It won't do anything if everything was sent.
  sendEventsToPlayer(player, gameId);
}

void sendEvents(uint32_t gameId) {
  for (Player &player : players)
    sendEventsToPlayer(player, gameId);
}

int main(int argc, char *argv[]) {
  lastRandom = (uint32_t) time(NULL);
  // parse command line arguments
  int option;
  while ((option = getopt(argc, argv, "W:H:p:s:t:r:")) != -1) {
    switch (option) {
      case 'W':
        WIDTH = parseUInt32(optarg);
        break;
      case 'H':
        HEIGHT = parseUInt32(optarg);
        break;
      case 'p':
        PORT = parseUInt16(optarg);
        break;
      case 's':
        ROUNDS_PER_SEC = parseUInt32(optarg);
        break;
      case 't':
        TURNING_SPEED = parseUInt32(optarg);
        break;
      case 'r':
        lastRandom = parseUInt32(optarg);
        break;
      default:
        fprintf(stderr, "Usage: %s [-W n] [-H n] [-p n] [-s n] [-t n] [-r n]\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }
  }

  fprintf(stderr,
          "Width: %u\nHeight: %u\nRounds per second: %u\n"
          "Turning speed: %u\nPort: %u\n",
          WIDTH, HEIGHT, ROUNDS_PER_SEC, TURNING_SPEED, PORT);

  sockaddr_in6 address6;
  address6.sin6_family = AF_INET6;
  address6.sin6_addr = in6addr_any;
  address6.sin6_port = htons(PORT);
  address6.sin6_flowinfo = 0;
  address6.sin6_scope_id = 0;
  sock.fd = socket(AF_INET6, SOCK_DGRAM, 0);
  checkSysError(sock.fd, "socket");
  sock.events = POLLIN;
  sock.revents = 0;
  int ipv6only = 0;
  checkSysError(setsockopt(sock.fd, IPPROTO_IPV6,
                           IPV6_V6ONLY, &ipv6only, sizeof(ipv6only)),
                "setsockopt");
  checkSysError(bind(sock.fd, (sockaddr *) &address6, sizeof(address6)),
                "bind");

  uint64_t nextRoundTime = getCurrentTime();
  uint64_t roundTime = 1'000'000 / ROUNDS_PER_SEC;

  bool gameInProgress = false;
  uint32_t gameId = 0;
  while (true) {
    uint64_t currentTime = getCurrentTime();

    if (currentTime >= nextRoundTime) {
      // Simulate a turn.
      if (!gameInProgress) {
        sort(players.begin(), players.end());
        deleteInactive();
        if (isEveryoneReady()) {
          // Start a new game.
          gameId = getRandom();
          fprintf(stderr, "New game id: %u\n", gameId);
          onGameStart();
          gameInProgress = true;
        }
      } else {
        for (Player &p : players) {
          if (p.hasSnake) {
            moveSnake(p.snake);
            if (alivePlayers < 2) {
              onGameOver();
              gameInProgress = false;
              break;
            }
          }
        }
        sendEvents(gameId);
      }
      nextRoundTime += roundTime;

    } else {
      // Recieve data.
      sock.revents = 0;
      int pollRet = poll(&sock, 1,
                         (int) ((nextRoundTime - currentTime) / 1000));
      checkNonFatal(pollRet, "poll");
      if (pollRet > 0 && sock.revents & POLLIN) {
        uint8_t buf[MAX_DATAGRAM_SIZE];
        sockaddr_in6 fromAddr;
        socklen_t fromAddrLen = sizeof(sockaddr_storage);
        ssize_t recvSize = recvfrom(sock.fd, &buf, sizeof(buf), 0,
                                    (sockaddr *) &fromAddr, &fromAddrLen);

        if (DEBUG) {
          char addrBuf[INET6_ADDRSTRLEN];
          inet_ntop(AF_INET6, &fromAddr.sin6_addr, addrBuf, sizeof(addrBuf));
          fprintf(stderr, "Recieved %zd bytes from [%s]:%u.\n",
                  recvSize, addrBuf, ntohs(fromAddr.sin6_port));
        }

        if (recvSize < 0) {
          checkNonFatal((int) recvSize, "recvfrom");
          continue;
        } else if (recvSize < sizeof(ClientToServerDatagram)
                   || recvSize > sizeof(ClientToServerDatagram) + 64) {
          fprintf(stderr, "Recieved datagram has incorrect size, ignoring.\n");
          continue;
        }

        ClientToServerDatagram *datagram = (ClientToServerDatagram *) buf;
        size_t playerNameLen = recvSize - sizeof(ClientToServerDatagram);

        if (DEBUG)
          fprintf(stderr,
                  "session ID: %" PRIu64 ", turn direction: %d, "
                  "next event: %d, player name: %.*s\n",
                  be64toh(datagram->sessionId), datagram->turnDirection,
                  ntohl(datagram->nextExpectedEventNumer),
                  (int) playerNameLen, datagram->playerName);

        bool ignoreThis = false;
        for (size_t i = 0; i < playerNameLen; ++i) {
          if (datagram->playerName[i] < 33 || datagram->playerName[i] > 126) {
            fprintf(stderr,
                    "Player name contains illegal character, ignoring.\n");
            ignoreThis = true;
            break;
          }
        }
        if(ignoreThis)
          continue;

        string playerName((char *) datagram->playerName, playerNameLen);

        Player *player = NULL;
        for (Player &p : players) {
          if (p.port == fromAddr.sin6_port
              && compareAddr(&fromAddr.sin6_addr, &p.addr)
              && p.name == playerName
              && !p.disconnected) {
            // It's the same client as saved.
            if (p.sessionId == be64toh(datagram->sessionId)) {
              // Same session ID as earlier - matching players.
              player = &p;
              break;
            } else if (p.sessionId < be64toh(datagram->sessionId)) {
              // Higher session ID - disconnect the old player
              // and create a new one later.
              p.disconnected = true;
            }
          }
        }
        if (player == NULL) {
          Player newPlayer;
          newPlayer.name = playerName;
          newPlayer.ready = false;
          newPlayer.hasSnake = false;
          newPlayer.disconnected = false;
          newPlayer.sessionId = be64toh(datagram->sessionId);
          newPlayer.nextExpectedEvent =
              ntohl(datagram->nextExpectedEventNumer);
          copyAddr(&newPlayer.addr, &fromAddr.sin6_addr);
          newPlayer.port = fromAddr.sin6_port;
          players.push_back(newPlayer);
          player = &newPlayer;
        }

        player->lastReceiveTime = getCurrentTime();
        player->snake.turnDirection = datagram->turnDirection;
        player->nextExpectedEvent = ntohl(datagram->nextExpectedEventNumer);

        if (!gameInProgress)
          player->ready = player->ready || (player->snake.turnDirection != 0);
      }
    }
  }
}