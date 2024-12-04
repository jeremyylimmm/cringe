#pragma once

#include <stdint.h>
#include <stdio.h>

#define X(name, ...) CB_NODE_##name,
typedef enum {
  CB_NODE_UNINITIALIZED,
  #include "node_kind.def"
} cb_node_kind_t;
#undef X

typedef struct cringe_arena_t cb_arena_t;

typedef struct cb_node_t cb_node_t;
typedef struct cb_use_t cb_use_t;

typedef enum {
  CB_NODE_FLAG_NONE = 0,
  CB_NODE_FLAG_IS_LEAF = BIT(0),
  CB_NODE_FLAG_IS_PROJ = BIT(1),
  CB_NODE_FLAG_IS_CFG = BIT(2)
} cb_node_flags_t;

struct cb_node_t {
  int id;
  cb_node_flags_t flags;
  cb_node_kind_t kind;

  cb_node_t** ins;
  int num_ins;

  cb_use_t* uses;
};

struct cb_use_t {
  cb_use_t* next;
  cb_node_t* node;
  int index;
};

typedef struct {
  cb_arena_t* arena;
  int next_id;

  cb_node_t *start, *end;
} cb_func_t;

typedef struct {
  cb_node_t* start_ctrl;
  cb_node_t* start_mem;
} cb_node_start_result_t;

typedef struct {
  cb_node_t* branch_true;
  cb_node_t* branch_false;
} cb_node_branch_result_t;

typedef struct {
  int index;
  cb_node_t* node;
  cb_node_t* _parent;
} cb_ins_iterator_t;

cb_arena_t* cb_new_arena();
void cb_free_arena(cb_arena_t* arena);

cb_func_t* cb_new_func(cb_arena_t* arena);

cb_node_start_result_t cb_node_start(cb_func_t* func);
cb_node_t* cb_node_end(cb_func_t* func, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* value);

cb_node_t* cb_node_region(cb_func_t* func);
cb_node_t* cb_node_phi(cb_func_t* func);

cb_node_branch_result_t cb_node_branch(cb_func_t* func, cb_node_t* ctrl, cb_node_t* predicate);

cb_node_t* cb_node_constant(cb_func_t* func, uint64_t value);

cb_node_t* cb_node_load(cb_func_t* func, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* address);
cb_node_t* cb_node_store(cb_func_t* func, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* address, cb_node_t* value);

cb_node_t* cb_node_add (cb_func_t* func, cb_node_t* lhs, cb_node_t* rhs);
cb_node_t* cb_node_sub (cb_func_t* func, cb_node_t* lhs, cb_node_t* rhs);
cb_node_t* cb_node_mul (cb_func_t* func, cb_node_t* lhs, cb_node_t* rhs);
cb_node_t* cb_node_sdiv(cb_func_t* func, cb_node_t* lhs, cb_node_t* rhs);

void cb_set_region_ins(cb_func_t* func, cb_node_t* region, int num_ins, cb_node_t** ins);
void cb_set_phi_ins(cb_func_t* func, cb_node_t* phi, cb_node_t* region, int num_ins, cb_node_t** ins);

void cb_finalize_func(cb_func_t* func);

void cb_graphviz_func(FILE* stream, cb_func_t* func);