#ifndef LMS4XXX_SPSC_RING_BUFFER_H
#define LMS4XXX_SPSC_RING_BUFFER_H

#include "lms4xxx_frame_receiver.h"
#include "ring_buffer.h"


namespace LMS4xxx {

	// Ring buffer type used between receive thread and parse thread.
	// Capacity should be >= 1024 frames for zero-loss at 600 Hz.
	using FrameRingBuffer = Common::RingBuffer<RawFrame>;

}  // namespace LMS4xxx

#endif	// LMS4XXX_SPSC_RING_BUFFER_H
