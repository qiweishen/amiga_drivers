#ifndef THREAD_SAFE_QUEUE_H
#define THREAD_SAFE_QUEUE_H

// Backward-compatibility shim: re-export Common::ThreadSafeQueue as the
// local ThreadSafeQueue alias so existing LMS driver code compiles unchanged.
// New code should use Common::ThreadSafeQueue directly.
//
// The relative path is resolved via the lms4xxx_driver/include/ entry in the
// -I include search path: ../../common/include/ -> common/include/.
#include <../../common/include/thread_safe_queue.h>

template<typename T>
using ThreadSafeQueue = Common::ThreadSafeQueue<T>;


#endif // THREAD_SAFE_QUEUE_H