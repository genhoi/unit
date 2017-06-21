
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_conf.h>
#if 0
#include <math.h>
#include <float.h>
#endif


#define NXT_CONF_JSON_STR_SIZE  14


typedef enum {
    NXT_CONF_JSON_NULL = 0,
    NXT_CONF_JSON_BOOLEAN,
    NXT_CONF_JSON_INTEGER,
    NXT_CONF_JSON_NUMBER,
    NXT_CONF_JSON_SHORT_STRING,
    NXT_CONF_JSON_STRING,
    NXT_CONF_JSON_ARRAY,
    NXT_CONF_JSON_OBJECT,
} nxt_conf_json_type_t;


typedef enum {
    NXT_CONF_JSON_OP_PASS = 0,
    NXT_CONF_JSON_OP_CREATE,
    NXT_CONF_JSON_OP_REPLACE,
    NXT_CONF_JSON_OP_DELETE,
} nxt_conf_json_op_action_t;


typedef struct nxt_conf_json_array_s   nxt_conf_json_array_t;
typedef struct nxt_conf_json_object_s  nxt_conf_json_object_t;


struct nxt_conf_json_value_s {
    union {
        uint32_t                boolean;  /* 1 bit. */
        int64_t                 integer;
     /* double                  number; */
        u_char                  str[1 + NXT_CONF_JSON_STR_SIZE];
        nxt_str_t               *string;
        nxt_conf_json_array_t   *array;
        nxt_conf_json_object_t  *object;
    } u;

    nxt_conf_json_type_t        type:8;   /* 3 bits. */
};


struct nxt_conf_json_array_s {
    nxt_uint_t                  count;
    nxt_conf_json_value_t       elements[];
};


typedef struct {
    nxt_conf_json_value_t       name;
    nxt_conf_json_value_t       value;
} nxt_conf_json_obj_member_t;


struct nxt_conf_json_object_s {
    nxt_uint_t                  count;
    nxt_conf_json_obj_member_t  members[];
};


struct nxt_conf_json_op_s {
    uint32_t            index;
    uint32_t            action;  /* nxt_conf_json_op_action_t */
    void                *ctx;
    nxt_conf_json_op_t  *next;
};


static u_char *nxt_conf_json_skip_space(u_char *pos, u_char *end);
static u_char *nxt_conf_json_parse_value(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool);
static u_char *nxt_conf_json_parse_object(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool);
static nxt_int_t nxt_conf_json_object_hash_add(nxt_lvlhsh_t *lvlhsh,
    nxt_conf_json_obj_member_t *member, nxt_mp_t *pool);
static nxt_int_t nxt_conf_json_object_hash_test(nxt_lvlhsh_query_t *lhq,
    void *data);
static void *nxt_conf_json_object_hash_alloc(void *data, size_t size);
static void nxt_conf_json_object_hash_free(void *data, void *p);
static u_char *nxt_conf_json_parse_array(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool);
static u_char *nxt_conf_json_parse_string(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool);
static u_char *nxt_conf_json_parse_number(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool);

static nxt_int_t nxt_conf_json_copy_value(nxt_conf_json_value_t *dst,
    nxt_conf_json_value_t *src, nxt_conf_json_op_t *op, nxt_mp_t *pool);
static nxt_int_t nxt_conf_json_copy_object(nxt_conf_json_value_t *dst,
    nxt_conf_json_value_t *src, nxt_conf_json_op_t *op, nxt_mp_t *pool);

static uintptr_t nxt_conf_json_print_integer(u_char *pos,
    nxt_conf_json_value_t *value);
static uintptr_t nxt_conf_json_print_string(u_char *pos,
    nxt_conf_json_value_t *value);
static uintptr_t nxt_conf_json_print_array(u_char *pos,
    nxt_conf_json_value_t *value, nxt_conf_json_pretty_t *pretty);
static uintptr_t nxt_conf_json_print_object(u_char *pos,
    nxt_conf_json_value_t *value, nxt_conf_json_pretty_t *pretty);

static uintptr_t nxt_conf_json_escape(u_char *dst, u_char *src, size_t size);


#define nxt_conf_json_newline(pos)                                            \
    ((pos)[0] = '\r', (pos)[1] = '\n', (pos) + 2)

nxt_inline u_char *
nxt_conf_json_indentation(u_char *pos, nxt_conf_json_pretty_t *pretty)
{
    nxt_uint_t  i;

    for (i = 0; i < pretty->level; i++) {
        pos[i] = '\t';
    }

    return pos + pretty->level;
}


typedef struct {
    u_char      *start;
    u_char      *end;
    nxt_bool_t  last;
} nxt_conf_path_parse_t;


static void nxt_conf_json_path_next_token(nxt_conf_path_parse_t *parse,
    nxt_str_t *token);


