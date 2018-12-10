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

#include "co_routine.h"
#include "co_routine_inner.h"
#include "co_epoll.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <string>
#include <map>

#include <poll.h>
#include <sys/time.h>
#include <errno.h>

#include <assert.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <limits.h>

extern "C"
{
	extern void coctx_swap(coctx_t *, coctx_t *) asm("coctx_swap");
};
using namespace std;
stCoRoutine_t *GetCurrCo(stCoRoutineEnv_t *env);
struct stCoEpoll_t;

//全局协程管理结构，主要用于控制协程嵌套。
struct stCoRoutineEnv_t
{
	stCoRoutine_t *pCallStack[128]; //调用栈

	// 该协程内允许嵌套创建128个协程(即协程1内创建协程2, 协程2内创建协程3... 协程127内创建协程128　
	// 该结构虽然是数组, 但将其作为栈来使用, 满足后进先出的特点)

	int iCallStackSize; // 该线程内嵌套创建的协程数量, 即pCallStack数组中元素的数量

	stCoEpoll_t *pEpoll;
	// epoll抽象体，管理所有事件。该线程内的epoll实例(套接字通过该结构内的epoll句柄向内核注册事件),
	// 也用于该线程的事件循环eventloop中

	//for copy stack log lastco and nextco
	stCoRoutine_t *pending_co;
	stCoRoutine_t *occupy_co;
};
//int socket(int domain, int type, int protocol);
void co_log_err(const char *fmt, ...)
{
}

#if defined(__LIBCO_RDTSCP__)
/*主要是调用rdtscp这条汇编指令，将计数（来一个时钟脉冲+1）读出来。 
将总共的时钟脉冲数读出再除以cpu的频率（每秒时钟脉冲）就是时间*/
static unsigned long long counter(void)
{
	register uint32_t lo, hi;
	register unsigned long long o;
	__asm__ __volatile__(
		"rdtscp"
		: "=a"(lo), "=d"(hi)::"%rcx");
	o = hi;
	o <<= 32;
	return (o | lo);
}
//读取配置文件，获得cpu频率，返回配置文件mhz*1000
static unsigned long long getCpuKhz()
{
	FILE *fp = fopen("/proc/cpuinfo", "r");
	if (!fp)
		return 1;
	char buf[4096] = {0};
	fread(buf, 1, sizeof(buf), fp);
	fclose(fp);

	char *lp = strstr(buf, "cpu MHz");
	if (!lp)
		return 1;
	lp += strlen("cpu MHz");
	while (*lp == ' ' || *lp == '\t' || *lp == ':')
	{
		++lp;
	}

	double mhz = atof(lp);
	unsigned long long u = (unsigned long long)(mhz * 1000);
	return u;
}
#endif
//读取1970年1月1日到现在的时间，单位毫秒
static unsigned long long GetTickMS()
{
#if defined(__LIBCO_RDTSCP__)
	static uint32_t khz = getCpuKhz();
	return counter() / khz;
#else
	struct timeval now = {0};
	gettimeofday(&now, NULL);
	unsigned long long u = now.tv_sec;
	u *= 1000;
	u += now.tv_usec / 1000;
	return u;
#endif
}
//读取tid  获取进程 pid
static pid_t GetPid()
{
	static __thread pid_t pid = 0;
	static __thread pid_t tid = 0;
	if (!pid || !tid || pid != getpid())
	{
		pid = getpid();
#if defined(__APPLE__)
		tid = syscall(SYS_gettid);
		if (-1 == (long)tid)
		{
			tid = pid;
		}
#elif defined(__FreeBSD__)
		syscall(SYS_thr_self, &tid);
		if (tid < 0)
		{
			tid = pid;
		}
#else
		tid = syscall(__NR_gettid);
#endif
	}
	return tid;
}
/*
static pid_t GetPid()
{
	char **p = (char**)pthread_self();
	return p ? *(pid_t*)(p + 18) : getpid();
}
*/
//将一个节点从链表中删除，此节点数据结构中包含前驱、链表等必要信息
template <class T, class TLink>
void RemoveFromLink(T *ap)
{
	TLink *lst = ap->pLink;
	if (!lst)
		return;
	assert(lst->head && lst->tail);

	if (ap == lst->head)
	{
		lst->head = ap->pNext;
		if (lst->head)
		{
			lst->head->pPrev = NULL;
		}
	}
	else
	{
		if (ap->pPrev)
		{
			ap->pPrev->pNext = ap->pNext;
		}
	}

	if (ap == lst->tail)
	{
		lst->tail = ap->pPrev;
		if (lst->tail)
		{
			lst->tail->pNext = NULL;
		}
	}
	else
	{
		ap->pNext->pPrev = ap->pPrev;
	}

	ap->pPrev = ap->pNext = NULL;
	ap->pLink = NULL;
}
//push_back，要注意 pLink（原来链表）一定为NULL，不然不会做任何操作。
template <class TNode, class TLink>
void inline AddTail(TLink *apLink, TNode *ap)
{
	if (ap->pLink)
	{
		return;
	}
	if (apLink->tail)
	{
		apLink->tail->pNext = (TNode *)ap;
		ap->pNext = NULL;
		ap->pPrev = apLink->tail;
		apLink->tail = ap;
	}
	else
	{
		apLink->head = apLink->tail = ap;
		ap->pNext = ap->pPrev = NULL;
	}
	ap->pLink = apLink;
}
template <class TNode, class TLink>
void inline PopHead(TLink *apLink)
{
	if (!apLink->head)
	{
		return;
	}
	TNode *lp = apLink->head;
	if (apLink->head == apLink->tail)
	{
		apLink->head = apLink->tail = NULL;
	}
	else
	{
		apLink->head = apLink->head->pNext;
	}

	lp->pPrev = lp->pNext = NULL;
	lp->pLink = NULL;

	if (apLink->head)
	{
		apLink->head->pPrev = NULL;
	}
}
//将链表 apOther 插入到 apLink 之后
template <class TNode, class TLink>
void inline Join(TLink *apLink, TLink *apOther)
{
	//printf("apOther %p\n",apOther);
	if (!apOther->head)
	{
		return;
	}
	TNode *lp = apOther->head;
	while (lp)
	{
		lp->pLink = apLink;
		lp = lp->pNext;
	}
	lp = apOther->head;
	if (apLink->tail)
	{
		apLink->tail->pNext = (TNode *)lp;
		lp->pPrev = apLink->tail;
		apLink->tail = apOther->tail;
	}
	else
	{
		apLink->head = apOther->head;
		apLink->tail = apOther->tail;
	}

	apOther->head = apOther->tail = NULL;
}

