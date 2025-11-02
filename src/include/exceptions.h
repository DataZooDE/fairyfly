#pragma once

#include <stdexcept>
#include <string>

namespace fairyfly {

/// Base class for all fairyfly exceptions
class FairyflyException : public std::runtime_error {
public:
    explicit FairyflyException(const std::string& message)
        : std::runtime_error(message) {}
};

/// User input errors (invalid arguments, bad config, etc.)
/// These should result in helpful error messages without stack traces
class UserError : public FairyflyException {
public:
    explicit UserError(const std::string& message)
        : FairyflyException(message) {}
};

/// System/internal errors (unexpected failures, bugs, etc.)
/// These should be logged with full context for debugging
class SystemError : public FairyflyException {
public:
    explicit SystemError(const std::string& message)
        : FairyflyException(message) {}
};

} // namespace fairyfly
