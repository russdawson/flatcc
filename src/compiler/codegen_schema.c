#include <stdio.h>
#include <stdlib.h>
#include "symbols.h"
#include "parser.h"
#include "codegen.h"
#include "fileio.h"
#include "flatcc/reflection/reflection_builder.h"
/* Needed to store length prefix. */
#include "flatcc/flatcc_endian.h"


typedef struct entry entry_t;
typedef entry_t object_entry_t;
typedef entry_t enum_entry_t;

struct entry {
    fb_compound_type_t *ct;
    char *name;
};

typedef struct catalog catalog_t;

struct catalog {
    int qualify_names;
    int nobjects;
    int nenums;
    int name_table_size;
    object_entry_t *objects;
    enum_entry_t *enums;
    char *name_table;
    reflection_Object_ref_t *object_map;
    object_entry_t *next_object;
    enum_entry_t *next_enum;
    char *next_name;
};

static void count_symbol(void *context, fb_symbol_t *sym)
{
    catalog_t *catalog = context;
    fb_ref_t *scope_name;
    int n = 0;

    /*
     * Find out how much space the name requires. We must store each
     * name in full for sorting because comparing a variable number of
     * parent scope names is otherwise tricky.
     */
    if (catalog->qualify_names) {
        scope_name = ((fb_compound_type_t *)sym)->scope->name;
        while (scope_name) {
            /* + 1 for '.'. */
            n += scope_name->ident->len + 1;
            scope_name = scope_name->link;
        }
    }
    /* + 1 for '\0'. */
    n += sym->ident->len + 1;
    catalog->name_table_size += n;

    switch (sym->kind) {
    case fb_is_struct:
    case fb_is_table:
        ++catalog->nobjects;
        break;
    case fb_is_union:
    case fb_is_enum:
        ++catalog->nenums;
        break;
    default: return;
    }
}

static void install_symbol(void *context, fb_symbol_t *sym)
{
    catalog_t *catalog = context;
    fb_ref_t *scope_name;
    int n = 0;
    char *s, *name;

    s = catalog->next_name;
    name = s;
    if (catalog->qualify_names) {
        scope_name = ((fb_compound_type_t *)sym)->scope->name;
        while (scope_name) {
            n = scope_name->ident->len;
            memcpy(s, scope_name->ident->text, n);
            s += n;
            *s++ = '.';
            scope_name = scope_name->link;
        }
    }
    n = sym->ident->len;
    memcpy(s, sym->ident->text, n);
    s += n;
    *s++ = '\0';
    catalog->next_name = s;

    switch (sym->kind) {
    case fb_is_struct:
    case fb_is_table:
        catalog->next_object->ct = (fb_compound_type_t *)sym;
        catalog->next_object->name = name;
        catalog->next_object++;
        break;
    case fb_is_union:
    case fb_is_enum:
        catalog->next_enum->ct = (fb_compound_type_t *)sym;
        catalog->next_enum->name = name;
        catalog->next_enum++;
        break;
    default: break;
    }
}

static void count_symbols(void *context, fb_scope_t *scope)
{
    fb_symbol_table_visit(&scope->symbol_index, count_symbol, context);
}

static void install_symbols(void *context, fb_scope_t *scope)
{
    fb_symbol_table_visit(&scope->symbol_index, install_symbol, context);
}

int compare_entries(const void *x, const void *y)
{
    return strcmp(((const entry_t *)x)->name, ((const entry_t *)y)->name);
}

static void sort_entries(entry_t *entries, int count)
{
    int i;

    qsort(entries, count, sizeof(entries[0]), compare_entries);

    for (i = 0; i < count; ++i) {
        entries[i].ct->export_index = (size_t)i;
    }
}

#define BaseType(x) FLATBUFFERS_WRAP_NAMESPACE(reflection_BaseType, x)

