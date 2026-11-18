// One clock for the whole app: monotonic seconds, as the playback timeline reads time.
#pragma once

#include <chrono>

namespace wave {

inline double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace wave
