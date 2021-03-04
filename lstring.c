/*
** $Id: lstring.c,v 2.56.1.1 2017/04/19 17:20:42 roberto Exp $
** String table (keeps all strings handled by Lua)
** See Copyright Notice in lua.h
*/

#define lstring_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"


#define MEMERRMSG       "not enough memory"


/*
** Lua will use at most ~(2^LUAI_HASHLIMIT) bytes from a string to
** compute its hash
*/
#if !defined(LUAI_HASHLIMIT)
#define LUAI_HASHLIMIT		5
#endif


/*
** equality for long strings
比较长字符串, assert同类型, 
先看是否指向同一对象, 
再比较长度是否相等, 
最后利用字符串长度, 用memcmp比较内存内容.

*/
int luaS_eqlngstr (TString *a, TString *b) {
  size_t len = a->u.lnglen;
  lua_assert(a->tt == LUA_TLNGSTR && b->tt == LUA_TLNGSTR);
  return (a == b) ||  /* same instance or... */
    ((len == b->u.lnglen) &&  /* equal length and ... */
     (memcmp(getstr(a), getstr(b), len) == 0));  /* equal contents */
}

/*
计算字符串的 hash 值. 对于长度小于2^LUAI_HASHLIMIT的字符串, 
每字节都参加计算hash(LUAI_HASHLIMIT默认为5). 
长度大于32的字符串, 从末尾开始, 每(l >> LUAI_HASHLIMIT) + 1参加计算hash值.
*/
unsigned int luaS_hash (const char *str, size_t l, unsigned int seed) {
  unsigned int h = seed ^ cast(unsigned int, l);
  size_t step = (l >> LUAI_HASHLIMIT) + 1;
  for (; l >= step; l -= step)
    h ^= ((h<<5) + (h>>2) + cast_byte(str[l - 1]));
  return h;
}

/* 计算长字符串的 hash 值 */
unsigned int luaS_hashlongstr (TString *ts) {
  lua_assert(ts->tt == LUA_TLNGSTR);
  if (ts->extra == 0) {  /* 判断是否已经计算过哈希值 */
    ts->hash = luaS_hash(getstr(ts), ts->u.lnglen, ts->hash);
    ts->extra = 1;  /* now it has its hash */
  }
  return ts->hash;
}


/*
** resizes the string table 

重新调整哈希桶

在lua源码中触发luaS_resize的地方有三个 ：

1.lstring.c 中的 luaS_init： 初始化哈希桶 ，大小是宏定义 MINSTRTABSIZE ，
在5.3中等于128，用于存储lua的保留字以及其他库文件使用到的保留字

2. lstring.c中的 internshrstr：如果哈希桶中的字符串数量nuse 超过当前容量size， 
并且 当前size 小于或等于 MAX_INT/2 则将stringtable扩容到原本的2倍

3.lgc.c 中的checksize ：进行检查，如果当前哈希桶实际存储字符串的数量nuse小于
容量size的四分之一，则将哈希桶的容量缩减为原来的二分之一

*/
void luaS_resize (lua_State *L, int newsize) {
  int i;
  stringtable *tb = &G(L)->strt;  	// 取得存储全局字符串的结构体
  if (newsize > tb->size) {  		/* grow table if needed */
  	 // 重新分配 newSize 个 TString *类型大小的数组，使用 hash 指向内存的起始处
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
	
    for (i = tb->size; i < newsize; i++)	// 将新分配的内存清空
      tb->hash[i] = NULL;
  }
  // 由于字符串所在位置是根据表长来计算的，但表长变成        newSize 时，需要将整个哈希桶的源字符串重新排列，计算位置。
  for (i = 0; i < tb->size; i++) {  /* rehash */ 
    TString *p = tb->hash[i];
    tb->hash[i] = NULL;
    while (p) {  /* for each node in the list */
      TString *hnext = p->u.hnext;  /* save next 暂存 next 节点*/
      unsigned int h = lmod(p->hash, newsize);  /* new position 重新计算应该放到哪个哈希桶中 */
      p->u.hnext = tb->hash[h];  	/* chain it. 节点 p 的 next 指向另一个桶的第一个节点 */
      tb->hash[h] = p;   			// 然后把 p节点设置成另外一个桶的开始位置
      p = hnext;
    }
  }
  if (newsize < tb->size) {  /* shrink table if needed. 缩减哈希桶 */
    /* vanishing slice should be empty */
    lua_assert(tb->hash[newsize] == NULL && tb->hash[tb->size - 1] == NULL);
    luaM_reallocvector(L, tb->hash, tb->size, newsize, TString *);
  }
  tb->size = newsize;
}


/*
** Clear API string cache. (Entries cannot be empty, so fill them with
** a non-collectable string.)
*/
void luaS_clearcache (global_State *g) {
  int i, j;
  for (i = 0; i < STRCACHE_N; i++)
    for (j = 0; j < STRCACHE_M; j++) {
    if (iswhite(g->strcache[i][j]))  /* will entry be collected? */
      g->strcache[i][j] = g->memerrmsg;  /* replace it with something fixed */
    }
}


/*
** Initialize the string table and the string cache
初始化 global_state 中的 memerrmsg 和 strcache
*/
void luaS_init (lua_State *L) {
  global_State *g = G(L);
  int i, j;
  luaS_resize(L, MINSTRTABSIZE);  /* initial size of string table */
  /* pre-create memory-error message */
  g->memerrmsg = luaS_newliteral(L, MEMERRMSG);
  luaC_fix(L, obj2gco(g->memerrmsg));  /* it should never be collected */
  for (i = 0; i < STRCACHE_N; i++)  	/* fill cache with valid strings */
    for (j = 0; j < STRCACHE_M; j++)
      g->strcache[i][j] = g->memerrmsg;
}


