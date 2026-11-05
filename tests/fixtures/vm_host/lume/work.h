#ifndef MOCK_WORK_H
#define MOCK_WORK_H
#define LUME_WORK_GENERAL 0u

struct lume_work
{
    void (*run)(void *);
    void *context;
    unsigned domain;
};

int lume_work_submit(struct lume_work *);
#endif
