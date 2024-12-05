#include <stdlib.h>

#include "internal.h"

typedef struct {
  vec_t(cb_node_t*) packed;
  vec_t(int) sparse;
} worklist_t;

typedef struct {
  bool ins_processed;
  cb_node_t* node;
} stack_item_t;

struct cb_opt_context_t {
  cb_func_t* func;
  worklist_t worklist;
  vec_t(stack_item_t) stack; // reset locally and used for recursive stuff
};

static stack_item_t stack_item(bool ins_processed, cb_node_t* node) {
  return (stack_item_t) {
    .ins_processed = ins_processed,
    .node = node
  };
}

static void worklist_add(cb_opt_context_t* opt, cb_node_t* node) {
  worklist_t* w = &opt->worklist;

  while (node->id >= vec_len(w->sparse)) {
    vec_put(w->sparse, -1);
  }

  if (w->sparse[node->id] != -1) {
    return;
  }

  int index = (int)vec_len(w->packed);
  w->sparse[node->id] = index;
  vec_put(w->packed, node);
}

static void worklist_remove(cb_opt_context_t* opt, cb_node_t* node) {
  worklist_t* w = &opt->worklist;

  if (node->id >= vec_len(w->sparse)) {
    return;
  }

  if (w->sparse[node->id] == -1) {
    return;
  }

  int index = w->sparse[node->id];
  cb_node_t* last = w->packed[index] = vec_pop(w->packed);

  w->sparse[last->id] = index;
  w->sparse[node->id] = -1;
}

static cb_node_t* worklist_pop(cb_opt_context_t* opt) {
  worklist_t* w = &opt->worklist;
  assert(vec_len(w->packed));

  cb_node_t* node = vec_pop(w->packed);
  w->sparse[node->id] = -1;

  return node;
}

static bool worklist_empty(cb_opt_context_t* opt) {
  return vec_len(opt->worklist.packed) == 0;
}

cb_opt_context_t* cb_new_opt_context() {
  cb_opt_context_t* opt = calloc(1, sizeof(cb_opt_context_t));
  return opt;
}

void cb_free_opt_context(cb_opt_context_t* opt) {
  vec_free(opt->worklist.packed);
  vec_free(opt->worklist.sparse);
  vec_free(opt->stack);

  free(opt);
}

static void reset_context(cb_opt_context_t* opt, cb_func_t* func) {
  vec_clear(opt->worklist.packed);
  vec_clear(opt->worklist.sparse);
  vec_clear(opt->stack);
  opt->func = func;
}

typedef cb_node_t*(*idealize_func_t)(cb_opt_context_t*, cb_node_t*);

static cb_node_t* try_simple_phi_elim(cb_node_t* phi) {
  cb_node_t* input = NULL;

  for (int i = 1; i < phi->num_ins; ++i) {
    if (phi->ins[i] == phi) {
      continue;
    }

    if (!input) {
      input = phi->ins[i];
    }

    if (input != phi->ins[i]) {
      return NULL;
    }
  }

  assert(input);
  return input;
}

static cb_node_t* idealize_phi(cb_opt_context_t* opt, cb_node_t* phi) {
  (void)opt;
  // if it can be determined that a phi has a single input, we can just replace the phi with that input
  cb_node_t* simple_result = try_simple_phi_elim(phi);

  if (simple_result) {
    worklist_add(opt, phi->ins[0]);
    return simple_result;
  }

  return phi;
}

static cb_node_t* idealize_region(cb_opt_context_t* opt, cb_node_t* node) {
  (void)opt;

  if (node->num_ins > 1) {
    return node;
  }

  foreach_list(cb_use_t, use, node->uses) {
    if (use->node->kind == CB_NODE_PHI) {
      return node;
    }
  }

  return node->ins[0];
}

static cb_node_t* idealize_load(cb_opt_context_t* opt, cb_node_t* load) {
  // look at the most recent memory effects this load depends on
  // if they all happen to the same address, we can eliminate the node and use the values directly

  scratch_t scratch = scratch_get(0, NULL);
  cb_node_t* result = load;

  cb_node_t* address = load->ins[LOAD_ADDR];

  vec_clear(opt->stack);
  cb_node_t* first = load->ins[LOAD_MEM];
  vec_put(opt->stack, stack_item(false, first));

  // store the value stored at each memory effect
  cb_node_t** map = arena_array(scratch.arena, cb_node_t*, opt->func->next_id);

  // used to fill info for phi filling
  cb_node_t** temp = arena_array(scratch.arena, cb_node_t*, opt->func->next_id);

  // recurse to discover all paths
  while (vec_len(opt->stack)) {
    stack_item_t item = vec_pop(opt->stack);
    cb_node_t* node = item.node;

    switch (node->kind) {
      default:
        goto end; // hit something we can't inspect the value of

      case CB_NODE_PHI: {

        if (!item.ins_processed) {
          if (map[node->id]) {
            continue;
          }

          map[node->id] = cb_node_phi(opt->func);

          vec_put(opt->stack, stack_item(true, node));

          for (int i = 1; i < node->num_ins; ++i) {
            vec_put(opt->stack, stack_item(false, node->ins[i]));
          }
        }
        else {
          for (int i = 1; i < node->num_ins; ++i) {
            cb_node_t* input = map[node->ins[i]->id];
            assert(input);
            temp[i-1] = input;
          }

          cb_set_phi_ins(opt->func, map[node->id], node->ins[0], node->num_ins-1, temp);
        }

      } break; 

      case CB_NODE_STORE: {
        if (node->ins[STORE_ADDR] != address) {
          goto end;
        }

        map[node->id] = node->ins[STORE_VALUE];
      } break;
    }
  }

  result = map[first->id];
  assert(result);

  end:
  scratch_release(&scratch);
  return result;
}

