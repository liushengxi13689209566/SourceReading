

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <queue>
#include <iostream>
#include "routine.h"
#include "routine.cpp"
#include <vector>
#include "Time_heap.h"
#include "Time_heap.cpp"
#include "Poller.h"
#include "Poller.cpp"
#include "Conditional_variable.cpp"
#include "Conditional_variable.h"
#include "Log.h"
#include "Log.cpp"

using namespace libfly;

void *func1(void *arg)
{
    for (int i = 0; i < 10000000000; i++)
    {
        std::cout << "coroutine 11111 " << std::endl;
    }
}
void *func2(void *arg)
{
    for (int i = 0; i < 10000000000; i++)
    {
        std::cout << "coroutine : 22222 " << std::endl;
    }
}
void *func3(void *arg)
{
    for (int i = 0; i < 10000000000; i++)
    {
        std::cout << "coroutine : 33333 " << std::endl;
    }
}
void *Func(void *arg)
{
    int *x = (int *)arg;
    std::cout << *x << std::endl;
}
int main()
{

    // std::vector<Routine *> RoutineArr;
    // int test1 = 11;
    // int test2 = 222;
    // int test3 = 3333;
    // std::cout << "I'm Main routine" << std::endl;

    // RoutineArr.push_back(new Routine(get_curr_thread_env(), NULL, func1, (void *)&test1));
    // RoutineArr.push_back(new Routine(get_curr_thread_env(), NULL, func2, (void *)&test2));
    // RoutineArr.push_back(new Routine(get_curr_thread_env(), NULL, func3, (void *)&test3));

    // RoutineArr[0]->resume();
    // RoutineArr[0]->yield();

    // RoutineArr[1]->resume();
    // RoutineArr[1]->yield();

    // RoutineArr[2]->resume();
    // RoutineArr[2]->yield();

    int x = 666;
    Routine *routine = new Routine(get_curr_thread_env(), NULL, Func, &x);
    routine->resume();
    routine->yield();

    return 0;
}
