// SHA-256 (FIPS 180-4), for the badge record's checksum on any board.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace badge {

class Sha256 {
 public:
  Sha256();
  void update(const uint8_t* data, size_t n);
  void finish(uint8_t out[32]);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t used_ = 0;
  uint64_t bits_ = 0;
};

}  // namespace badge