/////////////////for copy stack //////////////////////////
//向os申请stack_size大小的共享栈并初始化，返回一个栈结构体指针(自己构造的栈数据)
stStackMem_t *co_alloc_stackmem(unsigned int stack_size)
{
	stStackMem_t *stack_mem = (stStackMem_t *)malloc(sizeof(stStackMem_t));
	stack_mem->occupy_co = NULL;
	stack_mem->stack_size = stack_size;
	stack_mem->stack_buffer = (char *)malloc(stack_size);
	stack_mem->stack_bp = stack_mem->stack_buffer + stack_size;
	return stack_mem;
};
/*向os申请一个share栈并初始化，返回相应类型的结构体指针，参数表示内部的栈队列的数量和大小．

在协程环境初始化时，要先调用(co_alloc_sharestack)
来分配共享栈的内容，其中第一个参数 count 是指分配多少个共享栈，stack_size 是指每个栈的大小,
分配出来的结构名是 stShareStack_t 。

共享栈的结构是一个数组，它里面有 count 个元素，每个元素都是一个指向一段内存的指针 stStackMem_t 

*/

stShareStack_t *co_alloc_sharestack(int count, int stack_size)
{
	stShareStack_t *share_stack = (stShareStack_t *)malloc(sizeof(stShareStack_t));
	share_stack->alloc_idx = 0;
	share_stack->stack_size = stack_size;

	// alloc stack array
	share_stack->count = count;
	/*stStackMem_t 是一个指向一段内存的指针*/
	stStackMem_t **stack_array = (stStackMem_t **)calloc(count, sizeof(stStackMem_t *));
	for (int i = 0; i < count; i++)
	{
		stack_array[i] = co_alloc_stackmem(stack_size);
	}
	share_stack->stack_array = stack_array;
	return share_stack;
};
//从一个share栈的栈队列中取下一个栈并返回指针。
static stStackMem_t *co_get_stackmem(stShareStack_t *share_stack)
{
	if (!share_stack)
	{
		return NULL;
	}
	int idx = share_stack->alloc_idx % share_stack->count;
	share_stack->alloc_idx++;

	return share_stack->stack_array[idx];
}