static reflection_Type_ref_t export_type(flatcc_builder_t *B, fb_value_t type)
{
    fb_scalar_type_t st = fb_missing_type;
    int32_t index = -1;
    reflection_BaseType_enum_t base_type = BaseType(None);
    reflection_BaseType_enum_t element = BaseType(None);
    reflection_BaseType_enum_t primitive = BaseType(None);

    switch (type.type) {
    case vt_scalar_type:
        st = type.st;
        break;
    case vt_vector_type:
        st = type.st;
        base_type = BaseType(Vector);
        break;
    case vt_vector_string_type:
        element = BaseType(String);
        base_type = BaseType(Vector);
        break;
    case vt_vector_compound_type_ref:
        index = (int32_t)type.ct->export_index;
        switch (type.ct->symbol.kind) {
        case fb_is_enum:
            st = type.ct->type.st;
            base_type = BaseType(Vector);
            break;
        case fb_is_struct:
        case fb_is_table:
            base_type = BaseType(Vector);
            element = BaseType(Obj);
            break;
        default:
            break;
        }
        break;
    case vt_string:
        base_type = BaseType(String);
        break;
    case vt_compound_type_ref:
        index = (int32_t)type.ct->export_index;
        switch (type.ct->symbol.kind) {
        case fb_is_enum:
            st = type.ct->type.st;
            break;
        case fb_is_struct:
        case fb_is_table:
            base_type = BaseType(Obj);
            break;
        case fb_is_union:
            base_type = BaseType(Union);
            break;
        default:
            index = -1;
            break;
        }
        break;
    default:
        break;
    }
    /* If st is set, resolve scalar type and set it to base_type or element. */
    switch (st) {
    case fb_missing_type: break;
    case fb_ulong: primitive = BaseType(ULong); break;
    case fb_uint: primitive = BaseType(UInt); break;
    case fb_ushort: primitive = BaseType(UShort); break;
    case fb_ubyte: primitive = BaseType(UByte); break;
    case fb_bool: primitive = BaseType(Bool); break;
    case fb_long: primitive = BaseType(Long); break;
    case fb_int: primitive = BaseType(Int); break;
    case fb_short: primitive = BaseType(Short); break;
    case fb_byte: primitive = BaseType(Byte); break;
    case fb_double: primitive = BaseType(Double); break;
    case fb_float: primitive = BaseType(Float); break;
    default: break;
    }

    if (base_type == BaseType(None)) {
        base_type = primitive;
    } else if (base_type == BaseType(Vector)) {
        if (element == BaseType(None)) {
            element = primitive;
        }
    }
    return reflection_Type_create(B, base_type, element, index);
}

static void export_fields(flatcc_builder_t *B, fb_compound_type_t *ct)
{
    fb_symbol_t *sym;
    fb_member_t *member;
    flatbuffers_bool_t has_key, deprecated, required, key_processed = 0;
    int64_t default_integer;
    double default_real;

    for (sym = ct->members; sym; sym = sym->link) {
        member = (fb_member_t *)sym;
        /*
         * Unlike `flatc` we allow multiple keys in the parser, but
         * there is no way to tell which key is default in the
         * reflection schema because the fields are sorted, so we only
         * export the default (first) key.
         */
        has_key = !key_processed && (member->metadata_flags & fb_f_key) != 0;
        required = (member->metadata_flags & fb_f_required) != 0;
        default_integer = 0;
        default_real = 0.0;
        deprecated = (member->metadata_flags & fb_f_deprecated) != 0;

        if (member->type.type == vt_compound_type_ref && member->type.ct->symbol.kind == fb_is_union) {
            reflection_Field_push_start(B);
            reflection_Field_name_start(B, 0);
            reflection_Field_name_append(B, member->symbol.ident->text, member->symbol.ident->len);
            reflection_Field_name_append(B, "_type", 5);
            reflection_Field_name_end(B);
            reflection_Field_type_create(B, BaseType(UType), BaseType(None), -1);
            reflection_Field_offset_add(B, (member->id - 1 + 2) * sizeof(flatbuffers_voffset_t));
            reflection_Field_id_add(B, member->id - 1);
            reflection_Field_deprecated_add(B, deprecated);
            reflection_Field_push_end(B);
        }
        reflection_Field_push_start(B);
        reflection_Field_name_create(B, member->symbol.ident->text, member->symbol.ident->len);
        reflection_Field_type_add(B, export_type(B, member->type));
        switch (ct->symbol.kind) {
        case fb_is_table:
            switch (member->value.type) {
            case vt_uint:
                default_integer = (int64_t)member->value.u;
                break;
            case vt_int:
                default_integer = (int64_t)member->value.i;
                break;
            case vt_bool:
                default_integer = (int64_t)member->value.b;
                break;
            case vt_float:
                default_real = member->value.f;
                break;
            }
            reflection_Field_default_integer_add(B, default_integer);
            reflection_Field_default_real_add(B, default_real);
            reflection_Field_id_add(B, member->id);
            reflection_Field_offset_add(B, (member->id + 2) * sizeof(flatbuffers_voffset_t));
            reflection_Field_key_add(B, has_key);
            reflection_Field_required_add(B, required);
            break;
        case fb_is_struct:
            reflection_Field_offset_add(B, member->offset);
            break;
        default: break;
        }
        /* Deprecated struct fields not supported by `flatc` but is here as an option. */
        reflection_Field_deprecated_add(B, deprecated);
        reflection_Field_push_end(B);
        key_processed |= has_key;
    }
}

