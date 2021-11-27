/* Lua QPack - QPack support for Lua
 */

#include <qpack/qpack.h>
#include <assert.h>
#include <string.h>
#include <math.h>
#include <limits.h>
#include <lua.h>
#include <lauxlib.h>

int siri_err;

#ifndef QPACK_MODNAME
#define QPACK_MODNAME   "qpack"
#endif

#ifndef QPACK_VERSION
#define QPACK_VERSION   "1.0devel"
#endif

#ifdef _MSC_VER
#define QPACK_EXPORT    __declspec(dllexport)
#else
#define QPACK_EXPORT    extern
#endif

/* Workaround for Solaris platforms missing isinf() */
#if !defined(isinf) && (defined(USE_INTERNAL_ISINF) || defined(MISSING_ISINF))
#define isinf(x) (!isnan(x) && isnan((x) - (x)))
#endif

#define DEFAULT_ENCODE_MAX_DEPTH 1000
#define DEFAULT_DECODE_MAX_DEPTH 1000
#define DEFAULT_ENCODE_EMPTY_TABLE_AS_ARRAY 0

typedef struct {
    int encode_max_depth;
    int decode_max_depth;
    int encode_empty_table_as_array;
} qpack_config_t;

typedef struct {
    const char *data;
    const char *ptr;
    qpack_config_t *cfg;
} qpack_parse_t;

/* ===== CONFIGURATION ===== */

static qpack_config_t *qpack_fetch_config(lua_State *l)
{
    qpack_config_t *cfg;

    cfg = (qpack_config_t *)lua_touserdata(l, lua_upvalueindex(1));
    if (!cfg)
        luaL_error(l, "BUG: Unable to fetch CJSON configuration");

    return cfg;
}

/* Ensure the correct number of arguments have been provided.
 * Pad with nil to allow other functions to simply check arg[i]
 * to find whether an argument was provided */
static qpack_config_t *qpack_arg_init(lua_State *l, int args)
{
    luaL_argcheck(l, lua_gettop(l) <= args, args + 1,
                  "found too many arguments");

    while (lua_gettop(l) < args)
        lua_pushnil(l);

    return qpack_fetch_config(l);
}

/* Process integer options for configuration functions */
static int qpack_integer_option(lua_State *l, int optindex, int *setting,
                               int min, int max)
{
    char errmsg[64];
    int value;

    if (!lua_isnil(l, optindex)) {
        value = luaL_checkinteger(l, optindex);
        snprintf(errmsg, sizeof(errmsg), "expected integer between %d and %d", min, max);
        luaL_argcheck(l, min <= value && value <= max, 1, errmsg);
        *setting = value;
    }

    lua_pushinteger(l, *setting);

    return 1;
}

/* Process enumerated arguments for a configuration function */
static int qpack_enum_option(lua_State *l, int optindex, int *setting,
                            const char **options, int bool_true)
{
    static const char *bool_options[] = { "off", "on", NULL };

    if (!options) {
        options = bool_options;
        bool_true = 1;
    }

    if (!lua_isnil(l, optindex)) {
        if (bool_true && lua_isboolean(l, optindex))
            *setting = lua_toboolean(l, optindex) * bool_true;
        else
            *setting = luaL_checkoption(l, optindex, NULL, options);
    }

    if (bool_true && (*setting == 0 || *setting == bool_true))
        lua_pushboolean(l, *setting);
    else
        lua_pushstring(l, options[*setting]);

    return 1;
}
/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int qpack_cfg_encode_max_depth(lua_State *l)
{
    qpack_config_t *cfg = qpack_arg_init(l, 1);

    return qpack_integer_option(l, 1, &cfg->encode_max_depth, 1, INT_MAX);
}

/* Configures the maximum number of nested arrays/objects allowed when
 * encoding */
static int qpack_cfg_decode_max_depth(lua_State *l)
{
    qpack_config_t *cfg = qpack_arg_init(l, 1);

    return qpack_integer_option(l, 1, &cfg->decode_max_depth, 1, INT_MAX);
}

static int qpack_cfg_encode_empty_tables_as_array(lua_State *l)
{
    qpack_config_t *cfg = qpack_arg_init(l, 1);
    return qpack_enum_option(l, 1, &cfg->encode_empty_table_as_array, NULL, 1);
}

static int qpack_destroy_config(lua_State *l)
{
    /*
    qpack_config_t *cfg;

    cfg = (qpack_config_t *)lua_touserdata(l, 1);
    cfg = NULL;
    */

    return 0;
}