// ----------------------------------------------------------------------------
struct stTimeoutItemLink_t;
struct stTimeoutItem_t;
struct stCoEpoll_t
{
	int iEpollFd;
	static const int _EPOLL_SIZE = 1024 * 10;

	struct stTimeout_t *pTimeout;

	struct stTimeoutItemLink_t *pstTimeoutList;

	struct stTimeoutItemLink_t *pstActiveList;

	co_epoll_res *result;
};
typedef void (*OnPreparePfn_t)(stTimeoutItem_t *, struct epoll_event &ev, stTimeoutItemLink_t *active);
typedef void (*OnProcessPfn_t)(stTimeoutItem_t *);
struct stTimeoutItem_t
{

	enum
	{
		eMaxTimeout = 40 * 1000 //40s
	};
	stTimeoutItem_t *pPrev;
	stTimeoutItem_t *pNext;
	stTimeoutItemLink_t *pLink;

	unsigned long long ullExpireTime;

	OnPreparePfn_t pfnPrepare;
	OnProcessPfn_t pfnProcess;

	void *pArg; // routine
	bool bTimeout;
};
struct stTimeoutItemLink_t
{
	stTimeoutItem_t *head;
	stTimeoutItem_t *tail;
};
struct stTimeout_t
{
	stTimeoutItemLink_t *pItems;
	int iItemSize;

	unsigned long long ullStart;
	long long llStartIdx;
};
stTimeout_t *AllocTimeout(int iSize)
{
	stTimeout_t *lp = (stTimeout_t *)calloc(1, sizeof(stTimeout_t));

	lp->iItemSize = iSize;
	lp->pItems = (stTimeoutItemLink_t *)calloc(1, sizeof(stTimeoutItemLink_t) * lp->iItemSize);

	lp->ullStart = GetTickMS();
	lp->llStartIdx = 0;

	return lp;
}
void FreeTimeout(stTimeout_t *apTimeout)
{
	free(apTimeout->pItems);
	free(apTimeout);
}
int AddTimeout(stTimeout_t *apTimeout, stTimeoutItem_t *apItem, unsigned long long allNow)
{
	if (apTimeout->ullStart == 0)
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}
	if (allNow < apTimeout->ullStart)
	{
		co_log_err("CO_ERR: AddTimeout line %d allNow %llu apTimeout->ullStart %llu",
				   __LINE__, allNow, apTimeout->ullStart);

		return __LINE__;
	}
	if (apItem->ullExpireTime < allNow)
	{
		co_log_err("CO_ERR: AddTimeout line %d apItem->ullExpireTime %llu allNow %llu apTimeout->ullStart %llu",
				   __LINE__, apItem->ullExpireTime, allNow, apTimeout->ullStart);

		return __LINE__;
	}
	unsigned long long diff = apItem->ullExpireTime - apTimeout->ullStart;

	if (diff >= (unsigned long long)apTimeout->iItemSize)
	{
		diff = apTimeout->iItemSize - 1;
		co_log_err("CO_ERR: AddTimeout line %d diff %d",
				   __LINE__, diff);

		//return __LINE__;
	}
	AddTail(apTimeout->pItems + (apTimeout->llStartIdx + diff) % apTimeout->iItemSize, apItem);

	return 0;
}
inline void TakeAllTimeout(stTimeout_t *apTimeout, unsigned long long allNow, stTimeoutItemLink_t *apResult)
{
	if (apTimeout->ullStart == 0)
	{
		apTimeout->ullStart = allNow;
		apTimeout->llStartIdx = 0;
	}

	if (allNow < apTimeout->ullStart)
	{
		return;
	}
	int cnt = allNow - apTimeout->ullStart + 1;
	if (cnt > apTimeout->iItemSize)
	{
		cnt = apTimeout->iItemSize;
	}
	if (cnt < 0)
	{
		return;
	}
	for (int i = 0; i < cnt; i++)
	{
		int idx = (apTimeout->llStartIdx + i) % apTimeout->iItemSize;
		Join<stTimeoutItem_t, stTimeoutItemLink_t>(apResult, apTimeout->pItems + idx);
	}
	apTimeout->ullStart = allNow;
	apTimeout->llStartIdx += cnt - 1;
}
static int CoRoutineFunc(stCoRoutine_t *co, void *)
{
	if (co->pfn)
	{
		co->pfn(co->arg);
	}
	co->cEnd = 1;

	stCoRoutineEnv_t *env = co->env;

	co_yield_env(env);

	return 0;
}
//负责struct stCoRoutine_t的空间申请以及初始化。
struct stCoRoutine_t *co_create_env(stCoRoutineEnv_t *env, const stCoRoutineAttr_t *attr,
									pfn_co_routine_t pfn, void *arg)
