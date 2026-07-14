#pragma once

#include <string>
#include <variant>
#include <stdexcept>

/**
 * @file Result.h
 * @brief Result<T, E> type for expressive error handling (Elite Refactor #7)
 * 
 * Inspired by Rust's Result<T, E> and C++23's std::expected.
 * Eliminates bool returns that lose error context, replacing them with
 * rich error information that can be logged, propagated, or handled.
 * 
 * Example usage:
 *   Result<int> calculateSize() {
 *       if (error) return Error("Invalid input");
 *       return Ok(42);
 *   }
 * 
 *   auto result = calculateSize();
 *   if (result.isOk()) {
 *       int size = result.unwrap();
 *   } else {
 *       log(result.error());
 *   }
 */

// Error type with message and optional code
struct Error {
    std::string message;
    int code = 0;
    
    Error(const char* msg, int errorCode = 0) 
        : message(msg), code(errorCode) {}
    
    Error(const std::string& msg, int errorCode = 0) 
        : message(msg), code(errorCode) {}
};

// Success wrapper for void (no value to return)
struct Ok {
    // Empty struct for Result<void> success case
};

// Result<T> monad for error handling
template<typename T = void>
class Result {
public:
    // Construct success with value
    static Result Success(const T& value) {
        Result r;
        r.m_data = value;
        return r;
    }
    
    // Construct failure with error
    static Result Failure(const Error& err) {
        Result r;
        r.m_data = err;
        return r;
    }
    
    // Construct failure with message
    static Result Failure(const char* msg, int code = 0) {
        return Failure(Error(msg, code));
    }
    
    // Check if result is success
    bool isOk() const {
        return std::holds_alternative<T>(m_data);
    }
    
    // Check if result is error
    bool isError() const {
        return std::holds_alternative<Error>(m_data);
    }
    
    // Get value (throws if error)
    const T& unwrap() const {
        if (isError()) {
            throw std::runtime_error("Result::unwrap() called on error: " + error());
        }
        return std::get<T>(m_data);
    }
    
    // Get value or default
    T unwrapOr(const T& defaultValue) const {
        return isOk() ? std::get<T>(m_data) : defaultValue;
    }
    
    // Get error message (returns empty if success)
    std::string error() const {
        return isError() ? std::get<Error>(m_data).message : "";
    }
    
    // Get error code (returns 0 if success)
    int errorCode() const {
        return isError() ? std::get<Error>(m_data).code : 0;
    }
    
    // Conversion to bool (true if ok, false if error)
    explicit operator bool() const {
        return isOk();
    }

private:
    std::variant<T, Error> m_data;
    Result() = default;  // Private constructor, use Success/Failure
};

// Specialization for Result<void> (no value, only success/error)
template<>
class Result<void> {
public:
    // Construct success
    static Result Success() {
        Result r;
        r.m_isOk = true;
        return r;
    }
    
    // Construct failure with error
    static Result Failure(const Error& err) {
        Result r;
        r.m_isOk = false;
        r.m_error = err;
        return r;
    }
    
    // Construct failure with message
    static Result Failure(const char* msg, int code = 0) {
        return Failure(Error(msg, code));
    }
    
    // Check if result is success
    bool isOk() const {
        return m_isOk;
    }
    
    // Check if result is error
    bool isError() const {
        return !m_isOk;
    }
    
    // Get error message (returns empty if success)
    std::string error() const {
        return m_isOk ? "" : m_error.message;
    }
    
    // Get error code (returns 0 if success)
    int errorCode() const {
        return m_isOk ? 0 : m_error.code;
    }
    
    // Conversion to bool (true if ok, false if error)
    explicit operator bool() const {
        return m_isOk;
    }

private:
    bool m_isOk = false;
    Error m_error = Error("");
    Result() = default;  // Private constructor, use Success/Failure
};