nxt_conf_json_value_t *
nxt_conf_json_get_value(nxt_conf_json_value_t *value, nxt_str_t *path)
{
    nxt_str_t              token;
    nxt_conf_path_parse_t  parse;

    parse.start = path->start;
    parse.end = path->start + path->length;
    parse.last = 0;

    do {
        nxt_conf_json_path_next_token(&parse, &token);

        if (token.length == 0) {

            if (parse.last) {
                break;
            }

            return NULL;
        }

        value = nxt_conf_json_object_get_member(value, &token, NULL);

        if (value == NULL) {
            return NULL;
        }

    } while (parse.last == 0);

    return value;
}


static void
nxt_conf_json_path_next_token(nxt_conf_path_parse_t *parse, nxt_str_t *token)
{
    u_char  *p, *end;

    end = parse->end;
    p = parse->start + 1;

    token->start = p;

    while (p < end && *p != '/') {
        p++;
    }

    parse->start = p;
    parse->last = (p >= end);

    token->length = p - token->start;
}


nxt_conf_json_value_t *
nxt_conf_json_object_get_member(nxt_conf_json_value_t *value, nxt_str_t *name,
    uint32_t *index)
{
    nxt_str_t                   str;
    nxt_uint_t                  n;
    nxt_conf_json_object_t      *object;
    nxt_conf_json_obj_member_t  *member;

    if (value->type != NXT_CONF_JSON_OBJECT) {
        return NULL;
    }

    object = value->u.object;

    for (n = 0; n < object->count; n++) {
        member = &object->members[n];

        if (member->name.type == NXT_CONF_JSON_SHORT_STRING) {
            str.length = member->name.u.str[0];
            str.start = &member->name.u.str[1];

        } else {
            str = *member->name.u.string;
        }

        if (nxt_strstr_eq(&str, name)) {

            if (index != NULL) {
                *index = n;
            }

            return &member->value;
        }
    }

    return NULL;
}


nxt_int_t
nxt_conf_json_op_compile(nxt_conf_json_value_t *object,
    nxt_conf_json_value_t *value, nxt_conf_json_op_t **ops, nxt_str_t *path,
    nxt_mp_t *pool)
{
    nxt_str_t                   token;
    nxt_conf_json_op_t          *op, **parent;
    nxt_conf_path_parse_t       parse;
    nxt_conf_json_obj_member_t  *member;

    parse.start = path->start;
    parse.end = path->start + path->length;
    parse.last = 0;

    parent = ops;

    for ( ;; ) {
        op = nxt_mp_zget(pool, sizeof(nxt_conf_json_op_t));
        if (nxt_slow_path(op == NULL)) {
            return NXT_ERROR;
        }

        *parent = op;
        parent = (nxt_conf_json_op_t **) &op->ctx;

        nxt_conf_json_path_next_token(&parse, &token);

        object = nxt_conf_json_object_get_member(object, &token, &op->index);

        if (parse.last) {
            break;
        }

        if (object == NULL) {
            return NXT_DECLINED;
        }

        op->action = NXT_CONF_JSON_OP_PASS;
    }

    if (value == NULL) {

        if (object == NULL) {
            return NXT_DECLINED;
        }

        op->action = NXT_CONF_JSON_OP_DELETE;

        return NXT_OK;
    }

    if (object == NULL) {

        member = nxt_mp_zget(pool, sizeof(nxt_conf_json_obj_member_t));
        if (nxt_slow_path(member == NULL)) {
            return NXT_ERROR;
        }

        if (token.length > NXT_CONF_JSON_STR_SIZE) {

            member->name.u.string = nxt_mp_get(pool, sizeof(nxt_str_t));
            if (nxt_slow_path(member->name.u.string == NULL)) {
                return NXT_ERROR;
            }

            *member->name.u.string = token;
            member->name.type = NXT_CONF_JSON_STRING;

        } else {
            member->name.u.str[0] = token.length;
            nxt_memcpy(&member->name.u.str[1], token.start, token.length);

            member->name.type = NXT_CONF_JSON_SHORT_STRING;
        }

        member->value = *value;

        op->action = NXT_CONF_JSON_OP_CREATE;
        op->ctx = member;

    } else {
        op->action = NXT_CONF_JSON_OP_REPLACE;
        op->ctx = value;
    }

    return NXT_OK;
}


nxt_conf_json_value_t *
nxt_conf_json_clone_value(nxt_conf_json_value_t *value, nxt_conf_json_op_t *op,
    nxt_mp_t *pool)
{
    nxt_conf_json_value_t  *copy;

    copy = nxt_mp_get(pool, sizeof(nxt_conf_json_value_t));
    if (nxt_slow_path(copy == NULL)) {
        return NULL;
    }

    if (nxt_slow_path(nxt_conf_json_copy_value(copy, value, op, pool)
                      != NXT_OK))
    {
        return NULL;
    }

    return copy;
}


