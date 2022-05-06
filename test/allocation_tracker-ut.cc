// Unless explicitly stated otherwise all files in this repository are licensed
// under the Apache License Version 2.0. This product includes software
// developed at Datadog (https://www.datadoghq.com/). Copyright 2021-Present
// Datadog, Inc.
#include "allocation_tracker.hpp"

#include "ipc.hpp"
#include "perf_watcher.h"
#include "pevent_lib.h"
#include "ringbuffer_utils.hpp"
#include "syscalls.hpp"
#include "unwind.hpp"
#include "unwind_state.hpp"

#include <gtest/gtest.h>
#include <sys/syscall.h>
#include <unistd.h>

__attribute__((noinline)) void my_malloc(size_t size) {
  ddprof::AllocationTracker::track_allocation(0xdeadbeef, size);
  // prevent tail call optimization
  getpid();
}

extern "C" {
__attribute__((noinline)) void my_func_calling_malloc(size_t size) {
  my_malloc(size);
  // prevent tail call optimization
  getpid();
}
}

class RingBufferHolder {
public:
  explicit RingBufferHolder(size_t buffer_size_order) {
    pevent_init(&_pevent_hdr);
    _pevent_hdr.size = 1;
    EXPECT_TRUE(IsDDResOK(pevent_create_custom_ring_buffer(&_pevent_hdr.pes[0],
                                                           buffer_size_order)));
    EXPECT_TRUE(IsDDResOK(pevent_mmap(&_pevent_hdr, true)));
  }

  ~RingBufferHolder() { pevent_cleanup(&_pevent_hdr); }

  ddprof::RingBufferInfo get_buffer_info() const {
    const auto &pe = _pevent_hdr.pes[0];
    return {static_cast<int64_t>(pe.rb.size), pe.mapfd, pe.fd};
  }

  RingBuffer &get_ring_buffer() { return _pevent_hdr.pes[0].rb; }

private:
  PEventHdr _pevent_hdr;
};

TEST(allocation_tracker, start_stop) {
#ifdef __x86_64__
  const uint64_t rate = 1;
  const size_t buf_size_order = 5;
  RingBufferHolder ring_buffer{buf_size_order};
  ddprof::AllocationTracker::allocation_tracking_init(
      rate, ddprof::AllocationTracker::kDeterministicSampling,
      ring_buffer.get_buffer_info());

  my_func_calling_malloc(1);

  ddprof::RingBufferReader reader{ring_buffer.get_ring_buffer()};
  ASSERT_GT(reader.available_size(), 0);

  auto buf = reader.read_all_available();
  const perf_event_header *hdr =
      reinterpret_cast<const perf_event_header *>(buf.data());
  ASSERT_EQ(hdr->type, PERF_RECORD_SAMPLE);

  perf_event_sample *sample = hdr2samp(hdr, perf_event_default_sample_type());

  ASSERT_EQ(sample->period, 1);
  ASSERT_EQ(sample->pid, getpid());
  ASSERT_EQ(sample->tid, ddprof::gettid());

  UnwindState state;
  ddprof::unwind_init_sample(&state, sample->regs, sample->pid,
                             sample->size_stack, sample->data_stack);
  ddprof::unwindstate__unwind(&state);

  const auto &symbol_table = state.symbol_hdr._symbol_table;
  ASSERT_GT(state.output.nb_locs, NB_FRAMES_TO_SKIP);
  const auto &symbol =
      symbol_table[state.output.locs[NB_FRAMES_TO_SKIP]._symbol_idx];
  ASSERT_EQ(symbol._symname, "my_func_calling_malloc");

  ddprof::AllocationTracker::allocation_tracking_free();
#endif
}