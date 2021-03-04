/*
** $Id: lfunc.c,v 2.45.1.1 2017/04/19 17:39:34 roberto Exp $
** Auxiliary functions to manipulate prototypes and closures
** See Copyright Notice in lua.h
*/

#define lfunc_c
#define LUA_CORE

#include "lprefix.h"


#include <stddef.h>

#include "lua.h"

#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"



/*
创建一个需要执行的 C 闭包
*/
CClosure *luaF_newCclosure (lua_State *L, int n) {
  GCObject *o = luaC_newobj(L, LUA_TCCL, sizeCclosure(n)); // 分配内存
  CClosure *c = gco2ccl(o);			// 转成C闭包类型
  c->nupvalues = cast_byte(n);		// 使用了几个上值
  return c;
}

/*
创建一个需要执行的 Lua 闭包

n: upvalue 大小
*/
LClosure *luaF_newLclosure (lua_State *L, int n) {
  GCObject *o = luaC_newobj(L, LUA_TLCL, sizeLclosure(n));
  LClosure *c = gco2lcl(o);
  c->p = NULL;
  c->nupvalues = cast_byte(n);
  while (n--) c->upvals[n] = NULL;
  return c;
}

/*
** fill a closure with new closed upvalues
为 Lua 闭包分配上值，个数是：cl->nupvalues
*/
void luaF_initupvals (lua_State *L, LClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++) {
    UpVal *uv = luaM_new(L, UpVal);   // 创建 UpVal 变量
    uv->refcount = 1;
    uv->v = &uv->u.value;  			  /* 指向自己内部的值。 make it closed */
    setnilvalue(uv->v);
    cl->upvals[i] = uv;
  }
}

/*
把数据栈上的值转换成 Upvalue

1.首先遍历 lua_state 的 openupval，也就是当前栈的 upval。如果能找到对应值，就直接返回这个upval。
2.否则就新建一个 upval（这里注意 new 的是 open 的），然后链接到 openupval 中。
*/
UpVal *luaF_findupval (lua_State *L, StkId level) {
  UpVal **pp = &L->openupval;
  UpVal *p;
  UpVal *uv;
  lua_assert(isintwups(L) || L->openupval == NULL);
  while (*pp != NULL && (p = *pp)->v >= level) { // 先在当前的 openupval 链表中寻找是否已经转化过。
    lua_assert(upisopen(p));					 // 如果有就复用，否则就构建一个新的 upvalue 对象，并串到 openupval 链表中。
    if (p->v == level)  /* found a corresponding upvalue? */
      return p;  /* return it */
    pp = &p->u.open.next;
  }
  /* not found: create a new upvalue */
  uv = luaM_new(L, UpVal);	/* 新建一个 upval */
  uv->refcount = 0;
  uv->u.open.next = *pp;  	/* 链接到 lua_state 的 openupval 的头部。link it to list of open upvalues */
  uv->u.open.touched = 1;
  *pp = uv;					/* 新建的 upval 成为表头 */
  uv->v = level;  			/* 把新建 upval 的 v 设置为堆栈 level。current value lives in the stack */
  if (!isintwups(L)) {  	/* thread not in list of threads with upvalues? */
    L->twups = G(L)->twups; /* link it to the list */
    G(L)->twups = L;
  }
  return uv;
}

/*
当离开一个代码块后，这个代码块中定义的局部变量就变为不可见的。
Lua 会调整数据栈指针，销毁掉这些变量。若这些栈值还被某些闭包 
以 open 状态 的 upvalue 的形式引用，就需要把它们关闭。

luaF_close 函数逻辑：先将当前 UpVal 从 L->openipval 链表中剔除掉，
然后判断当前UpVal ->refcount 查看是否还有被其他闭包引用, 
如果refcount == 0 则释放 UpVal 结构；
如果还有引用则需要 把数据（uv->v 这时候在数据栈上）从数据栈上 copy 到 UpVal 结构中的 （uv->u.value）中，
最后修正 UpVal 中的指针 v（uv->v 现在指向UpVal 结构中  uv->u.value  所在地址）。
*/
void luaF_close (lua_State *L, StkId level) {
  UpVal *uv;
  while (L->openupval != NULL && (uv = L->openupval)->v >= level) {
    lua_assert(upisopen(uv));
    L->openupval = uv->u.open.next; /* remove from 'open' list */
    if (uv->refcount == 0)  		/* 没有被使用。 no references? */
      luaM_free(L, uv);  			/* 释放。free upvalue */
    else {
      setobj(L, &uv->u.value, uv->v);	/* 把栈上的value 复制到 upval 自己内部。 move value to upvalue slot */
      uv->v = &uv->u.value;  			/* 指向自己。now current value lives here */
      luaC_upvalbarrier(L, uv);
    }
  }
}


Proto *luaF_newproto (lua_State *L) {
  GCObject *o = luaC_newobj(L, LUA_TPROTO, sizeof(Proto));
  Proto *f = gco2p(o);
  f->k = NULL;
  f->sizek = 0;
  f->p = NULL;
  f->sizep = 0;
  f->code = NULL;
  f->cache = NULL;
  f->sizecode = 0;
  f->lineinfo = NULL;
  f->sizelineinfo = 0;
  f->upvalues = NULL;
  f->sizeupvalues = 0;
  f->numparams = 0;
  f->is_vararg = 0;
  f->maxstacksize = 0;
  f->locvars = NULL;
  f->sizelocvars = 0;
  f->linedefined = 0;
  f->lastlinedefined = 0;
  f->source = NULL;
  return f;
}


void luaF_freeproto (lua_State *L, Proto *f) {
  luaM_freearray(L, f->code, f->sizecode);
  luaM_freearray(L, f->p, f->sizep);
  luaM_freearray(L, f->k, f->sizek);
  luaM_freearray(L, f->lineinfo, f->sizelineinfo);
  luaM_freearray(L, f->locvars, f->sizelocvars);
  luaM_freearray(L, f->upvalues, f->sizeupvalues);
  luaM_free(L, f);
}


/*
** Look for n-th local variable at line 'line' in function 'func'.
** Returns NULL if not found.
*/
const char *luaF_getlocalname (const Proto *f, int local_number, int pc) {
  int i;
  for (i = 0; i<f->sizelocvars && f->locvars[i].startpc <= pc; i++) {
    if (pc < f->locvars[i].endpc) {  /* is variable active? */
      local_number--;
      if (local_number == 0)
        return getstr(f->locvars[i].varname);
    }
  }
  return NULL;  /* not found */
}

