#include "rvrbotron/dsp/Reverb.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>

namespace {

bool countAllocations = false;
std::size_t allocationCount = 0;

void recordAllocation() noexcept {
  if (countAllocations) {
    ++allocationCount;
  }
}

void* allocate(const std::size_t size) {
  recordAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void beginAllocationCount() noexcept {
  allocationCount = 0;
  countAllocations = true;
}

std::size_t endAllocationCount() noexcept {
  countAllocations = false;
  return allocationCount;
}

} // namespace

void* operator new(const std::size_t size) {
  return allocate(size);
}

void* operator new[](const std::size_t size) {
  return allocate(size);
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

int main() {
  beginAllocationCount();
  void* observedAllocation = ::operator new(1);
  ::operator delete(observedAllocation);
  if (endAllocationCount() != 1) {
    std::cerr << "test allocation counter did not observe operator new\n";
    return 1;
  }

  const rvrbotron::dsp::ResolvedConfig config{
      1,
      0,
      48000,
      {},
  };
  rvrbotron::dsp::Reverb reverb(config);

  std::array<rvrbotron::dsp::Sample, 7> left{
      0.5F,
      -0.25F,
      0.0F,
      1.0F,
      -0.75F,
      0.125F,
      -1.0F,
  };
  std::array<rvrbotron::dsp::Sample, 7> right{
      -0.5F,
      0.25F,
      1.0F,
      0.0F,
      0.75F,
      -0.125F,
      -1.0F,
  };
  const auto expectedLeft = left;
  const auto expectedRight = right;
  rvrbotron::dsp::Sample* firstBlock[]{left.data(), right.data()};
  rvrbotron::dsp::Sample* finalBlock[]{left.data() + 3, right.data() + 3};

  beginAllocationCount();
  reverb.process(firstBlock, 2, 3);
  reverb.process(finalBlock, 2, 4);
  const auto processingAllocations = endAllocationCount();

  if (processingAllocations != 0) {
    std::cerr << "configured Composition allocated while processing\n";
    return 1;
  }
  if (left != expectedLeft || right != expectedRight) {
    std::cerr << "empty Composition changed caller-owned samples\n";
    return 1;
  }

  return 0;
}