/* `vec` is filled with references to the constructed objects. */
static void export_objects(flatcc_builder_t *B, object_entry_t *objects, int nobjects,
        reflection_Object_ref_t *object_map)
{
    int i, is_struct;
    fb_compound_type_t *ct;

    for (i = 0; i < nobjects; ++i) {
        ct = objects[i].ct;
        reflection_Object_start(B);
        reflection_Object_name_create_str(B, objects[i].name);
        /*
         * We can post sort-fields because the index is not used, unlike
         * objects and enums.
         */
        reflection_Object_fields_start(B, 0);
        export_fields(B, ct);
        reflection_Object_fields_end(B);
        is_struct = ct->symbol.kind == fb_is_struct;
        if (is_struct) {
            reflection_Object_bytesize_add(B, ct->size);
        }
        reflection_Object_is_struct_add(B, is_struct);
        reflection_Object_minalign_add(B, ct->align);
        object_map[i] = reflection_Object_end(B);
    }
    reflection_Schema_objects_create(B, object_map, nobjects);
}

static void export_enumval(flatcc_builder_t *B, fb_member_t *member, reflection_Object_ref_t *object_map)
{
    reflection_EnumVal_push_start(B);
    reflection_EnumVal_name_create(B, member->symbol.ident->text, member->symbol.ident->len);
    if (object_map && member->type.type == vt_compound_type_ref) {
        reflection_EnumVal_object_add(B, object_map[member->type.ct->export_index]);
    }
    reflection_EnumVal_value_add(B, member->value.u);
    reflection_EnumVal_push_end(B);
}

static void export_enums(flatcc_builder_t *B, enum_entry_t *enums, int nenums,
        reflection_Object_ref_t *object_map)
{
    int i, is_union;
    fb_compound_type_t *ct;
    fb_symbol_t *sym;
    reflection_Enum_ref_t *vec;

    vec = reflection_Schema_enums_start(B, nenums);
    for (i = 0; i < nenums; ++i) {
        ct = enums[i].ct;
        is_union = ct->symbol.kind == fb_is_union;
        reflection_Enum_start(B);
        reflection_Enum_name_create_str(B, enums[i].name);
        reflection_Enum_values_start(B, 0);
        for (sym = ct->members; sym; sym = sym->link) {
            export_enumval(B, (fb_member_t *)sym, is_union ? object_map : 0);
        }
        reflection_Enum_values_end(B);
        reflection_Enum_is_union_add(B, is_union);
        reflection_Enum_underlying_type_add(B, export_type(B, ct->type));
        vec[i] = reflection_Enum_end(B);
    }
    reflection_Schema_enums_end(B);
}

static void export_root_type(flatcc_builder_t *B, fb_symbol_t * root_type,
        reflection_Object_ref_t *object_map)
{
    fb_compound_type_t *ct;
    if (root_type) {
        /*
         * We could also store a struct object here, but since the
         * binrary schema says root_table, not root_type as in the text
         * schema, it would be misleading.
         */
        if (root_type->kind == fb_is_table) {
            ct = (fb_compound_type_t *)root_type;
            reflection_Schema_root_table_add(B, object_map[ct->export_index]);
        }
    }
}


static int export_schema(flatcc_builder_t *B, fb_options_t *opts, fb_schema_t *S)
{
    catalog_t catalog;

    memset(&catalog, 0, sizeof(catalog));

    catalog.qualify_names = opts->bgen_qualify_names;
    /* Build support datastructures before export. */
    fb_scope_table_visit(&S->root_schema->scope_index, count_symbols, &catalog);
    catalog.objects = calloc(catalog.nobjects, sizeof(catalog.objects[0]));
    catalog.enums = calloc(catalog.nenums, sizeof(catalog.enums[0]));
    catalog.name_table = malloc(catalog.name_table_size);
    catalog.object_map = malloc(catalog.nobjects * sizeof(catalog.object_map[0]));
    catalog.next_object = catalog.objects;
    catalog.next_enum = catalog.enums;
    catalog.next_name = catalog.name_table;

    fb_scope_table_visit(&S->root_schema->scope_index, install_symbols, &catalog);
    /* Presort objects and enums because the sorted index is required in Type tables. */
    sort_entries(catalog.objects, catalog.nobjects);
    sort_entries(catalog.enums, catalog.nenums);

    /* Build the schema. */

    reflection_Schema_start_as_root(B);
    if (S->file_identifier.type == vt_string) {
        reflection_Schema_file_ident_create(B,
                S->file_identifier.s.s, S->file_identifier.s.len);
    }
    if (S->file_extension.type == vt_string) {
        reflection_Schema_file_ext_create(B,
                S->file_extension.s.s, S->file_extension.s.len);
    }
    export_objects(B, catalog.objects, catalog.nobjects, catalog.object_map);
    export_enums(B, catalog.enums, catalog.nenums, catalog.object_map);
    export_root_type(B, S->root_type.type, catalog.object_map);

    reflection_Schema_end_as_root(B);

    /* Clean up support datastructures. */

    free(catalog.objects);
    free(catalog.enums);
    free(catalog.name_table);
    free(catalog.object_map);
    return 0;
}