static nxt_int_t
nxt_conf_json_copy_value(nxt_conf_json_value_t *dst, nxt_conf_json_value_t *src,
    nxt_conf_json_op_t *op, nxt_mp_t *pool)
{
    size_t      size;
    nxt_int_t   rc;
    nxt_uint_t  n;

    if (op != NULL && src->type != NXT_CONF_JSON_OBJECT) {
        return NXT_ERROR;
    }

    dst->type = src->type;

    switch (src->type) {

    case NXT_CONF_JSON_STRING:

        dst->u.string = nxt_str_dup(pool, NULL, src->u.string);

        if (nxt_slow_path(dst->u.string == NULL)) {
            return NXT_ERROR;
        }

        break;

    case NXT_CONF_JSON_ARRAY:

        size = sizeof(nxt_conf_json_array_t)
               + src->u.array->count * sizeof(nxt_conf_json_value_t);

        dst->u.array = nxt_mp_get(pool, size);
        if (nxt_slow_path(dst->u.array == NULL)) {
            return NXT_ERROR;
        }

        dst->u.array->count = src->u.array->count;

        for (n = 0; n < src->u.array->count; n++) {
            rc = nxt_conf_json_copy_value(&dst->u.array->elements[n],
                                          &src->u.array->elements[n],
                                          NULL, pool);

            if (nxt_slow_path(rc != NXT_OK)) {
                return NXT_ERROR;
            }
        }

        break;

    case NXT_CONF_JSON_OBJECT:
        return nxt_conf_json_copy_object(dst, src, op, pool);

    default:
        dst->u = src->u;
    }

    return NXT_OK;
}


static nxt_int_t
nxt_conf_json_copy_object(nxt_conf_json_value_t *dst,
    nxt_conf_json_value_t *src, nxt_conf_json_op_t *op, nxt_mp_t *pool)
{
    size_t                      size;
    nxt_int_t                   rc;
    nxt_uint_t                  s, d, count, index;
    nxt_conf_json_op_t          *pass_op;
    nxt_conf_json_value_t       *value;
    nxt_conf_json_obj_member_t  *member;

    count = src->u.object->count;

    if (op != NULL) {
        if (op->action == NXT_CONF_JSON_OP_CREATE) {
            count++;

        } else if (op->action == NXT_CONF_JSON_OP_DELETE) {
            count--;
        }
    }

    size = sizeof(nxt_conf_json_object_t)
            + count * sizeof(nxt_conf_json_obj_member_t);

    dst->u.object = nxt_mp_get(pool, size);
    if (nxt_slow_path(dst->u.object == NULL)) {
        return NXT_ERROR;
    }

    dst->u.object->count = count;

    s = 0;
    d = 0;

    pass_op = NULL;

    /*
     * This initialization is needed only to
     * suppress a warning on GCC 4.8 and older.
     */
    index = 0;

    do {
        if (pass_op == NULL) {
            index = (op == NULL || op->action == NXT_CONF_JSON_OP_CREATE)
                    ? src->u.object->count : op->index;
        }

        while (s != index) {
            rc = nxt_conf_json_copy_value(&dst->u.object->members[d].name,
                                          &src->u.object->members[s].name,
                                          NULL, pool);

            if (nxt_slow_path(rc != NXT_OK)) {
                return NXT_ERROR;
            }

            rc = nxt_conf_json_copy_value(&dst->u.object->members[d].value,
                                          &src->u.object->members[s].value,
                                          pass_op, pool);

            if (nxt_slow_path(rc != NXT_OK)) {
                return NXT_ERROR;
            }

            s++;
            d++;
        }

        if (pass_op != NULL) {
            pass_op = NULL;
            continue;
        }

        if (op != NULL) {
            switch (op->action) {
            case NXT_CONF_JSON_OP_PASS:
                pass_op = op->ctx;
                index++;
                break;

            case NXT_CONF_JSON_OP_CREATE:
                member = op->ctx;

                rc = nxt_conf_json_copy_value(&dst->u.object->members[d].name,
                                              &member->name, NULL, pool);

                if (nxt_slow_path(rc != NXT_OK)) {
                    return NXT_ERROR;
                }

                dst->u.object->members[d].value = member->value;

                d++;
                break;

            case NXT_CONF_JSON_OP_REPLACE:
                rc = nxt_conf_json_copy_value(&dst->u.object->members[d].name,
                                              &src->u.object->members[s].name,
                                              NULL, pool);

                if (nxt_slow_path(rc != NXT_OK)) {
                    return NXT_ERROR;
                }

                value = op->ctx;

                dst->u.object->members[d].value = *value;

                s++;
                d++;
                break;

            case NXT_CONF_JSON_OP_DELETE:
                s++;
                break;
            }

            op = op->next;
        }

    } while (d != count);

    dst->type = src->type;

    return NXT_OK;
}


