#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base.h"

#define TABLE_CAPACITY 1024
#define MAX_ARITY 16

typedef enum {
  NODE_SUBTREE,
  NODE_LEAF,
  NODE_CODE_LITERAL,
} node_kind_t;

typedef struct node_t node_t;
struct node_t {
  node_kind_t kind;
  int subtree_count;
  const char* name;
  int arity;
  node_t** children;
  char* binding;
};

typedef struct rule_t rule_t;
struct rule_t {
  rule_t* next;
  int id;
  node_t* in;
  node_t* out;
};

enum {
  TOK_EOF = 0,
  TOK_IDENT = 256,
  TOK_ARROW,
  TOK_STRING
};

typedef struct {
  int kind;
  const char* start;
  int length;
  int line;
} token_t;

typedef struct {
  const char* p;
  int line;
  token_t cache;
} lexer_t;

typedef struct {
  const char* name;
  rule_t* rules_head;
  int rule_count;
} op_entry_t;

static op_entry_t table[TABLE_CAPACITY];

static char* tok_to_string(token_t token) {
  char* buf = malloc((token.length + 1) * sizeof(char));
  memcpy(buf, token.start, token.length * sizeof(char));
  buf[token.length] = '\0';
  return buf;
}

static op_entry_t* get_op_entry(token_t token) {
  size_t i = fnv1a(token.start, token.length * sizeof(char)) % TABLE_CAPACITY;

  for (size_t j = 0; j < TABLE_CAPACITY; ++j) {
    if (!table[i].name) {
      table[i].name = tok_to_string(token);
      return table + i;
    }

    if (strlen(table[i].name) == token.length && memcmp(token.start, table[i].name, token.length * sizeof(char)) == 0) {
      return table + i;
    }

    i = (i+1) % TABLE_CAPACITY;
  }

  printf("string table capacity reached\n");
  exit(1);
}

static char* load_pats(const char* path) {
  FILE* file;
  if (fopen_s(&file, path, "r")) {
    printf("Failed to read '%s'\n", path);
    exit(1);
  }

  fseek(file, 0, SEEK_END);
  size_t len = ftell(file);
  rewind(file);

  char* buf = malloc((len + 1) * sizeof(char));
  len = fread(buf, 1, len, file);
  buf[len] = '\0';

  return buf;
}

static bool is_ident(int c) {
  return isalnum(c) || c == '_';
}

static token_t lex(lexer_t* l) {
  if (l->cache.start) {
    token_t result = l->cache;
    l->cache.start = NULL;
    return result;
  }

  for (;;) {
    while (isspace(*l->p)) {
      if (*(l->p++) == '\n') {
        l->line++;
      }
    }

    if (l->p[0] == '/' && l->p[1] == '/') {
      while (*l->p != '\n' && *l->p != '\0') {
        l->p++;
      }
    }
    else {
      break;
    }
  }

  const char* start = l->p++;
  int line = l->line;
  int kind = *start;

  switch (*start) {
    default:
      if (is_ident(*start)) {
        while (is_ident(*l->p)) {
          l->p++;
        }
        kind = TOK_IDENT;
      }
      break;
    case '\0':
      --l->p;
      kind = TOK_EOF;
      break;

    case '-':
      if (*l->p == '>') {
        l->p++;
        kind = TOK_ARROW;
      }
      break;

    case '"': {
      while (*l->p != '\0' && *l->p != '\n' && *l->p != '"') {
        l->p++;
      }
      if (*l->p != '"') {
        printf("unterminated string on line %d", line);
        exit(1);
      }
      l->p++;
      kind = TOK_STRING;
    } break;
  }

  return (token_t) {
    .kind = kind,
    .start = start,
    .length = (int)(l->p-start),
    .line = line
  };
}

static token_t peek(lexer_t* l) {
  if (!l->cache.start) {
    l->cache = lex(l);
  }

  return l->cache;
}

static token_t expect(lexer_t* l, int kind, const char* message) {
  token_t tok = lex(l);

  if (tok.kind != kind) {
    printf("unexpected token '%.*s' on line %d: %s\n", tok.length, tok.start, tok.line, message);
    exit(1);
  }

  return tok;
}