static void qpack_create_config(lua_State *l)
{
    qpack_config_t *cfg;

    cfg = (qpack_config_t *)lua_newuserdata(l, sizeof(*cfg));

    /* Create GC method to clean up strbuf */
    lua_newtable(l);
    lua_pushcfunction(l, qpack_destroy_config);
    lua_setfield(l, -2, "__gc");
    lua_setmetatable(l, -2);

    cfg->encode_max_depth = DEFAULT_ENCODE_MAX_DEPTH;
    cfg->decode_max_depth = DEFAULT_DECODE_MAX_DEPTH;
    cfg->encode_empty_table_as_array = DEFAULT_ENCODE_EMPTY_TABLE_AS_ARRAY;
}

/* ===== ENCODING ===== */

static void qpack_encode_exception(lua_State *l, qpack_config_t *cfg, qp_packer_t *pk, int lindex,
                                  const char *reason)
{
    luaL_error(l, "Cannot serialise %s: %s",
                  lua_typename(l, lua_type(l, lindex)), reason);
}

/* qpack_append_string args:
 * - lua_State
 * - qp_packer_t
 * - String (Lua stack index)
 *
 * Returns nothing. Doesn't remove string from Lua stack */
static int qpack_append_string(lua_State *l, qp_packer_t *pk, int lindex)
{
    const char *str;
    size_t len;

    str = lua_tolstring(l, lindex, &len);
    // printf("%s: append string:%s len:%lu\n", __func__, str, len);

    return qp_add_string_term_n(pk, str, len);
}

/* Find the size of the array on the top of the Lua stack
 * -1   object (not a pure array)
 * >=0  elements in array
 */
static int lua_array_length(lua_State *l, qpack_config_t *cfg, qp_packer_t *pk)
{
    double k;
    int max;
    int items;

    max = 0;
    items = 0;

    lua_pushnil(l);
    /* table, startkey */
    while (lua_next(l, -2) != 0) {
        /* table, key, value */
        if (lua_type(l, -2) == LUA_TNUMBER &&
            (k = lua_tonumber(l, -2))) {
            /* Integer >= 1 ? */
            if (floor(k) == k && k >= 1) {
                if (k > max)
                    max = k;
                items++;
                lua_pop(l, 1);
                continue;
            }
        }

        /* Must not be an array (non integer key) */
        lua_pop(l, 2);
        return -1;
    }

    return max;
}

static void qpack_check_encode_depth(lua_State *l, qpack_config_t *cfg, int current_depth, qp_packer_t *pk)
{
    if (current_depth <= cfg->encode_max_depth  && lua_checkstack(l, 3))
        return;

    luaL_error(l, "Cannot serialise, excessive nesting (%d)",
               current_depth);
}

static void qpack_append_data(lua_State *l, qpack_config_t *cfg, int current_depth, qp_packer_t *pk);

/* qpack_append_array args:
 * - lua_State
 * - JSON strbuf
 * - Size of passwd Lua array (top of stack) */
static int qpack_append_array(lua_State *l, qpack_config_t *cfg, int current_depth,
                              qp_packer_t *pk, int array_length)
{
    int ret, i;
    ret = qp_add_type(pk, QP_ARRAY_OPEN);
    if (ret)
        return ret;

    for (i = 1; i <= array_length; i++) {
        lua_geti(l, -1, i);
        qpack_append_data(l, cfg, current_depth, pk);
        lua_pop(l, 1);
    }

    return qp_add_type(pk, QP_ARRAY_CLOSE);
}

static int qpack_append_null(lua_State *l, qpack_config_t *cfg,
        qp_packer_t *pk, int lindex)
{
    return qp_add_null(pk);
}

static int qpack_append_bool(lua_State *l, qpack_config_t *cfg,
        qp_packer_t *pk, int lindex)
{
    if (lua_toboolean(l, -1))
        return qp_add_true(pk);
    else
        return qp_add_false(pk);
}

static int qpack_append_number(lua_State *l, qpack_config_t *cfg,
        qp_packer_t *pk, int lindex)
{
#if LUA_VERSION_NUM >= 503
    if (lua_isinteger(l, lindex)) {
        lua_Integer num = lua_tointeger(l, lindex);
        return qp_add_int64(pk, num);
    }
#endif
    double num = lua_tonumber(l, lindex);
    return qp_add_double(pk, num); 
}

static int qpack_append_object(lua_State *l, qpack_config_t *cfg,
        int current_depth, qp_packer_t *pk)
{
    int keytype, ret;

    ret = qp_add_type(pk, QP_MAP_OPEN);
    if (ret)
        return ret;

    lua_pushnil(l);
    while (lua_next(l, -2) != 0) {
        /* table, key, value */
        keytype = lua_type(l, -2);
        if (keytype == LUA_TNUMBER) {
            qpack_append_number(l, cfg, pk, -2);
        } else if (keytype == LUA_TSTRING) {
            qpack_append_string(l, pk, -2);
        } else {
            qpack_encode_exception(l, cfg, pk, -2,
                                  "table key must be a number or string");
            /* never returns */
        }

        /* table, key, value */
        qpack_append_data(l, cfg, current_depth, pk);
        lua_pop(l, 1);
        /* table, key */
    }

    return qp_add_type(pk, QP_MAP_CLOSE);
}

