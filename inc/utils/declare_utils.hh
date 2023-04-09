#pragma once

#define no_copy_assignable(x) \
    x(const x&) = delete; \
    x& operator=(const x&) = delete;

#define no_moveable(x) \
    x(x&&) = delete; \
    x& operator=(x&&) = delete;