static node_t* parse_node(lexer_t* l, op_entry_t** out_entry, bool is_in) {
  if (peek(l).kind == TOK_STRING && !is_in) {
    token_t str = lex(l);

    char* buf = malloc(str.length-2+1);
    memcpy(buf, str.start+1, (str.length-2) * sizeof(char));
    buf[str.length-2] = '\0';
    
    node_t* node = calloc(1, sizeof(node));
    node->kind = NODE_CODE_LITERAL;
    node->name = buf;

    return node;
  }

  token_t op = expect(l, TOK_IDENT, "expected an operator name");

  char* binding = NULL;
  if (peek(l).kind == ':') {
    lex(l);
    token_t binding_tok = expect(l, TOK_IDENT, "expected an identifier for a binding");
    binding = tok_to_string(binding_tok);
  }

  int arity = 0;
  node_t* children[MAX_ARITY];

  node_kind_t kind = NODE_LEAF;

  if (peek(l).kind == '(') {
    expect(l, '(', "expected '('");
    kind = NODE_SUBTREE;

    while (peek(l).kind != ')' && peek(l).kind != TOK_EOF) {
      if (arity > 0) {
        expect(l, ',', "expected ','");
      }

      if (arity >= MAX_ARITY) {
        printf("pattern on line %d has a node whose arity is over the maximum\n", op.line);
        exit(1);
      }

      assert(arity < MAX_ARITY);
      children[arity++] = parse_node(l, NULL, is_in);
    }

    expect(l, ')', "expected ')'");
  }

  op_entry_t* entry = get_op_entry(op);

  node_t* result = calloc(1, sizeof(node_t));
  result->kind = kind;
  result->name = entry->name;
  result->arity = arity;
  result->subtree_count = result->kind == NODE_SUBTREE ? 1 : 0;
  result->binding = binding;

  result->children = calloc(1, arity * sizeof(node_t*));

  for (int i = 0; i < arity; ++i) {
    result->children[i] = children[i];
    result->subtree_count += children[i]->subtree_count;
  }

  if (out_entry) {
    *out_entry = entry;
  }

  return result;
}

static void parse_rule(lexer_t* l) {
  op_entry_t* in_entry = NULL;
  node_t* in = parse_node(l, &in_entry, true);

  expect(l, TOK_ARROW, "expected '->' between input and output pattern");
  node_t* out = parse_node(l, NULL, false);

  rule_t* rule = calloc(1, sizeof(rule_t));
  rule->id = in_entry->rule_count++;
  rule->in = in;
  rule->out = out;

  rule->next = in_entry->rules_head;
  in_entry->rules_head = rule;
}

static void parse_system(const char* pats_path) {
  char* pats = load_pats(pats_path);

  lexer_t l = {
    .p = pats,
    .line = 1
  };

  while (peek(&l).kind != TOK_EOF) {
    parse_rule(&l);
  }

  free(pats);
}

static void write_node_match(FILE* file, const char* c_value, node_t* node, bool is_root) {
  switch (node->kind) {
    case NODE_CODE_LITERAL:
      assert(false);
      break;
    case NODE_LEAF:
      return;
  }

  char temp[512];
  char* tail = temp;

  for (const char* c = node->name; *c; ++c) {
    assert(tail < temp + sizeof(temp) - 1);
    *(tail++) = (char)toupper(*c);
  }

  *tail = '\0';

  fprintf(file, "(%s && %s->kind == CB_NODE_%s", c_value, c_value, temp);

  if (!is_root) {
    fprintf(file, " && !bitset_get(s->is_root, %s->id)", c_value);
  }

  for (int i = 0; i < node->arity; ++i) {
    if (node->children[i]->kind == NODE_SUBTREE) {
      fprintf(file, " && ");
      snprintf(temp, sizeof(temp), "IN(%s, %d)", c_value, i);
      write_node_match(file, temp, node->children[i], false);
    }
  }

  fprintf(file, ")");
}

static void write_leaves(FILE* file, node_t* in, char* c_value, bool push) {
  if (!push) {
    if (in->binding) {
      fprintf(file, "      cb_node_t* %s = %s;\n", in->binding, c_value);
    }
  }

  for (int i = 0; i < in->arity; ++i) {
    node_t* child = in->children[i];

    switch (child->kind) {
      default:
        assert(false);
        break;

      case NODE_SUBTREE: {
        char temp[512];
        snprintf(temp, sizeof(temp), "IN(%s, %d)", c_value, i);
        write_leaves(file, child, temp, push);
      } break;

      case NODE_LEAF: {
        if (push) {
          fprintf(file, "      vec_put(s->stack, bool_node(false, IN(%s, %d)));\n", c_value, i);
        }
        else {
          fprintf(file, "      cb_node_t* leaf_%s = IN(%s, %d);\n", child->name, c_value, i);
        }
      } break;
    }
  }
}

static void write_node_input_name(FILE* file, int* ids, node_t* node, int index) {
  switch (node->children[index]->kind) {
    default:
      fprintf(file, "n%d", ids[index]);
      break;
    case NODE_LEAF:
      fprintf(file, "NULL");
      break;
    case NODE_CODE_LITERAL:
      fprintf(file, "%s", node->children[index]->name);
      break;
  }
}

