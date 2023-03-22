#pragma once

#define no_copy_assignable(x) \
    x(const x&) = delete; \
    x& operator=(const x&) = delete;
