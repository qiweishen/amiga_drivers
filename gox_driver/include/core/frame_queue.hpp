#pragma once

// Bounded queue used as SPSC here: one acquisition thread pushes, one writer
// thread pops. The implementation lives in common (close()-and-drain EOS
// semantics are the key to not losing tail frames on shutdown); this alias
// keeps the existing jai:: call sites and tests unchanged.

#include "bounded_queue.h"

namespace jai {

	template<typename T>
	using BoundedQueue = Common::BoundedQueue<T>;

}  // namespace jai