/*
1. 全局协程管理结构
2. 内部有stShareStack_t共享栈底层类型变量
参数3和参数4是协程入口函数和参数。
*/
{

	stCoRoutineAttr_t at;
	if (attr)
	{
		memcpy(&at, attr, sizeof(at));
	}
	if (at.stack_size <= 0)
	{
		at.stack_size = 128 * 1024;
	}
	else if (at.stack_size > 1024 * 1024 * 8)
	{
		at.stack_size = 1024 * 1024 * 8;
	}

	if (at.stack_size & 0xFFF) //如果size大于等于2的12次方（1024*4），size就向上对齐
	{
		at.stack_size &= ~0xFFF;
		at.stack_size += 0x1000;
	}

	stCoRoutine_t *lp = (stCoRoutine_t *)malloc(sizeof(stCoRoutine_t));

	memset(lp, 0, (long)(sizeof(stCoRoutine_t)));

	lp->env = env; //stCoRoutineEnv_t
	lp->pfn = pfn; //入口函数
	lp->arg = arg; //入口函数参数

	stStackMem_t *stack_mem = NULL; // stStackMem_t包含栈空间指针，栈帧，栈size，使用者
	if (at.share_stack)				//若共享栈存在，则stack_mem从共享栈中取，否则直接向操作系统申请。
	{
		stack_mem = co_get_stackmem(at.share_stack);
		//co_get_stackmem() 函数是从共享栈阵列中取出下一个共享栈
		at.stack_size = at.share_stack->stack_size;
	}
	else
	{
		stack_mem = co_alloc_stackmem(at.stack_size);
	}
	lp->stack_mem = stack_mem;

	lp->ctx.ss_sp = stack_mem->stack_buffer;
	lp->ctx.ss_size = at.stack_size;

	lp->cStart = 0;
	lp->cEnd = 0;
	lp->cIsMain = 0;
	lp->cEnableSysHook = 0;
	lp->cIsShareStack = at.share_stack != NULL;

	lp->save_size = 0;
	lp->save_buffer = NULL;

	return lp;
}

int co_create(stCoRoutine_t **ppco, const stCoRoutineAttr_t *attr, pfn_co_routine_t pfn, void *arg)
{
	if (!co_get_curr_thread_env())
	{
		co_init_curr_thread_env();
	}
	stCoRoutine_t *co = co_create_env(co_get_curr_thread_env(), attr, pfn, arg);
	*ppco = co;
	return 0;
}
void co_free(stCoRoutine_t *co)
{
	if (!co->cIsShareStack)
	{
		free(co->stack_mem->stack_buffer);
		free(co->stack_mem);
	}
	free(co);
}
void co_release(stCoRoutine_t *co)
{
	co_free(co);
}

void co_swap(stCoRoutine_t *curr, stCoRoutine_t *pending_co);

void co_resume(stCoRoutine_t *co)
{
	stCoRoutineEnv_t *env = co->env;
	stCoRoutine_t *lpCurrRoutine = env->pCallStack[env->iCallStackSize - 1];
	if (!co->cStart)
	{
		coctx_make(&co->ctx, (coctx_pfn_t)CoRoutineFunc, co, 0);
		co->cStart = 1;
	}
	env->pCallStack[env->iCallStackSize++] = co;
	co_swap(lpCurrRoutine, co);
}
void co_yield_env(stCoRoutineEnv_t *env)
{

	stCoRoutine_t *last = env->pCallStack[env->iCallStackSize - 2];
	stCoRoutine_t *curr = env->pCallStack[env->iCallStackSize - 1];

	env->iCallStackSize--;

	co_swap(curr, last);
}

