/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "Profiler.h"

#include "log.h"

#include <chrono>
#include <iomanip>
#include <map>
#include <set>
#include <unordered_map>

#ifdef MULTI_THREADED
#include <mutex>
#endif

// LCOV_EXCL_START

using namespace vc4c;

using Clock = std::chrono::system_clock;
using Duration = std::chrono::microseconds;

struct Entry
{
    std::string name;
    Duration duration;
    std::size_t invocations;
    std::string fileName;
    std::size_t lineNumber;

    bool operator<(const Entry& right) const
    {
        if(duration == right.duration)
            return name > right.name;
        return duration > right.duration;
    }
};

struct Counter
{
    std::string name;
    int64_t count;
    std::size_t index;
    std::size_t invocations;
    std::size_t prevCounter;
    std::string fileName;
    std::size_t lineNumber;

    bool operator<(const Counter& right) const
    {
        if(index == right.index)
            return name > right.name;
        return index < right.index;
    }
};

static std::unordered_map<std::string, Entry> times;
static std::map<std::size_t, Counter> counters;
#ifdef MULTI_THREADED
static std::mutex lockTimes;
static std::mutex lockCounters;
#endif

void profiler::endFunctionCall(ProfilingResult&& result)
{
#ifdef MULTI_THREADED
    std::lock_guard<std::mutex> guard(lockTimes);
#endif
    auto& entry = times[result.name];
    entry.name = std::move(result.name);
    entry.duration += std::chrono::duration_cast<Duration>(Clock::now() - result.startTime);
    entry.invocations += 1;
    entry.fileName = std::move(result.fileName);
    entry.lineNumber = result.lineNumber;
}

void profiler::dumpProfileResults(bool writeAsWarning)
{
    logging::logLazy(writeAsWarning ? logging::Level::WARNING : logging::Level::DEBUG, [&]() {
#ifdef MULTI_THREADED
        std::lock_guard<std::mutex> guard(lockTimes);
#endif
        std::set<Entry> entries;
        std::set<Counter> counts;
        for(auto& entry : times)
        {
            entry.second.name = entry.first;
            entries.emplace(entry.second);
        }
        for(const auto& count : counters)
        {
            counts.emplace(count.second);
        }

        auto logFunc = writeAsWarning ? logging::warn : logging::info;

        logFunc() << std::setfill(L' ') << logging::endl;
        logFunc() << "Profiling results for " << entries.size() << " functions:" << logging::endl;
        for(const Entry& entry : entries)
        {
            logFunc() << std::setw(40) << entry.name << std::setw(7)
                      << std::chrono::duration_cast<std::chrono::milliseconds>(entry.duration).count() << " ms"
                      << std::setw(12) << entry.duration.count() << " us" << std::setw(10) << entry.invocations
                      << " calls" << std::setw(12) << entry.duration.count() / entry.invocations << " us/call"
                      << std::setw(64) << entry.fileName << "#" << entry.lineNumber << logging::endl;
        }

        logFunc() << logging::endl;
        logFunc() << "Profiling results for " << counts.size() << " counters:" << logging::endl;
        for(const Counter& counter : counts)
        {
            logFunc() << std::setw(40) << counter.name << std::setw(7) << counter.count << " counts" << std::setw(5)
                      << counter.invocations << " calls" << std::setw(6) << counter.count / counter.invocations
                      << " avg./call" << std::setw(8) << (counter.prevCounter == SIZE_MAX ? "" : "diff") << std::setw(7)
                      << std::showpos
                      << (counter.prevCounter == SIZE_MAX ? 0 : counter.count - counters[counter.prevCounter].count)
                      << " (" << std::setw(5) << std::showpos
                      << (counter.prevCounter == SIZE_MAX ?
                                 0 :
                                 static_cast<int>(100 *
                                     (-1.0 +
                                         static_cast<double>(counter.count) /
                                             static_cast<double>(counters[counter.prevCounter].count))))
                      << std::noshowpos << "%)" << std::setw(64) << counter.fileName << "#" << counter.lineNumber
                      << logging::endl;
        }
    });
    times.clear();
    counters.clear();
}

void profiler::increaseCounter(const std::size_t index, std::string name, const std::size_t value, std::string file,
    const std::size_t line, const std::size_t prevIndex)
{
#ifdef MULTI_THREADED
    std::lock_guard<std::mutex> guard(lockCounters);
#endif
    auto& entry = counters[index];
    entry.index = index;
    entry.name = std::move(name);
    entry.count += value;
    entry.invocations += 1;
    entry.prevCounter = prevIndex;
    entry.fileName = std::move(file);
    entry.lineNumber = line;
}
// LCOV_EXCL_STOP