/* Field sorting is easier done on the finished buffer. */
static void sort_fields(void *buffer)
{
    size_t i;
    reflection_Schema_table_t schema;
    reflection_Object_vec_t objects;
    reflection_Object_table_t object;
    reflection_Field_vec_t fields;
    reflection_Field_mutable_vec_t mfields;

    schema = reflection_Schema_as_root(buffer);
    objects = reflection_Schema_objects(schema);
    for (i = 0; i < reflection_Object_vec_len(objects); ++i) {
        object = reflection_Object_vec_at(objects, i);
        fields = reflection_Object_fields(object);
        mfields = (reflection_Field_mutable_vec_t)fields;
        reflection_Field_vec_sort(mfields);
    }
}

static FILE *open_file(fb_options_t *opts, fb_schema_t *S)
{
    FILE *fp = 0;
    char *path;
    const char *prefix = opts->outpath ? opts->outpath : "";
    int prefix_len = strlen(prefix);
    const char *name;
    int len;
    const char *ext;

    name = S->basename;
    len = strlen(name);

    ext = flatbuffers_extension;

    /* We generally should not use cgen options here, but in this case it makes sense. */
    if (opts->gen_stdout) {
        return stdout;
    }
    checkmem((path = fb_create_join_path(prefix, prefix_len, name, len, ext, 1)));
    fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "error opening file for writing binary schema: %s\n", path);
    }
    free(path);
    return fp;
}

static void close_file(FILE *fp)
{
    if (fp && fp != stdout) {
        fclose(fp);
    }
}

/*
 * Normally enums are required to be ascending in the schema and
 * therefore there is no need to sort enums. If not, we export them in
 * the order defined anyway becuase there is no well-defined ordering
 * and blindly sorting the content would just loose more information.
 *
 * In conclusion: find by enum value is only support when enums are
 * defined in consequtive order.
 *
 * refers to: `opts->ascending_enum`
 *
 * `size` must hold the maximum buffer size.
 * Returns intput buffer if successful and updates size argument.
 */
void *fb_codegen_bfbs_to_buffer(fb_options_t *opts, fb_schema_t *S, void *buffer, size_t *size)
{
    flatcc_builder_t builder, *B;

    B = &builder;
    flatcc_builder_init(B);
    export_schema(B, opts, S);
    if (!flatcc_builder_copy_buffer(B, buffer, *size)) {
        goto done;
    }
    sort_fields(buffer);
done:
    *size = flatcc_builder_get_buffer_size(B);
    flatcc_builder_clear(B);
    return buffer;
}

/*
 * Like to_buffer, but returns allocated buffer.
 * Updates size argument with buffer size if not null.
 * Returned buffer must be deallocatd with `free`.
 * The buffer is malloc aligned which should suffice for reflection buffers.
 */
void *fb_codegen_bfbs_alloc_buffer(fb_options_t *opts, fb_schema_t *S, size_t *size)
{
    flatcc_builder_t builder, *B;
    void *buffer;

    B = &builder;
    flatcc_builder_init(B);
    export_schema(B, opts, S);
    if (!(buffer = flatcc_builder_finalize_buffer(B, size))) {
        goto done;
    }
    sort_fields(buffer);
done:
    flatcc_builder_clear(B);
    return buffer;
}

int fb_codegen_bfbs_to_file(fb_options_t *opts, fb_schema_t *S)
{
    void *buffer;
    size_t size;
    FILE *fp;
    int ret = -1;

    fp = open_file(opts, S);
    if (!fp) {
        return -1;
    }
    buffer = fb_codegen_bfbs_alloc_buffer(opts, S, &size);
    if (!buffer) {
        printf("failed to generate binary schema\n");
        goto done;
    }
    if (opts->bgen_length_prefix) {
        flatbuffers_uoffset_t length = flatbuffers_store_uoffset(size);
        if (sizeof(flatbuffers_uoffset_t) != fwrite(&length, 1, sizeof(length), fp)) {
            fprintf(stderr, "cound not write binary schema to file\n");
            goto done;
        }
    }
    if (size != fwrite(buffer, 1, size, fp)) {
        fprintf(stderr, "cound not write binary schema to file\n");
        goto done;
    }
    ret = 0;
done:
    if (buffer) {
        free(buffer);
    }
    close_file(fp);
    return ret;
}