nxt_conf_json_value_t *
nxt_conf_json_parse(u_char *pos, size_t length, nxt_mp_t *pool)
{
    u_char                 *end;
    nxt_conf_json_value_t  *value;

    value = nxt_mp_get(pool, sizeof(nxt_conf_json_value_t));
    if (nxt_slow_path(value == NULL)) {
        return NULL;
    }

    end = pos + length;

    pos = nxt_conf_json_skip_space(pos, end);

    if (nxt_slow_path(pos == end)) {
        return NULL;
    }

    pos = nxt_conf_json_parse_value(pos, end, value, pool);

    if (nxt_slow_path(pos == NULL)) {
        return NULL;
    }

    pos = nxt_conf_json_skip_space(pos, end);

    if (nxt_slow_path(pos != end)) {
        return NULL;
    }

    return value;
}


static u_char *
nxt_conf_json_skip_space(u_char *pos, u_char *end)
{
    for ( /* void */ ; nxt_fast_path(pos != end); pos++) {

        switch (*pos) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            continue;
        }

        break;
    }

    return pos;
}


static u_char *
nxt_conf_json_parse_value(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool)
{
    u_char  ch;

    ch = *pos;

    switch (ch) {
    case '{':
        return nxt_conf_json_parse_object(pos, end, value, pool);

    case '[':
        return nxt_conf_json_parse_array(pos, end, value, pool);

    case '"':
        return nxt_conf_json_parse_string(pos, end, value, pool);

    case 't':
        if (nxt_fast_path(end - pos >= 4
                          || nxt_memcmp(pos, (u_char *) "true", 4) == 0))
        {
            value->u.boolean = 1;
            value->type = NXT_CONF_JSON_BOOLEAN;

            return pos + 4;
        }

        return NULL;

    case 'f':
        if (nxt_fast_path(end - pos >= 5
                          || nxt_memcmp(pos, (u_char *) "false", 5) == 0))
        {
            value->u.boolean = 0;
            value->type = NXT_CONF_JSON_BOOLEAN;

            return pos + 5;
        }

        return NULL;

    case 'n':
        if (nxt_fast_path(end - pos >= 4
                          || nxt_memcmp(pos, (u_char *) "null", 4) == 0))
        {
            value->type = NXT_CONF_JSON_NULL;
            return pos + 4;
        }

        return NULL;
    }

    if (nxt_fast_path(ch == '-' || (ch - '0') <= 9)) {
        return nxt_conf_json_parse_number(pos, end, value, pool);
    }

    return NULL;
}


static const nxt_lvlhsh_proto_t  nxt_conf_json_object_hash_proto
    nxt_aligned(64) =
{
    NXT_LVLHSH_DEFAULT,
    nxt_conf_json_object_hash_test,
    nxt_conf_json_object_hash_alloc,
    nxt_conf_json_object_hash_free,
};


static u_char *
nxt_conf_json_parse_object(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool)
{
    nxt_mp_t                    *temp_pool;
    nxt_int_t                   rc;
    nxt_uint_t                  count;
    nxt_lvlhsh_t                hash;
    nxt_lvlhsh_each_t           lhe;
    nxt_conf_json_object_t      *object;
    nxt_conf_json_obj_member_t  *member, *element;

    pos = nxt_conf_json_skip_space(pos + 1, end);

    if (nxt_slow_path(pos == end)) {
        return NULL;
    }

    temp_pool = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(temp_pool == NULL)) {
        return NULL;
    }

    nxt_lvlhsh_init(&hash);

    count = 0;

    if (*pos != '}') {

        for ( ;; ) {
            count++;

            if (*pos != '"') {
                goto error;
            }

            member = nxt_mp_get(temp_pool, sizeof(nxt_conf_json_obj_member_t));
            if (nxt_slow_path(member == NULL)) {
                goto error;
            }

            pos = nxt_conf_json_parse_string(pos, end, &member->name, pool);

            if (nxt_slow_path(pos == NULL)) {
                goto error;
            }

            rc = nxt_conf_json_object_hash_add(&hash, member, temp_pool);

            if (nxt_slow_path(rc != NXT_OK)) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos, end);

            if (nxt_slow_path(pos == end || *pos != ':')) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos + 1, end);

            if (nxt_slow_path(pos == end)) {
                goto error;
            }

            pos = nxt_conf_json_parse_value(pos, end, &member->value, pool);

            if (nxt_slow_path(pos == NULL)) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos, end);

            if (nxt_slow_path(pos == end)) {
                goto error;
            }

            if (*pos == '}') {
                break;
            }

            if (nxt_slow_path(*pos != ',')) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos + 1, end);

            if (nxt_slow_path(pos == end)) {
                goto error;
            }
        }
    }

    object = nxt_mp_get(pool, sizeof(nxt_conf_json_object_t)
                              + count * sizeof(nxt_conf_json_obj_member_t));
    if (nxt_slow_path(object == NULL)) {
        goto error;
    }

    value->u.object = object;
    value->type = NXT_CONF_JSON_OBJECT;

    object->count = count;
    member = object->members;

    nxt_memzero(&lhe, sizeof(nxt_lvlhsh_each_t));
    lhe.proto = &nxt_conf_json_object_hash_proto;

    for ( ;; ) {
        element = nxt_lvlhsh_each(&hash, &lhe);

        if (element == NULL) {
            break;
        }

        *member++ = *element;
    }

    nxt_mp_destroy(temp_pool);

    return pos + 1;

