#include <stdarg.h>

#include "internal.h"

#define NULL_REG 0xffffffff

typedef struct {
  int32_t value;
} mov32_ri_data_t;

typedef struct {
  int index;
  cb_node_t* user;
  cb_node_t* root;
} root_reference_t;

typedef struct {
  cb_func_t* new_func;
  cb_node_t** map;
  uint64_t* is_root;
  vec_t(bool_node_t) stack;
  vec_t(root_reference_t) root_refs;
} sel_context_t;

typedef uint32_t reg_t;

enum {
  PR_EAX,
  PR_ECX,
  PR_EDX,
  FIRST_VR,
  NUM_PRS = FIRST_VR
};

static char* pr_names32[NUM_PRS] = {
  "eax",
  "ecx",
  "edx"
};

typedef struct {
  int id;
} alloca_t;

#define INST_MAX_READS 4
#define INST_MAX_WRITES 4

typedef struct {
  int op;

  int num_writes;
  int num_reads;

  reg_t writes[INST_MAX_WRITES];
  reg_t reads[INST_MAX_READS];

  uint64_t data;
} machine_inst_t;

typedef struct machine_block_t machine_block_t;
struct machine_block_t {
  cb_block_t* b;
  int terminator_count;

  int id;
  machine_block_t* next;
  vec_t(machine_inst_t) code;

  int successor_count;
  int predecessor_count;

  machine_block_t* successors[2];
  machine_block_t** predecessors;
};

typedef struct {
  arena_t* arena;

  machine_block_t** block_map;
  cb_gcm_result_t* gcm;

  reg_t* reg_map;
  reg_t next_reg;
  machine_block_t* mb;
  alloca_t** alloca_map;
} gen_context_t;

static void map_input(sel_context_t* s, cb_node_t* new_node, int new_index, cb_node_t* in) {
  assert(in);

  if (bitset_get(s->is_root, in->id)) {
    root_reference_t ref = {
      .index = new_index,
      .user = new_node,
      .root = in
    };

    vec_put(s->root_refs, ref);
  }
  else {
    set_input(s->new_func, new_node, s->map[in->id], new_index);
  }
}

static bool has_multiple_uses(cb_node_t* node) {
  if (!node->uses) {
    return false;
  }

  if (!node->uses->next) {
    return false;
  }

  return true;
}

static bool should_be_root(cb_node_t* node) {
  switch (node->kind) {
    case CB_NODE_CONSTANT:
      return false;

    case CB_NODE_START:
    case CB_NODE_END:
    case CB_NODE_REGION:
    case CB_NODE_PHI:
    case CB_NODE_BRANCH:
      return true;
  }

  if (node->flags & CB_NODE_FLAG_IS_CFG) {
    return true;
  }

  if (node->flags & CB_NODE_FLAG_IS_PROJ) {
    return true;
  }

  if (!has_multiple_uses(node)) {
    return false;
  }

  return true;
}

static cb_node_t* default_select(sel_context_t* s, cb_node_t* node) {
  // we gonna clone the node
  cb_node_t* clone = new_node(s->new_func, node->kind, node->num_ins, node->data_size, node->flags);
  
  for (int i = 0; i < node->num_ins; ++i) {
    cb_node_t* in = node->ins[i];

    if (!in) {
      continue;
    }

    map_input(s, clone, i, node->ins[i]);
  }

  memcpy(DATA(clone, void), DATA(node, void), node->data_size);

  return clone;
}

#define SELF_SEL(name) \
  static cb_node_t* top_down_select_##name(sel_context_t* s, cb_node_t* node) { \
    return default_select(s, node); \
  }\
  \
  static void push_leaves_##name(sel_context_t* s, cb_node_t* node) {\
    for (int i = 0; i < node->num_ins; ++i) { \
      if (node->ins[i]) { \
        vec_put(s->stack, bool_node(false, node->ins[i]));\
      } \
    } \
  }

SELF_SEL(START)
SELF_SEL(START_MEM)
SELF_SEL(START_CTRL)

SELF_SEL(REGION)
SELF_SEL(PHI)

SELF_SEL(ALLOCA)

SELF_SEL(BRANCH_TRUE)
SELF_SEL(BRANCH_FALSE)

static cb_node_t* targ_node_bin(sel_context_t* s, cb_node_kind_t kind, cb_node_t* left, cb_node_t* right) {
  cb_node_t* node = new_node(s->new_func, kind, 2, 0, CB_NODE_FLAG_NONE);
  set_input(s->new_func, node, left, 0);
  set_input(s->new_func, node, right, 1);
  return node;
}


