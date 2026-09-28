#pragma once
#include "provider.h"

namespace rcx {

class NullProvider : public Provider {
public:
    int  size() const override { return 0; }
    bool read(uint64_t, void*, int) const override { return false; }
    // name() returns "" via the base default: the address bar's source chip
    // then shows the plug icon with "Select source" (no liveness dot), and
    // clicking it opens the source chooser. kind() returns "File" via the
    // base default.
};

} // namespace rcx
