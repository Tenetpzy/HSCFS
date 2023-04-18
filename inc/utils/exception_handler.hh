#pragma once

#include <stdexcept>

namespace hscfs {

/*
 * C与C++异常处理的交接
 * 根据异常对象的类型，返回对应的错误码
 */
int handle_exception(const std::exception &e);

}  // namespace hscfs