/* Serialise Lua data into QPacker string. */
static void qpack_append_data(lua_State *l, qpack_config_t *cfg,
                                int current_depth, qp_packer_t *pk)
{
    int len, ret = 0;
    int dtype = lua_type(l, -1);

    switch (dtype) {
    case LUA_TSTRING:
        ret = qpack_append_string(l, pk, -1);
        break;
    case LUA_TNUMBER:
        ret = qpack_append_number(l, cfg, pk, -1);
        break;
    case LUA_TBOOLEAN:
        ret = qpack_append_bool(l, cfg, pk, -1);
        break;
    case LUA_TTABLE:
        current_depth++;
        qpack_check_encode_depth(l, cfg, current_depth, pk);
        if (luaL_getmetafield(l, -1, "__len") != LUA_TNIL) {
            lua_pushvalue(l, -2);
            lua_call(l, 1, 1);
            if (!lua_isinteger(l, -1)) {
                luaL_error(l, "__len should return integer");
            }
            len = lua_tointeger(l, -1);
            lua_pop(l, 1);
            ret = qpack_append_array(l, cfg, current_depth, pk, len);
        } else {
            len = lua_array_length(l, cfg, pk);
            if (len > 0 || (cfg->encode_empty_table_as_array && len == 0))
                ret = qpack_append_array(l, cfg, current_depth, pk, len);
            else
                ret = qpack_append_object(l, cfg, current_depth, pk);
        }
        break;
    case LUA_TNIL:
        ret = qpack_append_null(l, cfg, pk, -1);
        break;
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(l, -1) == NULL) {
            ret = qpack_append_null(l, cfg, pk, -1);
            break;
        }
    default:
        /* Remaining types (LUA_TFUNCTION, LUA_TUSERDATA, LUA_TTHREAD,
         * and LUA_TLIGHTUSERDATA) cannot be serialised */
        qpack_encode_exception(l, cfg, pk, -1, "type not supported");
        /* never returns */
    }
    if (ret) {
        luaL_error(l, "encode data type:%d failed err:%d", dtype, ret);
    }
}

static int qpack_encode(lua_State *l)
{
    qpack_config_t *cfg = qpack_fetch_config(l);
    qp_packer_t * pk = NULL;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    /* Use private buffer */
    pk = qp_packer_new(QP_SUGGESTED_SIZE);

    qpack_append_data(l, cfg, 0, pk);

    lua_pushlstring(l, (const char*)pk->buffer, pk->len);

    return 1;
}

/* ===== DECODING ===== */

static int qpack_process_obj(lua_State *l, qpack_parse_t *pk,
        qp_unpacker_t *up, qp_obj_t *obj)
{
    int ret = 0;

    switch (obj->tp) {
    case QP_ERR:
    case QP_ARRAY_CLOSE:
    case QP_MAP_CLOSE:
    case QP_END:
        luaL_error(l, "QPACK error obj->tp:%d", obj->tp);
        return -1;
        break;
    case QP_INT64:
        lua_pushinteger(l, obj->via.int64);
        break;
    case QP_DOUBLE:
        lua_pushnumber(l, obj->via.real);
        break;
    case QP_TRUE:
        lua_pushboolean(l, 1);
        break;
    case QP_FALSE:
        lua_pushboolean(l, 0);
        break;
    case QP_RAW:
        lua_pushlstring(l, (const char*)obj->via.raw, obj->len - 1);
        break;
    case QP_NULL:
        lua_pushlightuserdata(l, NULL);
        break;
    case QP_ARRAY0:
    case QP_ARRAY1:
    case QP_ARRAY2:
    case QP_ARRAY3:
    case QP_ARRAY4:
    case QP_ARRAY5:
    {
        lua_newtable(l);
        for (int i = i; i <= obj->tp - QP_ARRAY0; i++)
        {
            qp_next(up, obj);
            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;
            lua_rawseti(l, -2, i);            /* arr[i] = value */
        }
        break;
    }
    case QP_MAP0:
    case QP_MAP1:
    case QP_MAP2:
    case QP_MAP3:
    case QP_MAP4:
    case QP_MAP5:
    {
        lua_newtable(l);

        for (int i = 0; i < obj->tp - QP_MAP0; i++)
        {
            qp_next(up, obj);
            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;

            qp_next(up, obj);
            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;

            /* Set key = value */
            lua_rawset(l, -3);
        }
        break;
    }
    case QP_ARRAY_OPEN:
    {
        lua_newtable(l);
        size_t i = 1;

        while(qp_next(up, obj) && obj->tp != QP_ARRAY_CLOSE)
        {
            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;
            lua_rawseti(l, -2, i);            /* arr[i] = value */
            i++;
        }
        break;
    }
    case QP_MAP_OPEN:
    {
        lua_newtable(l);

        while(qp_next(up, obj) && obj->tp != QP_MAP_CLOSE)
        {
            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;

            qp_next(up, obj);

            ret = qpack_process_obj(l, pk, up, obj);
            if (ret)
                break;

            /* Set key = value */
            lua_rawset(l, -3);
        }
        break;
    }
    default:
    {
        ret = -1;
        luaL_error(l, "QPACK unknown obj->tp:%d", obj->tp);
    }
    }
    return ret;
}

