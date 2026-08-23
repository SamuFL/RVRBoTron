#include "rvrbotron/cli/FileSha256.h"

#include "rvrbotron/HarnessError.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace rvrbotron::cli {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

std::uint32_t rotateRight(const std::uint32_t value,
                          const unsigned int count) noexcept {
  return (value >> count) | (value << (32 - count));
}

class Sha256 {
public:
  void update(const std::uint8_t* bytes, std::size_t byteCount) {
    totalBytes_ += byteCount;
    while (byteCount > 0) {
      const auto copied =
          std::min(byteCount, buffer_.size() - bufferedBytes_);
      std::copy_n(bytes, copied, buffer_.data() + bufferedBytes_);
      bufferedBytes_ += copied;
      bytes += copied;
      byteCount -= copied;
      if (bufferedBytes_ == buffer_.size()) {
        transform(buffer_.data());
        bufferedBytes_ = 0;
      }
    }
  }

  std::array<std::uint8_t, 32> finish() {
    const auto bitCount = totalBytes_ * 8;
    const std::uint8_t marker = 0x80;
    update(&marker, 1);
    const std::uint8_t zero = 0;
    while (bufferedBytes_ != 56) {
      update(&zero, 1);
    }

    std::array<std::uint8_t, 8> encodedBitCount{};
    for (std::size_t index = 0; index < encodedBitCount.size(); ++index) {
      encodedBitCount[encodedBitCount.size() - 1 - index] =
          static_cast<std::uint8_t>(bitCount >> (index * 8));
    }
    update(encodedBitCount.data(), encodedBitCount.size());

    std::array<std::uint8_t, 32> digest{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
      for (std::size_t byte = 0; byte < 4; ++byte) {
        digest[index * 4 + byte] =
            static_cast<std::uint8_t>(
                state_[index] >> ((3 - byte) * 8));
      }
    }
    return digest;
  }

private:
  void transform(const std::uint8_t* block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      words[index] =
          static_cast<std::uint32_t>(block[index * 4]) << 24 |
          static_cast<std::uint32_t>(block[index * 4 + 1]) << 16 |
          static_cast<std::uint32_t>(block[index * 4 + 2]) << 8 |
          static_cast<std::uint32_t>(block[index * 4 + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto first =
          rotateRight(words[index - 15], 7) ^
          rotateRight(words[index - 15], 18) ^
          (words[index - 15] >> 3);
      const auto second =
          rotateRight(words[index - 2], 17) ^
          rotateRight(words[index - 2], 19) ^
          (words[index - 2] >> 10);
      words[index] =
          words[index - 16] + first + words[index - 7] + second;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sumOne =
          rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temporaryOne =
          h + sumOne + choose + kRoundConstants[index] + words[index];
      const auto sumZero =
          rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporaryTwo = sumZero + majority;

      h = g;
      g = f;
      f = e;
      e = d + temporaryOne;
      d = c;
      c = b;
      b = a;
      a = temporaryOne + temporaryTwo;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t bufferedBytes_{0};
  std::uint64_t totalBytes_{0};
};

} // namespace

std::string fileSha256(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw HarnessError(
        ErrorCategory::ioFailure,
        "could not open input for hashing: " + path.string());
  }

  Sha256 hash;
  std::array<std::uint8_t, 8192> buffer{};
  while (input) {
    input.read(
        reinterpret_cast<char*>(buffer.data()),
        static_cast<std::streamsize>(buffer.size()));
    const auto bytesRead = input.gcount();
    if (bytesRead > 0) {
      hash.update(buffer.data(), static_cast<std::size_t>(bytesRead));
    }
  }
  if (!input.eof()) {
    throw HarnessError(
        ErrorCategory::ioFailure,
        "could not read input for hashing: " + path.string());
  }

  const auto digest = hash.finish();
  std::ostringstream encoded;
  encoded << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    encoded << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return encoded.str();
}

} // namespace rvrbotron::cli