static cb_node_t* targ_node_add32_rr(sel_context_t* s, cb_node_t* left, cb_node_t* right) {
  return targ_node_bin(s, CB_NODE_X64_ADD32_RR, left, right);
}

static cb_node_t* targ_node_sub32_rr(sel_context_t* s, cb_node_t* left, cb_node_t* right) {
  return targ_node_bin(s, CB_NODE_X64_SUB32_RR, left, right);
}

static cb_node_t* targ_node_mul32_rr(sel_context_t* s, cb_node_t* left, cb_node_t* right) {
  return targ_node_bin(s, CB_NODE_X64_MUL32_RR, left, right);
}

static cb_node_t* targ_node_idiv32_rr(sel_context_t* s, cb_node_t* left, cb_node_t* right) {
  return targ_node_bin(s, CB_NODE_X64_IDIV32_RR, left, right);
}

static cb_node_t* targ_node_kill32(sel_context_t* s) {
  return new_leaf(s->new_func, CB_NODE_X64_KILL32, 0, CB_NODE_FLAG_NONE);
}

static cb_node_t* targ_node_mov32_ri(sel_context_t* s, uint32_t value) {
  cb_node_t* node = new_leaf(s->new_func, CB_NODE_X64_MOV32_RI, sizeof(value), CB_NODE_FLAG_NONE);
  *DATA(node, uint32_t) = value;
  return node;
}

static cb_node_t* targ_node_add32_ri(sel_context_t* s, cb_node_t* left, uint32_t right) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_ADD32_RI, 1, sizeof(right), CB_NODE_FLAG_NONE);
  set_input(s->new_func, node, left, 0);
  *DATA(node, uint32_t) = right;
  return node;
}

static cb_node_t* targ_node_mov32_mr(sel_context_t* s, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* address, cb_node_t* value) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_MOV32_MR, 4, 0, CB_NODE_FLAG_IS_PINNED | CB_NODE_FLAG_PRODUCES_MEMORY);
  set_input(s->new_func, node, ctrl, 0);
  set_input(s->new_func, node, mem, 1);
  set_input(s->new_func, node, address, 2);
  set_input(s->new_func, node, value, 3);
  return node;
}

static cb_node_t* targ_node_mov32_mi(sel_context_t* s, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* address, uint32_t value) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_MOV32_MI, 3, sizeof(value), CB_NODE_FLAG_IS_PINNED | CB_NODE_FLAG_PRODUCES_MEMORY);
  set_input(s->new_func, node, ctrl, 0);
  set_input(s->new_func, node, mem, 1);
  set_input(s->new_func, node, address, 2);
  *DATA(node, uint32_t) = value;
  return node;
}

static cb_node_t* targ_node_mov32_rm(sel_context_t* s, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* address) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_MOV32_RM, 3, 0, CB_NODE_FLAG_READS_MEMORY);
  set_input(s->new_func, node, ctrl, 0);
  set_input(s->new_func, node, mem, 1);
  set_input(s->new_func, node, address, 2);
  return node;
}

static cb_node_t* targ_node_end32(sel_context_t* s, cb_node_t* ctrl, cb_node_t* mem, cb_node_t* value) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_END32, 3, 0, CB_NODE_FLAG_IS_CFG | CB_NODE_FLAG_IS_PINNED);
  set_input(s->new_func, node, ctrl, 0);
  set_input(s->new_func, node, mem, 1);
  set_input(s->new_func, node, value, 2);
  return node;
}

static uint32_t get_const_32(cb_node_t* n) {
  assert(n->kind == CB_NODE_CONSTANT);
  return (uint32_t)DATA(n, constant_data_t)->value;
}

static cb_node_t* targ_node_branch32(sel_context_t* s, cb_node_t* ctrl, cb_node_t* predicate) {
  cb_node_t* node = new_node(s->new_func, CB_NODE_X64_BRANCH32, 2, 0, CB_NODE_FLAG_IS_CFG | CB_NODE_FLAG_IS_PINNED);
  set_input(s->new_func, node, ctrl, 0);
  set_input(s->new_func, node, predicate, 1);
  return node;
}

typedef struct {
  alloca_t* loc;
  uint32_t i;
} mov32_mi_data_t;

