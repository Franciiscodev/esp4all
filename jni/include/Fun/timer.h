#ifndef TIMER_H
#define TIMER_H

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>

struct timer {
    bool fpsio = false;
    int events = 0;
    void* data = nullptr;
    struct timespec str{}, end{}, old{}, now{}, sleep{}, vsync{}, test{}, strloop{}, endloop{};
    long long lodtime = 0;
    long long looptime = 0;
    long long runtime = 0;
    std::string name;
    long long Fps = 16666666LL; // Default 60 FPS in ns
    long long SleepTime = 0;
    long long vsyncSleepTime = 0;
    long long old_time = 0;
    long long now_time = 0;
    long vsynctiemr[2] = {0, 0};
    long long oldtimer = 0;
    bool islooptimestart = false;

    void setname(const char* name_) {
        name = name_ ? name_ : "";
    }

    explicit timer(const char* in_name) : name(in_name ? in_name : "") {}

    explicit timer(unsigned int fps) {
        SetFps(fps);
    }

    timer() = default;

    inline void start() {
        clock_gettime(CLOCK_MONOTONIC, &str);
    }

    inline float stop() {
        clock_gettime(CLOCK_MONOTONIC, &end);
        runtime = ((1000000000LL * end.tv_sec) + end.tv_nsec) -
                  ((1000000000LL * str.tv_sec) + str.tv_nsec);
        return static_cast<float>(runtime) / 1000000.0f;
    }

    inline void SetFps(unsigned int fps) {
        if (fps == 0) fps = 60;
        Fps = 1000000000LL / static_cast<long long>(fps);
    }

    inline void FpsEnd() {
        fpsio = false;
    }

    inline void AotuFPS_init() {
        clock_gettime(CLOCK_MONOTONIC, &old);
        SleepTime = Fps;
        start();
    }

    inline float AotuFPS() {
        clock_gettime(CLOCK_MONOTONIC, &now);
        old_time = ((1000000000LL * now.tv_sec) + now.tv_nsec) -
                   ((1000000000LL * old.tv_sec) + old.tv_nsec);
        SleepTime = Fps - old_time;
        if (SleepTime < 0) {
            SleepTime = 0;
            clock_gettime(CLOCK_MONOTONIC, &old);
            return static_cast<float>(old_time) / 1000000.0f;
        }
        nsleep(SleepTime);
        clock_gettime(CLOCK_MONOTONIC, &old);
        return static_cast<float>(old_time + SleepTime) / 1000000.0f;
    }

    inline bool cktime(unsigned int ms) {
        if (!lodtime) {
            start();
        }
        clock_gettime(CLOCK_MONOTONIC, &end);
        lodtime = (((1000000000LL * end.tv_sec) + end.tv_nsec) -
                   ((1000000000LL * str.tv_sec) + str.tv_nsec)) / 1000000LL;
        if (lodtime >= static_cast<long long>(ms)) {
            lodtime = 0;
            return true;
        }
        return false;
    }

    inline void looptimestart() {
        islooptimestart = true;
        clock_gettime(CLOCK_MONOTONIC, &strloop);
    }

    inline void looptimeend() {
        islooptimestart = false;
    }

    inline long long getlooptime() {
        clock_gettime(CLOCK_MONOTONIC, &endloop);
        looptime = ((1000000000LL * endloop.tv_sec) + endloop.tv_nsec) -
                   ((1000000000LL * strloop.tv_sec) + strloop.tv_nsec);
        return looptime;
    }

    int32_t getNumCpus() {
        static int32_t sNumCpus = []() {
            pid_t pid = gettid();
            cpu_set_t cpuSet;
            CPU_ZERO(&cpuSet);
            sched_getaffinity(pid, sizeof(cpuSet), &cpuSet);
            int32_t numCpus = 0;
            while (numCpus < CPU_SETSIZE && CPU_ISSET(numCpus, &cpuSet)) {
                ++numCpus;
            }
            return numCpus;
        }();
        return sNumCpus;
    }

    void setAffinity() {
        const int32_t numCpus = getNumCpus();
        if (numCpus <= 0) return;
        cpu_set_t cpuSet;
        CPU_ZERO(&cpuSet);
        for (int32_t cpu = 0; cpu < numCpus; ++cpu) {
            CPU_SET(cpu, &cpuSet);
        }
        sched_setaffinity(gettid(), sizeof(cpuSet), &cpuSet);
    }

    inline void nsleep(long long delayNs) {
        if (delayNs <= 0) return;
        struct timespec req{};
        struct timespec rem{};
        req.tv_sec = static_cast<time_t>(delayNs / 1000000000LL);
        req.tv_nsec = static_cast<long>(delayNs % 1000000000LL);
        while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
            req = rem;
        }
    }
};

#endif
