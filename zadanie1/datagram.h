#ifndef ZADANIE1_PACKET_H
#define ZADANIE1_PACKET_H

#include <cstdint>

#define MAX_FILE_LENGTH 65000

struct small_datagram_t {
  uint64_t timestamp;
  char c;
};

struct datagram_with_file_t {
  uint64_t timestamp;
  char c;
  char file_content[MAX_FILE_LENGTH];
};

#endif //ZADANIE1_PACKET_H