error:

    nxt_mp_destroy(temp_pool);
    return NULL;
}


static nxt_int_t
nxt_conf_json_object_hash_add(nxt_lvlhsh_t *lvlhsh,
    nxt_conf_json_obj_member_t *member, nxt_mp_t *pool)
{
    nxt_lvlhsh_query_t     lhq;
    nxt_conf_json_value_t  *name;

    name = &member->name;

    if (name->type == NXT_CONF_JSON_SHORT_STRING) {
        lhq.key.length = name->u.str[0];
        lhq.key.start = &name->u.str[1];

    } else {
        lhq.key = *name->u.string;
    }

    lhq.key_hash = nxt_djb_hash(lhq.key.start, lhq.key.length);
    lhq.replace = 0;
    lhq.value = member;
    lhq.proto = &nxt_conf_json_object_hash_proto;
    lhq.pool = pool;

    return nxt_lvlhsh_insert(lvlhsh, &lhq);
}


static nxt_int_t
nxt_conf_json_object_hash_test(nxt_lvlhsh_query_t *lhq, void *data)
{
    nxt_str_t              str;
    nxt_conf_json_value_t  *name;

    name = &((nxt_conf_json_obj_member_t *) data)->name;

    if (name->type == NXT_CONF_JSON_SHORT_STRING) {
        str.length = name->u.str[0];
        str.start = &name->u.str[1];

    } else {
        str = *name->u.string;
    }

    if (nxt_strstr_eq(&lhq->key, &str)) {
        return NXT_OK;
    }

    return NXT_DECLINED;
}


static void *
nxt_conf_json_object_hash_alloc(void *data, size_t size)
{
    return nxt_mp_align(data, size, size);
}


static void
nxt_conf_json_object_hash_free(void *data, void *p)
{
    return nxt_mp_free(data, p);
}


static u_char *
nxt_conf_json_parse_array(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool)
{
    nxt_mp_t               *temp_pool;
    nxt_uint_t             count;
    nxt_list_t             *list;
    nxt_conf_json_array_t  *array;
    nxt_conf_json_value_t  *element;

    pos = nxt_conf_json_skip_space(pos + 1, end);

    if (nxt_slow_path(pos == end)) {
        return NULL;
    }

    temp_pool = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(temp_pool == NULL)) {
        return NULL;
    }

    list = nxt_list_create(temp_pool, 8, sizeof(nxt_conf_json_value_t));
    if (nxt_slow_path(list == NULL)) {
        goto error;
    }

    count = 0;

    if (*pos != ']') {

        for ( ;; ) {
            count++;

            element = nxt_list_add(list);
            if (nxt_slow_path(element == NULL)) {
                goto error;
            }

            pos = nxt_conf_json_parse_value(pos, end, element, pool);

            if (nxt_slow_path(pos == NULL)) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos, end);

            if (nxt_slow_path(pos == end)) {
                goto error;
            }

            if (*pos == ']') {
                break;
            }

            if (nxt_slow_path(*pos != ',')) {
                goto error;
            }

            pos = nxt_conf_json_skip_space(pos + 1, end);

            if (nxt_slow_path(pos == end)) {
                goto error;
            }
        }
    }

    array = nxt_mp_get(pool, sizeof(nxt_conf_json_array_t)
                             + count * sizeof(nxt_conf_json_value_t));
    if (nxt_slow_path(array == NULL)) {
        goto error;
    }

    value->u.array = array;
    value->type = NXT_CONF_JSON_ARRAY;

    array->count = count;
    element = array->elements;

    nxt_list_each(value, list) {
        *element++ = *value;
    } nxt_list_loop;

    nxt_mp_destroy(temp_pool);

    return pos + 1;

error:

    nxt_mp_destroy(temp_pool);
    return NULL;
}