void co_yield_ct()
{

	co_yield_env(co_get_curr_thread_env());
}
void co_yield(stCoRoutine_t *co)
{
	co_yield_env(co->env);
}
/*
它通过计算 bp 到 sp 的距离，知道目前这个协程使用了的栈空间的大小，
然后通过malloc分配一段这么大的空间，把栈上的内容全部复制进去（aka. Copy Stack, copy-out），
栈上的内容也一样是储存在每个协程自己的结构 stCoroutine_t 上，因此每个协程依然有自己独立的栈空间。
*/
void save_stack_buffer(stCoRoutine_t *occupy_co)
{
	///copy out
	stStackMem_t *stack_mem = occupy_co->stack_mem;
	int len = stack_mem->stack_bp - occupy_co->stack_sp;

	if (occupy_co->save_buffer)
	{
		free(occupy_co->save_buffer), occupy_co->save_buffer = NULL;
	}

	occupy_co->save_buffer = (char *)malloc(len); //malloc buf;
	occupy_co->save_size = len;

	memcpy(occupy_co->save_buffer, occupy_co->stack_sp, len);
}

void co_swap(stCoRoutine_t *curr, stCoRoutine_t *pending_co)
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();

	//get curr stack sp
	char c;
	curr->stack_sp = &c;

	if (!pending_co->cIsShareStack)
	{
		env->pending_co = NULL;
		env->occupy_co = NULL;
	}
	else
	{
		env->pending_co = pending_co;
		//get last occupy co on the same stack mem
		stCoRoutine_t *occupy_co = pending_co->stack_mem->occupy_co;
		//set pending co to occupy thest stack mem;
		pending_co->stack_mem->occupy_co = pending_co;

		env->occupy_co = occupy_co;
		if (occupy_co && occupy_co != pending_co)
		{
			save_stack_buffer(occupy_co);
		}
	}

	//swap context 汇编，Swap寄存器
	coctx_swap(&(curr->ctx), &(pending_co->ctx));

	//stack buffer may be overwrite, so get again;
	stCoRoutineEnv_t *curr_env = co_get_curr_thread_env();
	stCoRoutine_t *update_occupy_co = curr_env->occupy_co;
	stCoRoutine_t *update_pending_co = curr_env->pending_co;

	if (update_occupy_co && update_pending_co && update_occupy_co != update_pending_co)
	{
		//resume stack buffer
		if (update_pending_co->save_buffer && update_pending_co->save_size > 0)
		{
			memcpy(update_pending_co->stack_sp, update_pending_co->save_buffer, update_pending_co->save_size);
		}
	}
}

//int poll(struct pollfd fds[], nfds_t nfds, int timeout);
// { fd,events,revents }
struct stPollItem_t;
struct stPoll_t : public stTimeoutItem_t
{
	struct pollfd *fds;
	nfds_t nfds; // typedef unsigned long int nfds_t;

	stPollItem_t *pPollItems;

	int iAllEventDetach;

	int iEpollFd;

	int iRaiseCnt;
};
struct stPollItem_t : public stTimeoutItem_t
{
	struct pollfd *pSelf;
	stPoll_t *pPoll;

	struct epoll_event stEvent;
};
/*
 *   EPOLLPRI 		POLLPRI    // There is urgent data to read.  
 *   EPOLLMSG 		POLLMSG
 *
 *   				POLLREMOVE
 *   				POLLRDHUP
 *   				POLLNVAL
 *
 * */
static uint32_t PollEvent2Epoll(short events)
{
	uint32_t e = 0;
	if (events & POLLIN)
		e |= EPOLLIN;
	if (events & POLLOUT)
		e |= EPOLLOUT;
	if (events & POLLHUP)
		e |= EPOLLHUP;
	if (events & POLLERR)
		e |= EPOLLERR;
	if (events & POLLRDNORM)
		e |= EPOLLRDNORM;
	if (events & POLLWRNORM)
		e |= EPOLLWRNORM;
	return e;
}
static short EpollEvent2Poll(uint32_t events)
{
	short e = 0;
	if (events & EPOLLIN)
		e |= POLLIN;
	if (events & EPOLLOUT)
		e |= POLLOUT;
	if (events & EPOLLHUP)
		e |= POLLHUP;
	if (events & EPOLLERR)
		e |= POLLERR;
	if (events & EPOLLRDNORM)
		e |= POLLRDNORM;
	if (events & EPOLLWRNORM)
		e |= POLLWRNORM;
	return e;
}

