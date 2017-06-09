#include <ctime>
#include <cerrno>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/time.h>
#include <cmath>

#include "siktacka.h"
#include "util.h"

using namespace std;

uint32_t WIDTH = 800,
         HEIGHT = 600,
         PORT = 12345,
         ROUNDS_PER_SEC = 50,
         TURNING_SPEED = 6;

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
  events.push_back(Event((uint32_t) events.size(), NEW_GAME,
                         maxx, maxy, 0, playerNames));
}

void putPixelEvent(uint8_t playerNumber, uint32_t x, uint32_t y) {
  events.push_back(Event((uint32_t) events.size(), PIXEL,
                         x, y, playerNumber, vector<string>()));
}

void putPlayerEliminatedEvent(uint8_t playerNumber) {
  events.push_back(Event((uint32_t) events.size(), PLAYER_ELIMINATED,
                         0, 0, playerNumber, vector<string>()));
}

void putGameOverEvent() {
  events.push_back(Event((uint32_t) events.size(), GAME_OVER,
                         0, 0, 0, vector<string>()));
}

// playerNumber = -1 sends to all players.
void sendEvents(int startEventNumber, int playerNumber) {
  // TODO
}

class Player {
public:
  bool ready;  // pressed a button at the start of the round
  bool eliminated;
  string name;
  uint8_t number;
  int64_t lastReceiveTime;
  long double x;
  long double y;
  long double angle;
  int turnDirection;  // -1: left, 0: straight, 1: right
};

uint8_t nextPlayerNumber = 0;
vector<Player> players;
uint8_t playingPlayers, alivePlayers;

// Deletes inactive players, updates player numbers.
// Should be used only after a game ends.
void refreshPlayers() {
  vector<Player> newPlayers;
  uint64_t currentTime = getCurrentTime();
  nextPlayerNumber = 0;
  for (Player player : players) {
    if (currentTime - player.lastReceiveTime > 2'000'000)
      continue;
    player.ready = false;
    player.number = nextPlayerNumber;
    ++nextPlayerNumber;
    newPlayers.push_back(player);
  }
  players = newPlayers;
}

// A field is 'true' when already taken by a player.
vector<vector<bool>> board;

// Puts a pixel on the player's current position or eliminates them.
void createPixel(Player &player) {
  uint32_t x = (uint32_t) player.x, y = (uint32_t) player.y;
  if (player.x < 0 || x >= WIDTH || player.y < 0 || y >= HEIGHT
      || board[x][y]) {
    // Out of bounds or pixel already taken - eliminate the player.
    player.eliminated = true;
    --alivePlayers;
    putPlayerEliminatedEvent(player.number);
  } else {
    board[x][y] = true;
    putPixelEvent(player.number, x, y);
  }
}

void movePlayer(Player &player) {
  player.angle += player.turnDirection * ((long double) TURNING_SPEED);
  uint32_t oldX = (uint32_t) player.x, oldY = (uint32_t) player.y;
  player.x += cos(player.angle * M_PIl / 180.0);
  player.y -= sin(player.angle * M_PIl / 180.0);
  if ((uint32_t) player.x != oldX || (uint32_t) player.y != oldY) {
    createPixel(player);
  }
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
        PORT = parseUInt32(optarg);
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

  while (true) {
    // new game
    uint32_t gameId = getRandom();
    events.clear();

    board.clear();
    board.resize(WIDTH);
    for (vector<bool> &row : board)
      row.resize(HEIGHT, false);

    refreshPlayers();
    vector<string> playerNames;
    for (Player player : players)
      playerNames.push_back(player.name);
    putNewGameEvent(WIDTH, HEIGHT, playerNames);

    uint8_t readyPlayers = 0;

    // TODO

    playingPlayers = 0;
    alivePlayers = 0;

    for (Player &player : players) {
      if (!player.name.empty()) {
        ++playingPlayers;
        ++alivePlayers;
        player.x = ((long double) (getRandom() % WIDTH)) + 0.5;
        player.y = ((long double) (getRandom() % HEIGHT)) + 0.5;
        player.angle = getRandom() % 360;
        createPixel(player);
      }
    }



  }
}