static u_char *
nxt_conf_json_parse_string(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool)
{
    u_char      ch, *last, *s;
    size_t      size, surplus;
    uint32_t    utf, utf_high;
    nxt_uint_t  i;
    enum {
        sw_usual = 0,
        sw_escape,
        sw_encoded1,
        sw_encoded2,
        sw_encoded3,
        sw_encoded4,
    } state;

    pos++;

    state = 0;
    surplus = 0;

    for (last = pos; last != end; last++) {
        ch = *last;

        switch (state) {

        case sw_usual:

            if (ch == '"') {
                break;
            }

            if (ch == '\\') {
                state = sw_escape;
                continue;
            }

            if (nxt_fast_path(ch >= ' ')) {
                continue;
            }

            return NULL;

        case sw_escape:

            switch (ch) {
            case '"':
            case '\\':
            case '/':
            case 'n':
            case 'r':
            case 't':
            case 'b':
            case 'f':
                surplus++;
                state = sw_usual;
                continue;

            case 'u':
                /*
                 * Basic unicode 6 bytes "\uXXXX" in JSON
                 * and up to 3 bytes in UTF-8.
                 *
                 * Surrogate pair: 12 bytes "\uXXXX\uXXXX" in JSON
                 * and 3 or 4 bytes in UTF-8.
                 */
                surplus += 3;
                state = sw_encoded1;
                continue;
            }

            return NULL;

        case sw_encoded1:
        case sw_encoded2:
        case sw_encoded3:
        case sw_encoded4:

            if (nxt_fast_path((ch >= '0' && ch <= '9')
                              || (ch >= 'A' && ch <= 'F')))
            {
                state = (state == sw_encoded4) ? sw_usual : state + 1;
                continue;
            }

            return NULL;
        }

        break;
    }

    if (nxt_slow_path(last == end)) {
        return NULL;
    }

    size = last - pos - surplus;

    if (size > NXT_CONF_JSON_STR_SIZE) {
        value->type = NXT_CONF_JSON_STRING;
        value->u.string = nxt_str_alloc(pool, size);

        if (nxt_slow_path(value->u.string == NULL)) {
            return NULL;
        }

        s = value->u.string->start;

    } else {
        value->type = NXT_CONF_JSON_SHORT_STRING;
        value->u.str[0] = size;

        s = &value->u.str[1];
    }

    if (surplus == 0) {
        nxt_memcpy(s, pos, size);
        return last + 1;
    }

    state = 0;

    do {
        ch = *pos++;

        if (ch != '\\') {
            *s++ = ch;
            continue;
        }

        ch = *pos++;

        switch (ch) {
        case '"':
        case '\\':
        case '/':
            *s++ = ch;
            continue;

        case 'n':
            *s++ = '\n';
            continue;

        case 'r':
            *s++ = '\r';
            continue;

        case 't':
            *s++ = '\t';
            continue;

        case 'b':
            *s++ = '\b';
            continue;

        case 'f':
            *s++ = '\f';
            continue;
        }

        utf = 0;
        utf_high = 0;

        for ( ;; ) {
            for (i = 0; i < 4; i++) {
                utf = (utf << 4) + (pos[i] - (pos[i] >= 'A' ? 'A' : '0'));
            }

            pos += 4;

            if (utf < 0xd800 || utf > 0xdbff || utf_high) {
                break;
            }

            utf_high = utf;
            utf = 0;

            if (pos[0] != '\\' || pos[1] != 'u') {
                break;
            }

            pos += 2;
        }

        if (utf_high != 0) {
            if (nxt_slow_path(utf_high < 0xd800
                              || utf_high > 0xdbff
                              || utf < 0xdc00
                              || utf > 0xdfff))
            {
                /* invalid surrogate pair */
                return NULL;
            }

            utf = ((utf_high - 0xd800) << 10) + (utf - 0xdc00);
        }

        s = nxt_utf8_encode(s, utf);

    } while (pos != last);

    if (size > NXT_CONF_JSON_STR_SIZE) {
        value->u.string->length = s - value->u.string->start;

    } else {
        value->u.str[0] = s - &value->u.str[1];
    }

    return pos + 1;
}


static u_char *
nxt_conf_json_parse_number(u_char *pos, u_char *end,
    nxt_conf_json_value_t *value, nxt_mp_t *pool)
{
    u_char     ch, *p;
    uint64_t   integer;
    nxt_int_t  sign;
#if 0
    uint64_t   frac, power
    nxt_int_t  e, negative;
#endif

    static const uint64_t cutoff = NXT_INT64_T_MAX / 10;
    static const uint64_t cutlim = NXT_INT64_T_MAX % 10;

    ch = *pos;

    if (ch == '-') {
        sign = -1;
        pos++;

    } else {
        sign = 1;
    }

    integer = 0;

    for (p = pos; nxt_fast_path(p != end); p++) {
        ch = *p;

        /* Values below '0' become >= 208. */
        ch = ch - '0';

        if (ch > 9) {
            break;
        }

        if (nxt_slow_path(integer >= cutoff
                          && (integer > cutoff || ch > cutlim)))
        {
             return NULL;
        }

        integer = integer * 10 + ch;
    }

    if (nxt_slow_path(p == pos || (p > pos + 1 && *pos == '0'))) {
        return NULL;
    }

    if (ch != '.') {
        value->type = NXT_CONF_JSON_INTEGER;
        value->u.integer = sign * integer;
        return p;
    }

#if 0
    pos = p + 1;

    frac = 0;
    power = 1;

    for (p = pos; nxt_fast_path(p != end); p++) {
        ch = *p;

        /* Values below '0' become >= 208. */
        ch = ch - '0';

        if (ch > 9) {
            break;
        }

        if (nxt_slow_path((frac >= cutoff && (frac > cutoff || ch > cutlim))
                          || power > cutoff))
        {
             return NULL;
        }

        frac = frac * 10 + ch;
        power *= 10;
    }

    if (nxt_slow_path(p == pos)) {
        return NULL;
    }

    value->type = NXT_CONF_JSON_NUMBER;
    value->u.number = integer + (double) frac / power;

    value->u.number = copysign(value->u.number, sign);

    if (ch == 'e' || ch == 'E') {
        pos = p + 1;

        ch = *pos;

        if (ch == '-' || ch == '+') {
            pos++;
        }

        negative = (ch == '-') ? 1 : 0;
        e = 0;

        for (p = pos; nxt_fast_path(p != end); p++) {
            ch = *p;

            /* Values below '0' become >= 208. */
            ch = ch - '0';

            if (ch > 9) {
                break;
            }

            e = e * 10 + ch;

            if (nxt_slow_path(e > DBL_MAX_10_EXP)) {
                return NULL;
            }
        }

        if (nxt_slow_path(p == pos)) {
            return NULL;
        }

        if (negative) {
            value->u.number /= exp10(e);

        } else {
            value->u.number *= exp10(e);
        }
    }

    if (nxt_fast_path(isfinite(value->u.number))) {
        return p;
    }
#endif

    return NULL;
}