static stCoRoutineEnv_t *g_arrCoEnvPerThread[204800] = {0};
void co_init_curr_thread_env()
{
	pid_t pid = GetPid();
	g_arrCoEnvPerThread[pid] = (stCoRoutineEnv_t *)calloc(1, sizeof(stCoRoutineEnv_t));
	stCoRoutineEnv_t *env = g_arrCoEnvPerThread[pid];

	env->iCallStackSize = 0;
	struct stCoRoutine_t *self = co_create_env(env, NULL, NULL, NULL);
	self->cIsMain = 1;

	env->pending_co = NULL;
	env->occupy_co = NULL;

	coctx_init(&self->ctx);

	env->pCallStack[env->iCallStackSize++] = self;

	stCoEpoll_t *ev = AllocEpoll();
	SetEpoll(env, ev);
}
stCoRoutineEnv_t *co_get_curr_thread_env()
{
	return g_arrCoEnvPerThread[GetPid()];
}

void OnPollProcessEvent(stTimeoutItem_t *ap)
{
	stCoRoutine_t *co = (stCoRoutine_t *)ap->pArg;
	co_resume(co);
}

void OnPollPreparePfn(stTimeoutItem_t *ap, struct epoll_event &e, stTimeoutItemLink_t *active)
{
	stPollItem_t *lp = (stPollItem_t *)ap;
	lp->pSelf->revents = EpollEvent2Poll(e.events);

	stPoll_t *pPoll = lp->pPoll;
	pPoll->iRaiseCnt++;

	if (!pPoll->iAllEventDetach)
	{
		pPoll->iAllEventDetach = 1;

		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(pPoll);

		AddTail(active, pPoll);
	}
}

void co_eventloop(stCoEpoll_t *ctx, pfn_co_eventloop_t pfn, void *arg)
{
	if (!ctx->result)
	{
		ctx->result = co_epoll_res_alloc(stCoEpoll_t::_EPOLL_SIZE);
	}
	co_epoll_res *result = ctx->result;

	for (;;)
	{
		int ret = co_epoll_wait(ctx->iEpollFd, result, stCoEpoll_t::_EPOLL_SIZE, 1);

		stTimeoutItemLink_t *active = (ctx->pstActiveList);
		stTimeoutItemLink_t *timeout = (ctx->pstTimeoutList);

		memset(timeout, 0, sizeof(stTimeoutItemLink_t));

		for (int i = 0; i < ret; i++)
		{
			stTimeoutItem_t *item = (stTimeoutItem_t *)result->events[i].data.ptr;
			if (item->pfnPrepare)
			{
				item->pfnPrepare(item, result->events[i], active);
			}
			else
			{
				AddTail(active, item);
			}
		}

		unsigned long long now = GetTickMS();
		TakeAllTimeout(ctx->pTimeout, now, timeout);

		stTimeoutItem_t *lp = timeout->head;
		while (lp)
		{
			//printf("raise timeout %p\n",lp);
			lp->bTimeout = true;
			lp = lp->pNext;
		}

		Join<stTimeoutItem_t, stTimeoutItemLink_t>(active, timeout);

		lp = active->head;
		while (lp)
		{

			PopHead<stTimeoutItem_t, stTimeoutItemLink_t>(active);
			if (lp->bTimeout && now < lp->ullExpireTime)
			{
				int ret = AddTimeout(ctx->pTimeout, lp, now);
				if (!ret)
				{
					lp->bTimeout = false;
					lp = active->head;
					continue;
				}
			}
			if (lp->pfnProcess)
			{
				lp->pfnProcess(lp);
			}

			lp = active->head;
		}
		if (pfn)
		{
			if (-1 == pfn(arg))
			{
				break;
			}
		}
	}
}
//将ap中pArg保存的stCoRoutine_t*取出，赋予执行权。pArg用于保存回到co_poll_inner的协程控制字指针。
void OnCoroutineEvent(stTimeoutItem_t *ap)
{
	stCoRoutine_t *co = (stCoRoutine_t *)ap->pArg;
	co_resume(co);
}