static int write_node_creation(FILE* file, node_t* node, int* id) {
  int my_id = (*id)++;

  int ids[MAX_ARITY];

  for (int i = 0; i < node->arity; ++i) {
    node_t* child = node->children[i];

    if (child->kind == NODE_SUBTREE) {
      ids[i] = write_node_creation(file, child, id);
    }
  }

  fprintf(file, "      cb_node_t* n%d = targ_node_%s(s", my_id, node->name);

  for (int i = 0; i < node->arity; ++i) {
    fprintf(file, ", ");
    write_node_input_name(file, ids, node, i);
  }

  fprintf(file, ");\n");

  for (int i = 0; i < node->arity; ++i) {
    if (node->children[i]->kind == NODE_LEAF) {
      fprintf(file, "      map_input(s, n%d, %d, leaf_%s);\n", my_id, i, node->children[i]->name);
    }
  }

  return my_id;
}

static int rule_cmp(const void* a, const void* b) {
  rule_t* ra = *(rule_t**)a;
  rule_t* rb = *(rule_t**)b;

  return rb->in->subtree_count - ra->in->subtree_count;
}

static void format_uppercase(char* buf, size_t buf_size, const char* name) {
  int i = 0;
  for (;name[i]; ++i) {
    if (i >= buf_size-1) {
      printf("Name limit reached.\n");
      exit(1);
    }

    buf[i] = (char)toupper(name[i]);
  } 

  buf[i] = '\0';
}

int main(int argc, char** argv) {
  if (argc != 3) {
    printf("Usage: %s <pats> <dfa>\n", argv[0]);
    return 1;
  }

  const char* pats_path = argv[1];
  const char* dfa_path = argv[2];

  parse_system(pats_path);

  FILE* file;
  if(fopen_s(&file, dfa_path, "w")) {
    printf("Failed to write '%s'\n", dfa_path);
    return 1;
  }

  fprintf(file, "#pragma once\n\n");
  fprintf(file, "#include \"back/internal.h\"\n\n");

  fprintf(file, "#define IN(node, input) (assert(input < (node->num_ins)), node->ins[input])\n\n");

  for (int i = 0; i < TABLE_CAPACITY; ++i) {
    op_entry_t* e = table + i;

    if (!e->name || e->rule_count == 0) {
      continue;
    }

    char uppercase_name[512];
    format_uppercase(uppercase_name, sizeof(uppercase_name), e->name);

    fprintf(file, "int bottom_up_dp_%s(sel_context_t* s, cb_node_t* node) {\n", uppercase_name);

    fprintf(file, "  (void)s;\n");

    int sorted_count = 0;
    rule_t* sorted[512];

    foreach_list (rule_t, r, e->rules_head) {
      if (sorted_count == ARRAY_LENGTH(sorted)) {
        printf("Maximum rule per node kind reached.\n");
        return 1;
      }

      sorted[sorted_count++] = r;
    }

    qsort(sorted, sorted_count, sizeof(sorted[0]), rule_cmp);

    for (int j = 0; j < sorted_count; ++j) {
      rule_t* r = sorted[j];

      fprintf(file, "  if(");
      write_node_match(file, "node", r->in, true);
      fprintf(file, ") {\n");
      fprintf(file, "    return %d;\n", r->id);
      fprintf(file, "  }\n");
    }

    fprintf(file, "  return -1;\n");
    fprintf(file, "}\n\n");

    fprintf(file, "void push_leaves_%s(sel_context_t* s, cb_node_t* node) {\n", uppercase_name);
    fprintf(file, "  switch (bottom_up_dp_%s(s, node)) {\n", uppercase_name);
    fprintf(file, "    default: assert(false); break;\n");

    for (int j = 0; j < sorted_count; ++j) {
      rule_t* r = sorted[j];

      fprintf(file, "    case %d: {\n", r->id);
      write_leaves(file, r->in, "node", true);
      fprintf(file, "    } break;\n");
    }

    fprintf(file, "  }\n");

    fprintf(file, "}\n\n");

    fprintf(file, "cb_node_t* top_down_select_%s(sel_context_t* s, cb_node_t* node) {\n", uppercase_name);

    fprintf(file, "  switch (bottom_up_dp_%s(s, node)) {\n", uppercase_name);
    fprintf(file, "    default: assert(false); return NULL;\n");

    for (int j = 0; j < sorted_count; ++j) {
      rule_t* r = sorted[j];

      fprintf(file, "    case %d: {\n", r->id);

      write_leaves(file, r->in, "node", false);

      int id = 0;
      int root_id = write_node_creation(file, r->out, &id);

      fprintf(file, "      return n%d;\n", root_id);

      fprintf(file, "    }\n");
    }

    fprintf(file, "  }\n");

    fprintf(file, "}\n\n");
  }

  fclose(file);

  return 0;
}