uintptr_t
nxt_conf_json_print_value(u_char *pos, nxt_conf_json_value_t *value,
    nxt_conf_json_pretty_t *pretty)
{
    switch (value->type) {

    case NXT_CONF_JSON_NULL:

        if (pos == NULL) {
            return sizeof("null") - 1;
        }

        return (uintptr_t) nxt_cpymem(pos, "null", 4);

    case NXT_CONF_JSON_BOOLEAN:

        if (pos == NULL) {
            return value->u.boolean ? sizeof("true") - 1 : sizeof("false") - 1;
        }

        if (value->u.boolean) {
            return (uintptr_t) nxt_cpymem(pos, "true", 4);
        }

        return (uintptr_t) nxt_cpymem(pos, "false", 5);

    case NXT_CONF_JSON_INTEGER:
        return nxt_conf_json_print_integer(pos, value);

    case NXT_CONF_JSON_NUMBER:
        /* TODO */
        return (pos == NULL) ? 0 : (uintptr_t) pos;

    case NXT_CONF_JSON_SHORT_STRING:
    case NXT_CONF_JSON_STRING:
        return nxt_conf_json_print_string(pos, value);

    case NXT_CONF_JSON_ARRAY:
        return nxt_conf_json_print_array(pos, value, pretty);

    case NXT_CONF_JSON_OBJECT:
        return nxt_conf_json_print_object(pos, value, pretty);
    }

    nxt_unreachable();

    return (pos == NULL) ? 0 : (uintptr_t) pos;
}


static uintptr_t
nxt_conf_json_print_integer(u_char *pos, nxt_conf_json_value_t *value)
{
    int64_t  num;

    num = value->u.integer;

    if (pos == NULL) {
        num = llabs(num);

        if (num <= 9999) {
            return sizeof("-9999") - 1;
        }

        if (num <= 99999999999) {
            return sizeof("-99999999999") - 1;
        }

        return NXT_INT64_T_LEN;
    }

    return (uintptr_t) nxt_sprintf(pos, pos + NXT_INT64_T_LEN, "%L", num);
}


static uintptr_t
nxt_conf_json_print_string(u_char *pos, nxt_conf_json_value_t *value)
{
    size_t  len;
    u_char  *s;

    if (value->type == NXT_CONF_JSON_SHORT_STRING) {
        len = value->u.str[0];
        s = &value->u.str[1];

    } else {
        len = value->u.string->length;
        s = value->u.string->start;
    }

    if (pos == NULL) {
        return 2 + len + nxt_conf_json_escape(NULL, s, len);
    }

    *pos++ = '"';

    pos = (u_char *) nxt_conf_json_escape(pos, s, len);

    *pos++ = '"';

    return (uintptr_t) pos;
}