stCoEpoll_t *AllocEpoll()
{
	stCoEpoll_t *ctx = (stCoEpoll_t *)calloc(1, sizeof(stCoEpoll_t));

	ctx->iEpollFd = co_epoll_create(stCoEpoll_t::_EPOLL_SIZE);
	ctx->pTimeout = AllocTimeout(60 * 1000);

	ctx->pstActiveList = (stTimeoutItemLink_t *)calloc(1, sizeof(stTimeoutItemLink_t));
	ctx->pstTimeoutList = (stTimeoutItemLink_t *)calloc(1, sizeof(stTimeoutItemLink_t));

	return ctx;
}

void FreeEpoll(stCoEpoll_t *ctx)
{
	if (ctx)
	{
		free(ctx->pstActiveList);
		free(ctx->pstTimeoutList);
		FreeTimeout(ctx->pTimeout);
		co_epoll_res_free(ctx->result);
	}
	free(ctx);
}

stCoRoutine_t *GetCurrCo(stCoRoutineEnv_t *env)
{
	return env->pCallStack[env->iCallStackSize - 1];
}
//获取正在执行的（当然就是调用者自己喽）协程控制块
stCoRoutine_t *GetCurrThreadCo()
{
	stCoRoutineEnv_t *env = co_get_curr_thread_env();
	if (!env)
		return 0;
	return GetCurrCo(env);
}

typedef int (*poll_pfn_t)(struct pollfd fds[], nfds_t nfds, int timeout);
int co_poll_inner(stCoEpoll_t *ctx, struct pollfd fds[], nfds_t nfds, int timeout, poll_pfn_t pollfunc)
{
	if (timeout == 0)
	{
		return pollfunc(fds, nfds, timeout);
	}
	if (timeout < 0)
	{
		timeout = INT_MAX;
	}
	int epfd = ctx->iEpollFd;
	stCoRoutine_t *self = co_self();

	//1.struct change
	stPoll_t &arg = *((stPoll_t *)malloc(sizeof(stPoll_t)));
	memset(&arg, 0, sizeof(arg));

	arg.iEpollFd = epfd;
	arg.fds = (pollfd *)calloc(nfds, sizeof(pollfd));
	arg.nfds = nfds;

	stPollItem_t arr[2];
	if (nfds < sizeof(arr) / sizeof(arr[0]) && !self->cIsShareStack)
	{
		arg.pPollItems = arr;
	}
	else
	{
		arg.pPollItems = (stPollItem_t *)malloc(nfds * sizeof(stPollItem_t));
	}
	memset(arg.pPollItems, 0, nfds * sizeof(stPollItem_t));

	arg.pfnProcess = OnPollProcessEvent;
	arg.pArg = GetCurrCo(co_get_curr_thread_env());

	//2. add epoll
	for (nfds_t i = 0; i < nfds; i++)
	{
		arg.pPollItems[i].pSelf = arg.fds + i;
		arg.pPollItems[i].pPoll = &arg;

		arg.pPollItems[i].pfnPrepare = OnPollPreparePfn;
		struct epoll_event &ev = arg.pPollItems[i].stEvent;

		if (fds[i].fd > -1)
		{
			ev.data.ptr = arg.pPollItems + i;
			ev.events = PollEvent2Epoll(fds[i].events);

			int ret = co_epoll_ctl(epfd, EPOLL_CTL_ADD, fds[i].fd, &ev);
			if (ret < 0 && errno == EPERM && nfds == 1 && pollfunc != NULL)
			{
				if (arg.pPollItems != arr)
				{
					free(arg.pPollItems);
					arg.pPollItems = NULL;
				}
				free(arg.fds);
				free(&arg);
				return pollfunc(fds, nfds, timeout);
			}
		}
		//if fail,the timeout would work
	}

	//3.add timeout

	unsigned long long now = GetTickMS();
	arg.ullExpireTime = now + timeout;
	int ret = AddTimeout(ctx->pTimeout, &arg, now);
	int iRaiseCnt = 0;
	if (ret != 0)
	{
		co_log_err("CO_ERR: AddTimeout ret %d now %lld timeout %d arg.ullExpireTime %lld",
				   ret, now, timeout, arg.ullExpireTime);
		errno = EINVAL;
		iRaiseCnt = -1;
	}
	else
	{
		co_yield_env(co_get_curr_thread_env());
		iRaiseCnt = arg.iRaiseCnt;
	}

	{
		//clear epoll status and memory
		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&arg);
		for (nfds_t i = 0; i < nfds; i++)
		{
			int fd = fds[i].fd;
			if (fd > -1)
			{
				co_epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &arg.pPollItems[i].stEvent);
			}
			fds[i].revents = arg.fds[i].revents;
		}

		if (arg.pPollItems != arr)
		{
			free(arg.pPollItems);
			arg.pPollItems = NULL;
		}

		free(arg.fds);
		free(&arg);
	}

	return iRaiseCnt;
}