static uint64_t make_mov32_mi_data(gen_context_t* g, alloca_t* loc, uint32_t i) {
  mov32_mi_data_t* d = arena_type(g->arena, mov32_mi_data_t);
  d->loc = loc;
  d->i = i;
  return (uint64_t)d;
}


static char* format_string(arena_t* arena, char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  int count = vsnprintf(NULL, 0, fmt, ap);
  char* buf = arena_push(arena, (count+1) * sizeof(char));
  vsnprintf(buf, (count+1) * sizeof(char), fmt, ap);

  va_end(ap);

  return buf;
}

static char* format_reg32(arena_t* arena, reg_t reg) {
  if (reg >= FIRST_VR) {
    return format_string(arena, "%%%u", reg);
  }
  else {
    return format_string(arena, "%s", pr_names32[reg]);
  }
}

static char* format_alloca(arena_t* arena, alloca_t* a) {
  return format_string(arena, "STACK%d", a->id);
}

#define R32(r) format_reg32(scratch.arena, r)
#define ALLOCA(a) format_alloca(scratch.arena, a)

#include "x64_isa.h"

#undef R32
#undef ALLOCA

cb_func_t* cb_select_x64(cb_arena_t* arena, cb_func_t* in_func) {
  scratch_t scratch = scratch_get(1, &arena);
  cb_func_t* new_func = cb_new_func(arena);

  func_walk_t walk = func_walk_unspecified_order(scratch.arena, in_func);
  
  int root_count = 0;
  cb_node_t** roots = arena_array(scratch.arena, cb_node_t*, in_func->next_id);
  uint64_t* is_root = bitset_alloc(scratch.arena, in_func->next_id);

  for (size_t i = 0; i < walk.len; ++i) {
    cb_node_t* node = walk.nodes[i];

    if (should_be_root(node)) {
      bitset_set(is_root, node->id);
      roots[root_count++] = node;
    }
  }

  sel_context_t s = {
    .map = arena_array(scratch.arena, cb_node_t*, in_func->next_id),
    .is_root = is_root,
    .new_func = new_func
  };

  for (int i = 0; i < root_count; ++i) {
    cb_node_t* root = roots[i];

    vec_clear(s.stack);
    vec_put(s.stack, bool_node(false, root));

    // do a post-order traversal
    while (vec_len(s.stack)) {
      bool_node_t item = vec_pop(s.stack);
      cb_node_t* node = item.node;

      if (!item.processed) {
        if (node != root && bitset_get(is_root,node->id)) {
          continue;
        }

        vec_put(s.stack, bool_node(true, node));

        #define X(name, ...) case CB_NODE_##name: push_leaves_##name(&s, node); break;
        switch (node->kind) {
          default:
            assert(false);
            break;
          #include "node_kind.def"
        }
        #undef X
      }
      else {
        #define X(name, ...) case CB_NODE_##name: s.map[node->id] = top_down_select_##name(&s, node); break;
        switch (node->kind) {
          default:
            assert(false);
            break;
          #include "node_kind.def"
        }
        #undef X
      }
    }
  }

  new_func->end = s.map[in_func->end->id];

  // go through all roots and patch inputs

  for (int i = 0; i < vec_len(s.root_refs); ++i) {
    root_reference_t root_ref = s.root_refs[i];
    set_input(new_func, root_ref.user, s.map[root_ref.root->id], root_ref.index);
  }

  cb_finalize_func(new_func);

  vec_free(s.root_refs);
  vec_free(s.stack);
  scratch_release(&scratch);

  return new_func;
};

#define IN(idx) g->reg_map[node->ins[idx]->id]

static reg_t new_reg(gen_context_t* g) {
  return g->next_reg++;
}

static void prepend(machine_block_t* mb, machine_inst_t inst) {
  machine_inst_t blank = {0};
  vec_put(mb->code, blank);

  for (int i = (int)vec_len(mb->code)-1; i >= 1; --i) {
    mb->code[i] = mb->code[i-1];
  }

  mb->code[0] = inst;
}

static reg_t gen_binary_rr(gen_context_t* g, cb_node_t* node, machine_inst_t(*make_inst)(gen_context_t*, reg_t, reg_t)) {
  reg_t dest = new_reg(g);

  vec_put(g->mb->code, inst_mov32_rr(g, dest, IN(BINARY_LHS)));
  vec_put(g->mb->code, make_inst(g, dest, IN(BINARY_RHS)));

  return dest;
}

