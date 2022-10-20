#ifndef DYNAMIC_TASK_SCHEDULER_HPP_
#define DYNAMIC_TASK_SCHEDULER_HPP_

#include <BasicUsageEnvironment.hh>

class DynamicTaskScheduler : public BasicTaskScheduler
{
public:
    DynamicTaskScheduler(unsigned maxSchedulerGranularity)
        : BasicTaskScheduler(maxSchedulerGranularity) {}
    virtual ~DynamicTaskScheduler() = default;

    virtual void SingleStep(unsigned maxDelayTime);
};


#endif