static int qpack_decode(lua_State *l)
{
    qpack_parse_t qpack;
    size_t qpack_len;
    qp_unpacker_t up;
    qp_obj_t obj;

    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    qpack.cfg = qpack_fetch_config(l);

    qpack.data = luaL_checklstring(l, 1, &qpack_len);

    qp_unpacker_init(&up, (unsigned char*)qpack.data, qpack_len);

    qp_next(&up, &obj);
    if (obj.tp == QP_END) {
        luaL_error(l, "QPACK cannot parse empty string");
    } else {
        qpack_process_obj(l, &qpack, &up, &obj);
    }

    return 1;
}

/* ===== INITIALISATION ===== */

/* Call target function in protected mode with all supplied args.
 * Assumes target function only returns a single non-nil value.
 * Convert and return thrown errors as: nil, "error message" */
static int qpack_protect_conversion(lua_State *l)
{
    int err;

    /* Deliberately throw an error for invalid arguments */
    luaL_argcheck(l, lua_gettop(l) == 1, 1, "expected 1 argument");

    /* pcall() the function stored as upvalue(1) */
    lua_pushvalue(l, lua_upvalueindex(1));
    lua_insert(l, 1);
    err = lua_pcall(l, 1, 1, 0);
    if (!err)
        return 1;

    if (err == LUA_ERRRUN) {
        lua_pushnil(l);
        lua_insert(l, -2);
        return 2;
    }

    /* Since we are not using a custom error handler, the only remaining
     * errors are memory related */
    return luaL_error(l, "Memory allocation error in QPACK protected call");
}

/* Return qpack module table */
static int lua_qpack_new(lua_State *l)
{
    luaL_Reg reg[] = {
        { "encode", qpack_encode },
        { "decode", qpack_decode },
        { "encode_max_depth", qpack_cfg_encode_max_depth },
        { "decode_max_depth", qpack_cfg_decode_max_depth },
        { "encode_empty_table_as_array", qpack_cfg_encode_empty_tables_as_array },
        { "new", lua_qpack_new },
        { NULL, NULL }
    };

    /* qpack module table */
    lua_newtable(l);

    /* Register functions with config data as upvalue */
    qpack_create_config(l);
    luaL_setfuncs(l, reg, 1);

    /* Set qpack.null */
    lua_pushlightuserdata(l, NULL);
    lua_setfield(l, -2, "null");

    /* Set module name / version fields */
    lua_pushliteral(l, QPACK_MODNAME);
    lua_setfield(l, -2, "_NAME");
    lua_pushliteral(l, QPACK_VERSION);
    lua_setfield(l, -2, "_VERSION");

    return 1;
}

/* Return qpack.safe module table */
static int lua_qpack_safe_new(lua_State *l)
{
    const char *func[] = { "decode", "encode", NULL };
    int i;

    lua_qpack_new(l);

    /* Fix new() method */
    lua_pushcfunction(l, lua_qpack_safe_new);
    lua_setfield(l, -2, "new");

    for (i = 0; func[i]; i++) {
        lua_getfield(l, -1, func[i]);
        lua_pushcclosure(l, qpack_protect_conversion, 1);
        lua_setfield(l, -2, func[i]);
    }

    return 1;
}

QPACK_EXPORT int luaopen_qpack(lua_State *l)
{
    lua_qpack_new(l);

#ifdef ENABLE_QPACK_GLOBAL
    /* Register a global "qpack" table. */
    lua_pushvalue(l, -1);
    lua_setglobal(l, QPACK_MODNAME);
#endif

    /* Return qpack table */
    return 1;
}

QPACK_EXPORT int luaopen_qpack_safe(lua_State *l)
{
    lua_qpack_safe_new(l);

    /* Return qpack.safe table */
    return 1;
}

/* vi:ai et sw=4 ts=4:
 */
