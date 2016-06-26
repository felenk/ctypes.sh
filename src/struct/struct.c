#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <dwarf.h>
#include <search.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <link.h>
#include <ffi.h>

#include "dwarves.h"
#include "dutil.h"
#include "builtins.h"
#include "variables.h"
#include "arrayfunc.h"
#include "common.h"
#include "bashgetopt.h"
#include "util.h"
#include "types.h"
#include "shell.h"

// This is just to disable ctf support.
static int debug_fmt_error(void) { return -1; }

// Export an unused debug format to disable ctf.
struct debug_fmt_ops ctf__ops = {
    .load_file = debug_fmt_error,
};

// Used to store context inside dwarves callbacks.
struct cookie {
    char             *typename;
    int               result;
    char            **filenames;
    unsigned          nfiles;
    SHELL_VAR        *assoc;
    struct cus       *cus;
    struct conf_load *conf;
};

// Map dwarf basetypes to ctypes prefixes
static const char *prefix_for_basetype(char *basetype)
{
    static struct {
        const char *basetype;
        const char *prefix;
    } basetypemap[] = {
        { .basetype = "unsigned", .prefix = "unsigned" },
        { .basetype = "signed int", .prefix = "int" },
        { .basetype = "unsigned int", .prefix = "unsigned" },
        { .basetype = "int", .prefix = "int" },
        { .basetype = "short unsigned int", .prefix = "ushort" },
        { .basetype = "signed short", .prefix = "short" },
        { .basetype = "unsigned short", .prefix = "ushort" },
        { .basetype = "short int", .prefix = "short" },
        { .basetype = "char", .prefix = "char" },
        { .basetype = "signed char", .prefix = "char" },
        { .basetype = "unsigned char", .prefix = "uchar" },
        { .basetype = "signed long", .prefix = "long" },
        { .basetype = "long int", .prefix = "long" },
        { .basetype = "signed long", .prefix = "long" },
        { .basetype = "unsigned long", .prefix = "ulong" },
        { .basetype = "long unsigned int", .prefix = "ulong" },
        { .basetype = "bool", .prefix = "byte" },
        { .basetype = "_Bool", .prefix = "byte" },
        { .basetype = "long long unsigned int", .prefix = "uint64" },
        { .basetype = "long long int", .prefix = "int64" },
        { .basetype = "signed long long", .prefix = "int64" },
        { .basetype = "unsigned long long", .prefix = "uint64" },
        { .basetype = "double", .prefix = "double" },
        { .basetype = "double double", .prefix = "longdouble" },
        { .basetype = "single float", .prefix = "float" },
        { .basetype = "float", .prefix = "float" },
        { .basetype = "long double", .prefix = "longdouble" },
        { 0 },
    }, *n = basetypemap;

    while (n->basetype) {
        if (strcmp(n->basetype, basetype) == 0)
            return n->prefix;
        n++;
    }

    return NULL;
};

// This gets called once for every compilation unit, and we're expected to
// search it to see if it contains something we're interested in.
static enum load_steal_kind create_array_stealer(struct cu *cu, struct conf_load *conf_load)
{
    static uint16_t class_id;
    struct tag *tag;
    struct class *class;
    struct class_member *member;
    struct cookie *cookie = conf_load->cookie;
    struct conf_fprintf conf = {0};

    // Check if this compilation unit contains the structname requested.
    if (!(tag = cu__find_struct_by_name(cu, cookie->typename, false, &class_id)))
        return LSK__DELETE;

    fprintf(stderr, "struct: found a compilation unit that contains type `%s`\n", cookie->typename);

    if (!(class = tag__class(tag))) {
        fprintf(stderr, "struct: failed to get class, will continue loading more cu's\n");
        return LSK__DELETE;
    }

