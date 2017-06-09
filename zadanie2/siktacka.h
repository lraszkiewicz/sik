#ifndef ZADANIE2_SIKTACKA_H
#define ZADANIE2_SIKTACKA_H

#include <cstdint>


const int PLAYER_NAME_MAX_LENGTH = 64;

// Datagram definitions to be used by both client and server.

// Fields that are commented out have to be manually parsed/added
// because of their variable byte lengths.


struct __attribute__((__packed__)) ClientToServerDatagram {
  uint64_t sessionId;
  int8_t turnDirection;
  int32_t nextExpectedEventNumer;
  uint8_t playerName[];
};


struct ServerToClientDatagramHeader {
  uint32_t gameId;
  // events
};

enum EventType : uint8_t {
  NEW_GAME = 0,
  PIXEL = 1,
  PLAYER_ELIMINATED = 2,
  GAME_OVER = 3
};


struct EventHeader {
  int32_t len;
  uint32_t eventNumber;
  EventType eventType;
  // eventData
  // crc32
};

struct NewGameEventDataHeader {
  uint32_t width;
  uint32_t height;
  // playerNames
};

struct PixelEventData {
  uint8_t playerNumber;
  uint32_t x;
  uint32_t y;
};

struct PlayerEliminatedEventData {
  uint8_t playerNumber;
};

#endif //ZADANIE2_SIKTACKA_H