static uintptr_t
nxt_conf_json_print_array(u_char *pos, nxt_conf_json_value_t *value,
    nxt_conf_json_pretty_t *pretty)
{
    size_t                 len;
    nxt_uint_t             n;
    nxt_conf_json_array_t  *array;

    array = value->u.array;

    if (pos == NULL) {
        /* [] */
        len = 2;

        if (pretty != NULL) {
            pretty->level++;
        }

        value = array->elements;

        for (n = 0; n < array->count; n++) {
            len += nxt_conf_json_print_value(NULL, &value[n], pretty);

            if (pretty != NULL) {
                /* indentation and new line */
                len += pretty->level + 2;
            }
        }

        if (pretty != NULL) {
            pretty->level--;

            if (n != 0) {
                /* indentation and new line */
                len += pretty->level + 2;
            }
        }

        /* reserve space for "n" commas */
        return len + n;
    }

    *pos++ = '[';

    if (array->count != 0) {
        value = array->elements;

        if (pretty != NULL) {
            pos = nxt_conf_json_newline(pos);

            pretty->level++;
            pos = nxt_conf_json_indentation(pos, pretty);
        }

        pos = (u_char *) nxt_conf_json_print_value(pos, &value[0], pretty);

        for (n = 1; n < array->count; n++) {
            *pos++ = ',';

            if (pretty != NULL) {
                pos = nxt_conf_json_newline(pos);
                pos = nxt_conf_json_indentation(pos, pretty);

                pretty->more_space = 0;
            }

            pos = (u_char *) nxt_conf_json_print_value(pos, &value[n], pretty);
        }

        if (pretty != NULL) {
            pos = nxt_conf_json_newline(pos);

            pretty->level--;
            pos = nxt_conf_json_indentation(pos, pretty);

            pretty->more_space = 1;
        }
    }

    *pos++ = ']';

    return (uintptr_t) pos;
}


static uintptr_t
nxt_conf_json_print_object(u_char *pos, nxt_conf_json_value_t *value,
    nxt_conf_json_pretty_t *pretty)
{
    size_t                      len;
    nxt_uint_t                  n;
    nxt_conf_json_object_t      *object;
    nxt_conf_json_obj_member_t  *member;

    object = value->u.object;

    if (pos == NULL) {
        /* {} */
        len = 2;

        if (pretty != NULL) {
            pretty->level++;
        }

        member = object->members;

        for (n = 0; n < object->count; n++) {
            len += nxt_conf_json_print_string(NULL, &member[n].name) + 1
                   + nxt_conf_json_print_value(NULL, &member[n].value, pretty)
                   + 1;

            if (pretty != NULL) {
                /*
                 * indentation, space after ":", new line, and possible
                 * additional empty line between non-empty objects
                 */
                len += pretty->level + 1 + 2 + 2;
            }
        }

        if (pretty != NULL) {
            pretty->level--;

            /* indentation and new line */
            len += pretty->level + 2;
        }

        return len;
    }

    *pos++ = '{';

    if (object->count != 0) {

        if (pretty != NULL) {
            pos = nxt_conf_json_newline(pos);
            pretty->level++;
        }

        member = object->members;

        n = 0;

        for ( ;; ) {
            if (pretty != NULL) {
                pos = nxt_conf_json_indentation(pos, pretty);
            }

            pos = (u_char *) nxt_conf_json_print_string(pos, &member[n].name);

            *pos++ = ':';

            if (pretty != NULL) {
                *pos++ = ' ';
            }

            pos = (u_char *) nxt_conf_json_print_value(pos, &member[n].value,
                                                       pretty);

            n++;

            if (n == object->count) {
                break;
            }

            *pos++ = ',';

            if (pretty != NULL) {
                pos = nxt_conf_json_newline(pos);

                if (pretty->more_space) {
                    pretty->more_space = 0;
                    pos = nxt_conf_json_newline(pos);
                }
            }
        }

        if (pretty != NULL) {
            pos = nxt_conf_json_newline(pos);

            pretty->level--;
            pos = nxt_conf_json_indentation(pos, pretty);

            pretty->more_space = 1;
        }
    }

    *pos++ = '}';

    return (uintptr_t) pos;
}


static uintptr_t
nxt_conf_json_escape(u_char *dst, u_char *src, size_t size)
{
    u_char  ch;
    size_t  len;

    if (dst == NULL) {
        len = 0;

        while (size) {
            ch = *src++;

            if (ch == '\\' || ch == '"') {
                len++;

            } else if (ch <= 0x1f) {

                switch (ch) {
                case '\n':
                case '\r':
                case '\t':
                case '\b':
                case '\f':
                    len++;
                    break;

                default:
                    len += sizeof("\\u001F") - 2;
                }
            }

            size--;
        }

        return len;
    }

    while (size) {
        ch = *src++;

        if (ch > 0x1f) {

            if (ch == '\\' || ch == '"') {
                *dst++ = '\\';
            }

            *dst++ = ch;

        } else {
            *dst++ = '\\';

            switch (ch) {
            case '\n':
                *dst++ = 'n';
                break;

            case '\r':
                *dst++ = 'r';
                break;

            case '\t':
                *dst++ = 't';
                break;

            case '\b':
                *dst++ = 'b';
                break;

            case '\f':
                *dst++ = 'f';
                break;

            default:
                *dst++ = 'u'; *dst++ = '0'; *dst++ = '0';
                *dst++ = '0' + (ch >> 4);

                ch &= 0xf;

                *dst++ = (ch < 10) ? ('0' + ch) : ('A' + ch - 10);
            }
        }

        size--;
    }

    return (uintptr_t) dst;
}