/*
** creates a new string object
创建一个 TString 对象。
*/
static TString *createstrobj (lua_State *L, size_t l, int tag, unsigned int h) {
  TString *ts;
  GCObject *o;
  size_t totalsize;  	/* total size of TString object */
  totalsize = sizelstring(l);   // 对于一个 TString，要分配的内存大小
  o = luaC_newobj(L, tag, totalsize); // 分配内存，使用 GCObject *o 指向该节点，为GCObject结构体里的 marked, tt, next 赋值
  ts = gco2ts(o); 		// 把 GCObject *o 转成 TString *
  ts->hash = h;
  ts->extra = 0;
  getstr(ts)[l] = '\0';  /* ending 0 UTString 结构体后面的一个字节设置为 \0*/
  return ts;
}

// 创建长字符串
TString *luaS_createlngstrobj (lua_State *L, size_t l) {
  // 将全局随机种子seed存储在hash成员变量中
  TString *ts = createstrobj(L, l, LUA_TLNGSTR, G(L)->seed);
  ts->u.lnglen = l;
  return ts;
}


void luaS_remove (lua_State *L, TString *ts) {
  stringtable *tb = &G(L)->strt;
  TString **p = &tb->hash[lmod(ts->hash, tb->size)];
  while (*p != ts)  /* find previous element */
    p = &(*p)->u.hnext;
  *p = (*p)->u.hnext;  /* remove element from its list */
  tb->nuse--;
}


/*
** checks whether short string exists and reuses it or creates a new one
*/
static TString *internshrstr (lua_State *L, const char *str, size_t l) {
  TString *ts;
  global_State *g = G(L);
  unsigned int h = luaS_hash(str, l, g->seed); 				// 生成哈希值，也就是散列函数的 key 值
  TString **list = &g->strt.hash[lmod(h, g->strt.size)]; 	// 获取到对应的哈希桶开始位置
  lua_assert(str != NULL); 		 /* otherwise 'memcmp'/'memcpy' are undefined */
  for (ts = *list; ts != NULL; ts = ts->u.hnext) {
	// 查找是否已经存在了相同的字符串，如果找到，直接返回引用
    if (l == ts->shrlen &&
        (memcmp(str, getstr(ts), l * sizeof(char)) == 0)) {
      /* found! */
      if (isdead(g, ts))  /* dead (but not collected yet)? 如果字符串已经死掉，但是还没有被回收，立刻复活*/
        changewhite(ts);  /* resurrect it */
      return ts;
    }
  }

  // 如果哈希桶的字符串数量 nuse 超过当前容量 size, 并且还没达到 MAX_INT/2, 就扩容到2倍。
  if (g->strt.nuse >= g->strt.size && g->strt.size <= MAX_INT/2) {
    luaS_resize(L, g->strt.size * 2);
    list = &g->strt.hash[lmod(h, g->strt.size)];  /* recompute with new size */
  }

  //创建TString结构体 ，将字符串复制到该结构体后面 ，将字符串的长度赋值给成员shrlen。
  ts = createstrobj(L, l, LUA_TSHRSTR, h);
  memcpy(getstr(ts), str, l * sizeof(char));
  ts->shrlen = cast_byte(l);
  ts->u.hnext = *list;   // 该 TString 节点 ts 的 next 指向当前h哈希桶中的首个节点
  *list = ts;            // 把 h哈希桶的开始节点设置为 ts，即：插入链表表头
  g->strt.nuse++;
  return ts;
}


/*
** new string (with explicit length)
*/
TString *luaS_newlstr (lua_State *L, const char *str, size_t l) {
  if (l <= LUAI_MAXSHORTLEN)  /* short string? */
    return internshrstr(L, str, l); // 短字符串
  else {   // 长字符串
    TString *ts;
    if (l >= (MAX_SIZE - sizeof(TString))/sizeof(char))
      luaM_toobig(L);
    ts = luaS_createlngstrobj(L, l);
    memcpy(getstr(ts), str, l * sizeof(char));
    return ts;
  }
}


/*
** Create or reuse a zero-terminated string, first checking in the
** cache (using the string address as a key). The cache can contain
** only zero-terminated strings, so it is safe to use 'strcmp' to
** check hits.
创建字符串：如果在 global_state 的 strcache 中已经存在，就复用；否则就新建。
*/
TString *luaS_new (lua_State *L, const char *str) {
  unsigned int i = point2uint(str) % STRCACHE_N;  /* hash */
  int j;
  TString **p = G(L)->strcache[i];
  for (j = 0; j < STRCACHE_M; j++) {
    if (strcmp(str, getstr(p[j])) == 0)  /* hit? */
      return p[j];  	/* 在 cache 中找到相同的字符串。that is it */
  }
  /* normal route */
  for (j = STRCACHE_M - 1; j > 0; j--)
    p[j] = p[j - 1];  	/* 移动元素。move out last element */

  /* 新元素插入到最前端。 new element is first in the list */
  p[0] = luaS_newlstr(L, str, strlen(str));
  return p[0];
}


Udata *luaS_newudata (lua_State *L, size_t s) {
  Udata *u;
  GCObject *o;
  if (s > MAX_SIZE - sizeof(Udata))
    luaM_toobig(L);
  o = luaC_newobj(L, LUA_TUSERDATA, sizeludata(s));
  u = gco2u(o);
  u->len = s;
  u->metatable = NULL;
  setuservalue(L, u, luaO_nilobject);
  return u;
}