int co_poll(stCoEpoll_t *ctx, struct pollfd fds[], nfds_t nfds, int timeout_ms)
{
	return co_poll_inner(ctx, fds, nfds, timeout_ms, NULL);
}

void SetEpoll(stCoRoutineEnv_t *env, stCoEpoll_t *ev)
{
	env->pEpoll = ev;
}
stCoEpoll_t *co_get_epoll_ct()
{
	if (!co_get_curr_thread_env())
	{
		co_init_curr_thread_env();
	}
	return co_get_curr_thread_env()->pEpoll;
}
struct stHookPThreadSpec_t
{
	stCoRoutine_t *co;
	void *value;

	enum
	{
		size = 1024
	};
};
void *co_getspecific(pthread_key_t key)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if (!co || co->cIsMain)
	{
		return pthread_getspecific(key);
	}
	return co->aSpec[key].value;
}
int co_setspecific(pthread_key_t key, const void *value)
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if (!co || co->cIsMain)
	{
		return pthread_setspecific(key, value);
	}
	co->aSpec[key].value = (void *)value;
	return 0;
}

void co_disable_hook_sys()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	if (co)
	{
		co->cEnableSysHook = 0;
	}
}
bool co_is_enable_sys_hook()
{
	stCoRoutine_t *co = GetCurrThreadCo();
	return (co && co->cEnableSysHook);
}

stCoRoutine_t *co_self()
{
	return GetCurrThreadCo();
}

//co cond
struct stCoCond_t;
struct stCoCondItem_t
{
	stCoCondItem_t *pPrev;
	stCoCondItem_t *pNext;
	stCoCond_t *pLink;

	stTimeoutItem_t timeout;
};
struct stCoCond_t
{
	stCoCondItem_t *head;
	stCoCondItem_t *tail;
};
static void OnSignalProcessEvent(stTimeoutItem_t *ap)
{
	stCoRoutine_t *co = (stCoRoutine_t *)ap->pArg;
	co_resume(co);
}

stCoCondItem_t *co_cond_pop(stCoCond_t *link);
int co_cond_signal(stCoCond_t *si)
{
	stCoCondItem_t *sp = co_cond_pop(si);
	if (!sp)
	{
		return 0;
	}
	RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&sp->timeout);

	AddTail(co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout);

	return 0;
}
int co_cond_broadcast(stCoCond_t *si)
{
	for (;;)
	{
		stCoCondItem_t *sp = co_cond_pop(si);
		if (!sp)
			return 0;

		RemoveFromLink<stTimeoutItem_t, stTimeoutItemLink_t>(&sp->timeout);

		AddTail(co_get_curr_thread_env()->pEpoll->pstActiveList, &sp->timeout);
	}

	return 0;
}

int co_cond_timedwait(stCoCond_t *link, int ms)
{
	stCoCondItem_t *psi = (stCoCondItem_t *)calloc(1, sizeof(stCoCondItem_t));
	psi->timeout.pArg = GetCurrThreadCo();
	psi->timeout.pfnProcess = OnSignalProcessEvent;

	if (ms > 0)
	{
		unsigned long long now = GetTickMS();
		psi->timeout.ullExpireTime = now + ms;

		int ret = AddTimeout(co_get_curr_thread_env()->pEpoll->pTimeout, &psi->timeout, now);
		if (ret != 0)
		{
			free(psi);
			return ret;
		}
	}
	AddTail(link, psi);

	co_yield_ct();

	RemoveFromLink<stCoCondItem_t, stCoCond_t>(psi);
	free(psi);

	return 0;
}
stCoCond_t *co_cond_alloc()
{
	return (stCoCond_t *)calloc(1, sizeof(stCoCond_t));
}
int co_cond_free(stCoCond_t *cc)
{
	free(cc);
	return 0;
}

stCoCondItem_t *co_cond_pop(stCoCond_t *link)
{
	stCoCondItem_t *p = link->head;
	if (p)
	{
		PopHead<stCoCondItem_t, stCoCond_t>(link);
	}
	return p;
}