static idealize_func_t idealize_table[NUM_CB_NODE_KINDS] = {
  [CB_NODE_PHI] = idealize_phi,
  [CB_NODE_REGION] = idealize_region,
  [CB_NODE_LOAD] = idealize_load,
};

static void find_and_remove_use(cb_node_t* user, int index) {
  for (cb_use_t** pu = &user->ins[index]->uses; *pu;) {
    cb_use_t* u = *pu;

    if (u->node == user && u->index == index) {
      *pu = u->next;
      return;
    }
    else {
      pu = &u->next;
    }
  }

  assert(false);
}

static void remove_node(cb_opt_context_t* opt, cb_node_t* first) {
  vec_clear(opt->stack);
  vec_put(opt->stack, stack_item(false, first));

  while (vec_len(opt->stack)) {
    cb_node_t* node = vec_pop(opt->stack).node;
    assert(node->uses == NULL);

    worklist_remove(opt, node);

    for (int i = 0; i < node->num_ins; ++i) {
      if (!node->ins[i]) {
        continue;
      }

      find_and_remove_use(node, i);

      if (node->ins[i]->uses == NULL) {
        vec_put(opt->stack, stack_item(false, node->ins[i]));
      }
    }

    //memset(node, 0, sizeof(*node));
  }
}

static void replace_node(cb_opt_context_t* opt, cb_node_t* target, cb_node_t* source) {
  while (target->uses) {
    cb_use_t* use = target->uses;
    target->uses = use->next;

    worklist_add(opt, use->node);
    
    assert(use->node->ins[use->index] == target);
    use->node->ins[use->index] = source;

    use->next = source->uses;
    source->uses = use;
  }

  remove_node(opt, target);
}

static void peepholes(cb_opt_context_t* opt) {
  while (!worklist_empty(opt)) {
    cb_node_t* node = worklist_pop(opt);
    idealize_func_t idealize = idealize_table[node->kind];

    if (!idealize) {
      continue;
    }

    cb_node_t* ideal = idealize(opt, node);

    if (ideal == node) {
      continue;
    }

    replace_node(opt, node, ideal);
  }
}

typedef struct {
  size_t count;
  cb_node_t** nodes;
} mem_deps_t;

static mem_deps_t get_mem_deps(arena_t* arena, cb_node_t* node) {
  mem_deps_t result = {0};
  result.nodes = arena_array(arena, cb_node_t*, node->num_ins);

  static_assert(NUM_CB_NODE_KINDS == 19, "handle memory dependencies");
  switch (node->kind) {

    case CB_NODE_PHI: {
      for (int i = 1; i < node->num_ins; ++i) {
        result.nodes[result.count++] = node->ins[i];
      }
    } break;

    case CB_NODE_LOAD: {
      result.nodes[result.count++] = node->ins[LOAD_MEM];
    } break;

    case CB_NODE_STORE: {
      result.nodes[result.count++] = node->ins[STORE_MEM];
    } break;

    case CB_NODE_END: {
      result.nodes[result.count++] = node->ins[END_MEM];
    } break;
  }

  return result;
}

typedef enum {
  DSE_NO_LOADS,
  DSE_LOADS,
} dse_state_t;

static void dead_store_elim(cb_opt_context_t* opt) {
  scratch_t scratch = scratch_get(0, NULL);

  dse_state_t* states = arena_array(scratch.arena, dse_state_t, opt->func->next_id);

  int store_count = 0;
  cb_node_t** stores = arena_array(scratch.arena, cb_node_t*, opt->func->next_id);

  vec_clear(opt->stack);

  func_walk_t walk = func_walk_unspecified_order(scratch.arena, opt->func);

  // look for all nodes that read memory - add them to stack
  // look for all stores - record them so we can potentially eliminate them
  for (size_t i = 0; i < walk.len; ++i) {
    cb_node_t* node = walk.nodes[i];

    if (node->flags & CB_NODE_FLAG_READS_MEMORY) {
      vec_put(opt->stack, stack_item(false, node));
    }

    if (node->kind == CB_NODE_STORE) {
      stores[store_count++] = node;
    }
  }

  // walk up dependency chains recording whether memory effects are observed
  while (vec_len(opt->stack)) {
    cb_node_t* node = vec_pop(opt->stack).node;

    if (states[node->id] == DSE_LOADS) {
      continue;
    }

    states[node->id] = DSE_LOADS;

    mem_deps_t deps = get_mem_deps(scratch.arena, node);

    for (size_t i = 0; i < deps.count; ++i) {
      vec_put(opt->stack, stack_item(false, deps.nodes[i]));
    }
  }

  // remove any stores that don't have observable effects
  for (int i = 0; i < store_count; ++i) {
    cb_node_t* store = stores[i];

    if (states[store->id] == DSE_NO_LOADS) {
      replace_node(opt, store, store->ins[STORE_MEM]);
    }
  }

  scratch_release(&scratch);
}

void cb_opt_func(cb_opt_context_t* opt, cb_func_t* func) {
  scratch_t scratch = scratch_get(0, NULL);
  reset_context(opt, func);

  func_walk_t walk = func_walk_unspecified_order(scratch.arena, func);

  for (size_t i = 0; i < walk.len; ++i) {
    worklist_add(opt, walk.nodes[i]);
  }

  do {
    peepholes(opt);
    dead_store_elim(opt);
  } while (!worklist_empty(opt));

  scratch_release(&scratch);
}