static reg_t gen_X64_ADD32_RR(gen_context_t* g, cb_node_t* node) {
  return gen_binary_rr(g, node, inst_add32_rr);
}

static reg_t gen_X64_SUB32_RR(gen_context_t* g, cb_node_t* node) {
  return gen_binary_rr(g, node, inst_sub32_rr);
}

static reg_t gen_X64_MUL32_RR(gen_context_t* g, cb_node_t* node) {
  return gen_binary_rr(g, node, inst_mul32_rr);
}

static reg_t gen_X64_IDIV32_RR(gen_context_t* g, cb_node_t* node) {
  reg_t dest = new_reg(g);

  vec_put(g->mb->code, inst_mov32_rr(g, PR_EAX, IN(BINARY_LHS)));
  vec_put(g->mb->code, inst_cdq(g));
  vec_put(g->mb->code, inst_idiv_r(g, IN(BINARY_RHS)));
  vec_put(g->mb->code, inst_mov32_rr(g, dest, PR_EAX));

  return dest;
}

static reg_t gen_X64_ADD32_RI(gen_context_t* g, cb_node_t* node) {
  reg_t dest = new_reg(g);

  vec_put(g->mb->code, inst_mov32_rr(g, dest, IN(0)));
  vec_put(g->mb->code, inst_add32_ri(g, dest, *DATA(node, uint32_t)));

  return dest;
}

static reg_t gen_X64_KILL32(gen_context_t* g, cb_node_t* node) {
  (void)node;
  reg_t dest = new_reg(g);
  vec_put(g->mb->code, inst_kill32(g, dest));
  return dest;
}

static reg_t gen_X64_MOV32_RI(gen_context_t* g, cb_node_t* node) {
  reg_t dest = new_reg(g);
  vec_put(g->mb->code, inst_mov32_ri(g, dest, *DATA(node, uint32_t)));
  return dest;
}

static reg_t gen_X64_MOV32_RM(gen_context_t* g, cb_node_t* node) {
  reg_t dest = new_reg(g);
  vec_put(g->mb->code, inst_mov32_rm(g, dest, g->alloca_map[node->ins[2]->id]));
  return dest;
}

static reg_t gen_X64_MOV32_MR(gen_context_t* g, cb_node_t* node) {
  vec_put(g->mb->code, inst_mov32_mr(g, IN(3), g->alloca_map[node->ins[2]->id]));
  return NULL_REG;
}

static reg_t gen_X64_MOV32_MI(gen_context_t* g, cb_node_t* node) {
  vec_put(g->mb->code, inst_mov32_mi(g, g->alloca_map[node->ins[2]->id], *DATA(node, uint32_t)));
  return NULL_REG;
}

static reg_t gen_X64_END32(gen_context_t* g, cb_node_t* node) {
  vec_put(g->mb->code, inst_mov32_rr(g, PR_EAX, IN(2)));
  vec_put(g->mb->code, inst_ret(g));
  return NULL_REG;
}

static reg_t gen_X64_BRANCH32(gen_context_t* g, cb_node_t* node) {
  cb_node_t* proj_true = NULL;
  cb_node_t* proj_false = NULL;

  foreach_list (cb_use_t, u, node->uses) {
    switch (u->node->kind) {
      case CB_NODE_BRANCH_TRUE:
        proj_true = u->node;
        break;
      case CB_NODE_BRANCH_FALSE:
        proj_false = u->node;
        break;
    }
  }
  
  machine_block_t* block_true = g->block_map[g->gcm->map[proj_true->id]->id];
  machine_block_t* block_false = g->block_map[g->gcm->map[proj_false->id]->id];

  vec_put(g->mb->code, inst_test32(g, IN(1), IN(1)));
  vec_put(g->mb->code, inst_jz(g, block_false));
  vec_put(g->mb->code, inst_jmp(g, block_true));

  g->mb->terminator_count = 2;

  return NULL_REG;
}

static void insert_before_n(machine_block_t* mb, machine_inst_t inst, int n) {
  machine_inst_t blank = {0};
  vec_put(mb->code, blank);

  int i;
  for (i = (int)vec_len(mb->code)-1; i >= vec_len(mb->code)-n; --i) {
    mb->code[i] = mb->code[i-1];
  }

  mb->code[i] = inst;
}

