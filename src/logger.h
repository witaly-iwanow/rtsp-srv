#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>

#define LOG Logger::instance()

// Meyers' Singleton pattern to enable simple thread-safe logging
class Logger {
public:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static Logger& instance() {
        static Logger logger;
        return logger;
    }

    // Entry that holds the lock and manages timestamp/newline
    class Entry {
    public:
        // Constructor that only writes the timestamp
        Entry(Logger& logger): lock_(logger.mutex_), stream_(logger.stream_) { stream_ << logger.make_timestamp() << " "; }

        // Constructor that writes timestamp and the first value
        template <typename T>
        Entry(Logger& logger, const T& val): Entry(logger) {
            stream_ << val;
        }

        ~Entry() { stream_ << std::endl; }

        // Stream insertion operator for chaining
        template <typename T>
        Entry& operator<<(const T& val) {
            stream_ << val;
            return *this;
        }

    private:
        std::lock_guard<std::mutex> lock_;
        std::ostream& stream_;
    };

    // Enable stream-like syntax: LOG << "message"
    template <typename T>
    Entry operator<<(const T& val) {
        return Entry(*this, val);
    }

private:
    std::string make_timestamp() const {
        using namespace std::chrono;
        const auto now = system_clock::now();
        const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
        const std::time_t tt = system_clock::to_time_t(now);

        std::tm tm{};
        localtime_r(&tt, &tm);

        std::ostringstream ts;
        ts << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M") << ':' << std::put_time(&tm, "%S") << '.' << std::setw(3) << std::setfill('0') << ms.count() << ']';
        return ts.str();
    }

    mutable std::mutex mutex_;
    std::ostream& stream_ = std::cout;
};
