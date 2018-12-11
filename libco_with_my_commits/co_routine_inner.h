/*
* Tencent is pleased to support the open source community by making Libco available.

* Copyright (C) 2014 THL A29 Limited, a Tencent company. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License"); 
* you may not use this file except in compliance with the License. 
* You may obtain a copy of the License at
*
*	http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, 
* software distributed under the License is distributed on an "AS IS" BASIS, 
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. 
* See the License for the specific language governing permissions and 
* limitations under the License.
*/

#ifndef __CO_ROUTINE_INNER_H__

#include "co_routine.h"
#include "coctx.h"
struct stCoRoutineEnv_t;
struct stCoSpec_t
{
	void *value;
};

struct stStackMem_t
{
	stCoRoutine_t *occupy_co;
	int stack_size;
	char *stack_bp; //stack_buffer + stack_size
	char *stack_buffer;
};

struct stShareStack_t
{
	unsigned int alloc_idx;
	int stack_size;
	int count;
	stStackMem_t **stack_array;
};
//协程的所有信息
struct stCoRoutine_t
{
	stCoRoutineEnv_t *env; //全局协程管理结构，协程嵌套调用栈管理结构体
	pfn_co_routine_t pfn;  //协程主函数(typedef void *(*pfn_co_routine_t)(void *); //函数指针，包含一个void *指针)
	void *arg;			   //主函数的参数
	coctx_t ctx;		   //程上下文信息，包括寄存器、用户栈信息

	char cStart;  //主函数是否被运行过
	char cEnd;	//主函数是否运行结束过
	char cIsMain; //协程是否是主协程
	char cEnableSysHook;
	char cIsShareStack; //本结构体成员 stack_mem 用户栈是否是从 share 栈中取下来的

	void *pvEnv;

	//char sRunStack[ 1024 * 128 ];
	stStackMem_t *stack_mem; //用户栈结构体

	//save satck buffer while confilct on same stack_buffer;
	char *stack_sp; //下面三个成员都是用于对用户栈内容的备份，但是销毁 env 的时候不知道为什么没有销毁这个备份，
	// 推测之前有提前销毁备份的动作
	unsigned int save_size;
	char *save_buffer;

	stCoSpec_t aSpec[1024]; //协程私有数据
};

//1.env
void co_init_curr_thread_env();
stCoRoutineEnv_t *co_get_curr_thread_env();

//2.coroutine
void co_free(stCoRoutine_t *co);
void co_yield_env(stCoRoutineEnv_t *env);

//3.func

//-----------------------------------------------------------------------------------------------

struct stTimeout_t;
struct stTimeoutItem_t;

stTimeout_t *AllocTimeout(int iSize);
void FreeTimeout(stTimeout_t *apTimeout);
int AddTimeout(stTimeout_t *apTimeout, stTimeoutItem_t *apItem, uint64_t allNow);

struct stCoEpoll_t;
stCoEpoll_t *AllocEpoll();
void FreeEpoll(stCoEpoll_t *ctx);

stCoRoutine_t *GetCurrThreadCo();
void SetEpoll(stCoRoutineEnv_t *env, stCoEpoll_t *ev);

typedef void (*pfnCoRoutineFunc_t)();

#endif

#define __CO_ROUTINE_INNER_H__