void cb_generate_x64(cb_func_t* func) {
  scratch_t scratch = scratch_get(0, NULL);  

  cb_gcm_result_t gcm = cb_run_global_code_motion(scratch.arena, func);

  machine_block_t** block_map = arena_array(scratch.arena, machine_block_t*, gcm.block_count);

  machine_block_t block_head = {0};
  machine_block_t* block_tail = &block_head;

  reg_t* reg_map = arena_array(scratch.arena, reg_t, func->next_id);
  alloca_t** alloca_map = arena_array(scratch.arena, alloca_t*, func->next_id);

  foreach_list(cb_block_t, b, gcm.cfg) {
    machine_block_t* mb = block_tail = block_tail->next = arena_type(scratch.arena, machine_block_t);
    mb->b = b;
    mb->id = b->id;
    block_map[b->id] = mb;
  }

  foreach_list(cb_block_t, b, gcm.cfg) {
    machine_block_t* mb = block_map[b->id];

    mb->successor_count = b->successor_count;
    mb->predecessor_count = b->predecessor_count;

    mb->predecessors = arena_array(scratch.arena, machine_block_t*, mb->predecessor_count);

    for (int i = 0; i < mb->successor_count; ++i) {
      mb->successors[i] = block_map[b->successors[i]->id];
    }

    for (int i = 0; i < mb->predecessor_count; ++i) {
      mb->predecessors[i] = block_map[b->predecessors[i]->id];
    }
  }

  vec_t(machine_block_t*) stack = NULL;

  gen_context_t g = {
    .arena = scratch.arena,
    
    .gcm = &gcm,
    .block_map = block_map,

    .reg_map = reg_map,
    .next_reg = FIRST_VR,
    .alloca_map = alloca_map
  };

  vec_put(stack, block_head.next);

  int phi_count = 0;
  cb_node_t** phis = arena_array(scratch.arena, cb_node_t*, func->next_id);

  int next_alloca_id = 0;

  while (vec_len(stack)) { // generate the blocks in order specified by dominator tree -> defs dominate their uses except for phis
    machine_block_t* mb = vec_pop(stack);
    cb_block_t* b = mb->b;

    g.mb = mb;

    for (int i = 0; i < b->dom_children_count; ++i) {
      cb_block_t* d = b->dom_children[i];
      vec_put(stack, block_map[d->id]);
    }

    for (int i = 0; i < b->node_count; ++i) {
      cb_node_t* node = b->nodes[i];

      #define X(name, ...) case CB_NODE_##name: reg_map[node->id] = gen_##name(&g, node); break;
      switch (node->kind) {
        default:
          assert(false);
          break;

        case CB_NODE_START:
        case CB_NODE_START_CTRL:
        case CB_NODE_START_MEM:
        case CB_NODE_REGION:
          break;

        case CB_NODE_ALLOCA: {
          alloca_t* a = arena_type(scratch.arena, alloca_t);
          a->id = next_alloca_id++;
          alloca_map[node->id] = a;
        } break;

        case CB_NODE_PHI: {
          reg_map[node->id] = new_reg(&g);

          if (!(node->flags & CB_NODE_FLAG_PRODUCES_MEMORY)) {
            phis[phi_count++] = node;
          }
        } break;

        case CB_NODE_BRANCH_TRUE:
        case CB_NODE_BRANCH_FALSE:
        {
        } break;

        #include "x64_node_kind.def"
      }
      #undef X
    }

    if (mb->successor_count == 1) {
      vec_put(mb->code, inst_jmp(&g, mb->successors[0]));
      mb->terminator_count = 1;
    }
  }

  for (int i = 0; i < phi_count; ++i) {
    cb_node_t* phi = phis[i];

    cb_node_t* region = phi->ins[0];
    machine_block_t* mb = block_map[gcm.map[region->id]->id];

    reg_t temp = new_reg(&g);

    for (int j = 1; j < phi->num_ins; ++j) {
      cb_node_t* in = phi->ins[j];
      machine_block_t* pred = block_map[gcm.map[in->id]->id];

      insert_before_n(pred, inst_mov32_rr(&g, temp, reg_map[in->id]), pred->terminator_count);
    }

    prepend(mb, inst_mov32_rr(&g, reg_map[phi->id], temp));
  }

  foreach_list(machine_block_t, mb, block_head.next) {
    printf("bb_%d:\n", mb->id);
    for (int i = 0; i < vec_len(mb->code); ++i) {
      machine_inst_t* inst = mb->code + i; 

      printf("  ");
      print_inst(stdout, inst);
      printf("\n");
    }
  }

  vec_free(stack);
  scratch_release(&scratch);
}