    // This macro enumerates through each member of the struct.
    type__for_each_data_member(&class->type, member) {
        struct tag *type = cu__type(cu, member->tag.type);

        fprintf(stderr, "struct: examining member %s, size %lu, tag %s\n",
                        class_member__name(member, cu),
                        member->byte_size,
                        dwarf_tag_name(type->tag));

        // Keep calling cu__type() until this is a base type.
        while (tag__is_typedef(type)) {
            fprintf(stderr, "struct: \t->typedef %s...\n", type__name(tag__type(type), cu));

            if (!(type = cu__type(cu, type->type))) {
                fprintf(stderr, "struct: error, failed to resolve typedef\n");
            }
        }

        // If this is a base type, we're done and can set this member.
        if (type->tag == DW_TAG_base_type) {
            char varname[128] = {0};

            fprintf(stderr, "struct: \t->base %s is a %s\n",
                            class_member__name(member, cu),
                            cu__string(cu, tag__base_type(type)->name));

            snprintf(varname, sizeof varname, "%s[\"%s\"]", cookie->assoc->name, class_member__name(member, cu));

            if (assign_array_element(varname,
                                     prefix_for_basetype(cu__string(cu, tag__base_type(type)->name)),
                                     AV_USEIND) == NULL) {
                fprintf(stderr, "struct: error setting %s!\n", varname);
            }
        }

        // TODO
        if (tag__is_struct(type) || tag__is_union(type) || tag__is_enumeration(type)) {
            fprintf(stderr, "struct: sorry, this member isn't supported yet, soon!\n");
        }

        if (type->tag == DW_TAG_array_type) {
            fprintf(stderr, "struct: sorry, arrays arent supported yet, soon!\n");
        }
    }

    // If we reach here, we were able to successfully export the struct.
    cookie->result = EXECUTION_SUCCESS;

    // No need to keep loading.
    return LSK__STOP_LOADING;
}

static int shared_library_callback(struct dl_phdr_info *info, size_t size, void *data)
{
    struct cookie *config = data;

    if (strlen(info->dlpi_name) == 0)
        return 0;

    fprintf(stderr, "struct: learned about object %s\n", info->dlpi_name);

    cus__load_file(config->cus, config->conf, info->dlpi_name);

    if (config->result == EXECUTION_SUCCESS) {
        fprintf(stderr, "struct: found a match for symbol after %s\n", info->dlpi_name);
        return 1;
    }

    return 0;
}

static int generate_standard_struct(WORD_LIST *list)
{
    HASH_TABLE *hashtable;
    struct conf_load conf_load = {
        .steal                  = create_array_stealer,
        .format_path            = NULL,
        .extra_dbg_info         = false,
        .fixup_silly_bitfields  = true,
        .get_addr_info          = false,
    };
    struct cookie config = {
        .result     = EXECUTION_FAILURE,
        .assoc      = NULL,
        .cus        = cus__new(),
        .conf       = &conf_load,
    };

    // Verify we have two parameters.
    if (!list || !list->next) {
        builtin_usage();
        return EXECUTION_FAILURE;
    }

    // Create the array used to save the result.
    config.assoc     = make_new_assoc_variable(list->next->word->word);
    config.typename  = list->word->word;
    conf_load.cookie = &config;
    hashtable        = assoc_create(1);

    // Throw away the default hash table.
    assoc_dispose((void *) config.assoc->value);

    // Replace it with our own hash table with just one bucket.
    config.assoc->value = (char *) hashtable;

    dwarves__init(0);

    dl_iterate_phdr(shared_library_callback, &config);

    if (config.result != EXECUTION_SUCCESS) {
        fprintf(stderr, "struct: struct not found or couldnt be parsed, may be incomplete\n");
    }

    // The members were appended in reverse, so try to fix.
    hashtable->bucket_array[0] = REVERSE_LIST(hashtable->bucket_array[0], BUCKET_CONTENTS *);

    fprintf(stderr, "struct: dumping members:\n\t%s=%s\n",
                    list->next->word->word,
                    assoc_to_string(config.assoc->value, ",", true));

    fprintf(stderr, "struct: run this comand to see:\n\tset | grep ^%s=\n", list->next->word->word);

    cus__delete(config.cus);
    dwarves__exit();
    return config.result;
}

static char *struct_usage[] = {
    "Generate a bash array for a standard structure.",
    NULL,
};

struct builtin __attribute__((visibility("default"))) struct_struct = {
    .name       = "struct",
    .function   = generate_standard_struct,
    .flags      = BUILTIN_ENABLED,
    .long_doc   = struct_usage,
    .short_doc  = "generate array for matching structure",
    .handle     = NULL,
};