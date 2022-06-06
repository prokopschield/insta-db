#include "../build/blake3-build/blake3_hash.h"
#include "../build/libdeflate/libdeflate.h"
#include <errno.h>
#include <fcntl.h>
#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

void blake3_hash(const void* data, const size_t len, uint8_t hash[BLAKE3_OUT_LEN])
{
    static blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, len);
    blake3_hasher_finalize(&hasher, hash, BLAKE3_OUT_LEN);
}

typedef struct db_entry {
    char magic[8]; // "DbEntry"
    uint8_t hash[32];
    uint32_t next;
    uint16_t size;
    uint16_t len;
    uint32_t val;
    uint8_t data[];
} db_entry_t;

#define ENTRY_MAX_SIZE_BYTES 4096
#define ENTRY_MAX_SIZE_SHIFT 12
// ENTRY_MAX_SIZE_BYTES = 1 << ENTRY_MAX_SIZE_SHIFT;
#define ENTRY_SIZE_SHIFT 6
#define INDEX_SIZE_SHIFT 4
#define DB_MAGIC_NUMBER "InstaDB"
#define DB_ENTRY_MAGIC_NUMBER "DbEntry"
#define DB_ENTRY_ARRAY_MAGIC_NUMBER "DbEntAr"

typedef struct db {
    char magic[8]; // "InstaDB"
    uint32_t size;
    uint32_t used;
    uint32_t buckets[]; // index = size >> INDEX_SIZE_SHIFT
} db_t;

#define QUERY_SIZE 32

typedef struct db_wrapper {
    char query[QUERY_SIZE];
    db_t* RW;
    db_t* RO;
    struct db_wrapper* copy;
    struct db_wrapper* rodb;
} db_wrapper_t;

extern inline db_entry_t* bucket_to_entry_f(db_t* db, uint32_t bucket)
{
    return (db_entry_t*)(((uint8_t*)(db)) + (((ptrdiff_t)(bucket)) << ENTRY_SIZE_SHIFT));
}

extern inline uint32_t entry_to_bucket_f(db_t* db, db_entry_t* entry)
{
    return (uint32_t)(((uint8_t*)db - (uint8_t*)entry) >> ENTRY_SIZE_SHIFT);
}

const char* error_texts[] = {
    { "napi_ok" },
    { "napi_invalid_arg" },
    { "napi_object_expected" },
    { "napi_string_expected" },
    { "napi_name_expected" },
    { "napi_function_expected" },
    { "napi_number_expected" },
    { "napi_boolean_expected" },
    { "napi_array_expected" },
    { "napi_generic_failure" },
    { "napi_pending_exception" },
    { "napi_cancelled" },
    { "napi_escape_called_twice" },
    { "napi_handle_scope_mismatch" },
    { "napi_callback_scope_mismatch" },
    { "napi_queue_full" },
    { "napi_closing" },
    { "napi_bigint_expected" },
    { "napi_date_expected" },
    { "napi_arraybuffer_expected" },
    { "napi_detachable_arraybuffer_expected" },
    { "napi_would_deadlock" }
};

napi_status status;
#define errcheck(txt)                        \
    if (status != napi_ok) {                 \
        napi_throw_error(env, nullptr, txt); \
        return ret;                          \
    }

#define errcheckd() errcheck(error_texts[status])

#define malloc_failed_check(var)                         \
    if ((var) == NULL) {                                 \
        napi_throw_error(env, nullptr, "Out of memory"); \
        return ret;                                      \
    }

db_wrapper_t* db_alloc_f(const char* filename, ssize_t size, bool readonly)
{
    db_wrapper_t* wrapper = (db_wrapper_t*)calloc(1, sizeof(db_wrapper_t));

    int fd = open(filename, readonly ? O_RDONLY : O_RDWR | O_CREAT, readonly ? 0400 : 0600);
    if (fd <= 0) {
        fprintf(stderr, "Could not open '%s': %s\n", filename, strerror(errno));
        return NULL;
    }

    struct stat s;
    if (fstat(fd, &s)) {
        fprintf(stderr, "Could not open '%s': %s\n", filename, strerror(errno));
        close(fd);
        return NULL;
    }

    if (!readonly && (s.st_size < size)) {
        if (ftruncate(fd, size)) {
            fprintf(stderr, "Could not truncate '%s': %s\n", filename, strerror(errno));
            close(fd);
            return NULL;
        }
        if (fstat(fd, &s)) {
            fprintf(stderr, "Could not open '%s': %s\n", filename, strerror(errno));
            close(fd);
            return NULL;
        }
    }

    wrapper->RO = (db_t*)mmap(NULL, s.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (wrapper->RO == MAP_FAILED) {
        fprintf(stderr, "Could not map '%s': %s\n", filename, strerror(errno));
        close(fd);
        return NULL;
    }

    if (!readonly) {
        wrapper->RW = (db_t*)mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (wrapper->RW == MAP_FAILED) {
            wrapper->RW = NULL;
        } else {
            if (wrapper->RW->size == 0) {
                wrapper->RW->size = ((size_t)size) >> ENTRY_SIZE_SHIFT;
            }

            uint32_t index_size = wrapper->RW->size >> INDEX_SIZE_SHIFT;
            uint32_t index_size_bytes = index_size * sizeof(uint32_t);
            uint32_t header_size_bytes = index_size_bytes + sizeof(db_t);
            uint32_t header_size = ((header_size_bytes - 1) >> ENTRY_SIZE_SHIFT) + 1;

            if (wrapper->RW->used < header_size) {
                strcpy(wrapper->RW->magic, DB_MAGIC_NUMBER);
                wrapper->RW->used = header_size;
            }
        }
    }

    return wrapper;
}

void db_alloc_sub_f(napi_env env, napi_value buf, db_wrapper_t* db, ssize_t size, bool readonly)
{
    size_t data_len = 0;
    const char* data = "0\0 0\0";

    status = napi_get_buffer_info(env, buf, (void**)&data, &data_len);
    if (status != napi_ok) {
        fprintf(stderr, "Invalid DB options.\n");
        return;
    }

    size_t num_entries = 0;
    sscanf(data, "%ld", &num_entries);

    for (size_t i = 0; (data_len > 0) && (i < num_entries); ++i) {
        while (--data_len && *(data++))
            ; // get next filename
        if ((data_len > 0) && (*data)) {
            db_wrapper_t* ndb = db_alloc_f(data, size, readonly);
            if (ndb != NULL) {
                if (readonly) {
                    ndb->rodb = db->rodb;
                    db->rodb = ndb;
                } else {
                    ndb->copy = db->copy;
                    db->copy = ndb;
                }
            }
        }
    }
}

void db_free_f(db_wrapper_t* db);
void db_free_f(db_wrapper_t* db)
{
    if (db == nullptr) {
        return;
    }

    if (db->rodb != nullptr) {
        db_free_f(db->rodb);
    }

    if (db->copy != nullptr) {
        db_free_f(db->copy);
    }

    if (db->RO != nullptr) {
        munmap(db->RO, db->RO->size << ENTRY_SIZE_SHIFT);
    }

    if (db->RW != nullptr) {
        munmap(db->RW, db->RW->size << ENTRY_SIZE_SHIFT);
    }

    free((void*)db);
}

void db_destroy_f(napi_env env, void* db_ptr, void* null_ptr)
{
    (void)null_ptr;
    db_free_f((db_wrapper_t*)db_ptr);
}

#define USED_COMPRESSION_LEVEL 12
struct libdeflate_compressor* compressor = nullptr;
struct libdeflate_decompressor* decompressor = nullptr;

napi_env genv = nullptr;

size_t compress_f(const void* in, size_t in_nbytes, void* out, size_t out_nbytes_avail)
{
    return libdeflate_zlib_compress(compressor, in, in_nbytes, out, out_nbytes_avail);
}

size_t decompress_f(const void* in, size_t in_nbytes, void* out, size_t out_nbytes_avail)
{
    size_t actual_out_nbytes_ret = 0;
    enum libdeflate_result result = libdeflate_zlib_decompress(decompressor, in, in_nbytes, out, out_nbytes_avail, &actual_out_nbytes_ret);

    if (result) {
        napi_throw_error(genv, nullptr, "Decompression error: data probably corrupted.");
    }

    return actual_out_nbytes_ret;
}

uint32_t db_find_chunk_by_hash_f(db_t* db, uint8_t hash[BLAKE3_OUT_LEN])
{
    uint32_t bucket_index = *((uint32_t*)(hash)) % (db->size >> INDEX_SIZE_SHIFT);
    uint32_t bucket = db->buckets[bucket_index];

    while (bucket) {
        if (bucket >= db->used) {
            napi_throw_error(genv, nullptr, "Hash table corrupted.");
            return 0;
        }

        db_entry_t* entry = bucket_to_entry_f(db, bucket);

        if (!memcmp(entry->hash, hash, BLAKE3_OUT_LEN)) {
            return bucket;
        } else {
            bucket = entry->next;
        }
    }

    return 0;
}

uint32_t dbw_insert_chunk_f(db_wrapper_t* db, uint8_t* data, uint16_t length)
{
    uint8_t hash[BLAKE3_OUT_LEN] = "";

    blake3_hash(data, length, (uint8_t*)&hash);

    uint32_t found = db_find_chunk_by_hash_f(db->RO, hash);

    if (found != 0) {
        return found;
    }

    uint32_t bucket_index = *((uint32_t*)(hash)) % (db->RO->size >> INDEX_SIZE_SHIFT);

    if (db->RO->used >= db->RO->size) {
        napi_throw_error(genv, nullptr, "Database is full!");
        return 0;
    }

    uint32_t bucket = db->RW->used;
    db_entry_t* entry = bucket_to_entry_f(db->RW, bucket);

    size_t available_space = ((db->RO->size - db->RO->used) << ENTRY_SIZE_SHIFT) - sizeof(db_entry_t);
    if (available_space < ENTRY_MAX_SIZE_BYTES) {
        napi_throw_error(genv, nullptr, "Database is too full!");
        return 0;
    }

    entry->size = compress_f(data, length, entry->data, available_space);
    if (entry->size == 0) {
        napi_throw_error(genv, nullptr, "Database is too full!");
        return 0;
    }

    entry->len = length;
    entry->val = 0; // NULL
    memcpy(entry->hash, hash, BLAKE3_OUT_LEN);
    memcpy(entry->magic, DB_ENTRY_MAGIC_NUMBER, sizeof(DB_ENTRY_MAGIC_NUMBER));

    size_t new_data_size_bytes = entry->size + sizeof(db_entry_t);
    size_t new_data_size = ((new_data_size_bytes - 1) >> ENTRY_SIZE_SHIFT) + 1;
    for (db_wrapper_t* dbc = db->copy; dbc != nullptr; dbc = dbc->copy) {
        db_entry_t* dest = bucket_to_entry_f(dbc->RW, bucket);
        memcpy(dest, entry, new_data_size_bytes);
    }
    for (db_wrapper_t* dbc = db; dbc != nullptr; dbc = dbc->copy) {
        db_entry_t* entry_c = bucket_to_entry_f(dbc->RW, bucket);
        entry_c->next = dbc->RW->buckets[bucket_index];
        dbc->RW->buckets[bucket_index] = bucket;
        dbc->RW->used += new_data_size;
    }

    return bucket;
}

typedef struct db_entry_array {
    uint32_t data_length;
    uint32_t array_length;
    uint32_t buckets[];
} db_entry_array_t;

uint32_t dbw_insert_buffer_f(db_wrapper_t* db, uint8_t* data, uint32_t length)
{
    if (length <= ENTRY_MAX_SIZE_BYTES) {
        return dbw_insert_chunk_f(db, data, length);
    } else {
        uint32_t arr_len = ((length - 1) >> ENTRY_MAX_SIZE_SHIFT) + 1;
        uint32_t arr_size_bytes = sizeof(db_entry_array_t) + sizeof(uint32_t) * arr_len;
        db_entry_array_t* array = (db_entry_array_t*)calloc(1, arr_size_bytes);
        array->data_length = length;
        array->array_length = arr_len;
        for (uint32_t i = 0; i < arr_len; ++i) {
            uint32_t chunk_len = length - (i << ENTRY_MAX_SIZE_SHIFT);
            if (chunk_len > ENTRY_MAX_SIZE_BYTES) {
                chunk_len = ENTRY_MAX_SIZE_BYTES;
            }
            if ((array->buckets[i] = dbw_insert_chunk_f(db, data + (i << ENTRY_MAX_SIZE_SHIFT), chunk_len)) == 0) {
                // something went wrong, and we probably already threw
                free((void*)array);
                return 0;
            }
        }
        uint32_t arr_bucket = dbw_insert_chunk_f(db, (uint8_t*)array, arr_size_bytes);
        free((void*)array);
        if (arr_bucket == 0) {
            // something went wrong, and we probably already threw
            return 0;
        }
        for (db_wrapper_t* dbc = db; dbc != nullptr; dbc = dbc->copy) {
            db_entry_t* entry = bucket_to_entry_f(dbc->RW, arr_bucket);
            strcpy(entry->magic, DB_ENTRY_ARRAY_MAGIC_NUMBER);
        }
        return arr_bucket;
    }
}

napi_value dbm_store_f(napi_env env, napi_callback_info info)
{
    napi_value ret;
    napi_get_undefined(env, &ret);
    errcheckd();

    genv = env;

    napi_value thisarg;
    napi_value argv[1];
    size_t argc = 1;

    status = napi_get_cb_info(env, info, &argc, argv, &thisarg, nullptr);
    errcheck("Could not read function arguments");

    napi_value query;
    napi_get_named_property(env, thisarg, "query", &query);
    errcheck("store() called with invalid thisArg");

    bool query_is_buffer;
    status = napi_is_buffer(env, query, &query_is_buffer);
    errcheckd();
    if (query_is_buffer == false) {
        napi_throw_error(env, nullptr, "db.query is invalid.");
        return ret;
    }

    db_wrapper_t* db = nullptr;
    size_t query_length = 0;
    napi_get_buffer_info(env, query, (void**)&db, &query_length);
    errcheck("Could not get db_wrapper from query");
    if (db == nullptr) {
        napi_throw_error(env, nullptr, "db.query has null pointer");
        return ret;
    }

    uint8_t* buffer_data = nullptr;
    size_t buffer_length = 0;

    napi_get_buffer_info(env, argv[0], (void**)&buffer_data, &buffer_length);
    errcheck("db.store(): Could not get data from buffer.");

    if (buffer_data == nullptr) {
        napi_throw_error(env, nullptr, "db.store(): buffer_data is null");
        return ret;
    }

    uint32_t bucket = 0;

    if (buffer_length != 0) {
        bucket = dbw_insert_buffer_f(db, buffer_data, buffer_length);
    }

    if (bucket != 0) {
        db_entry_t* entry = bucket_to_entry_f(db->RO, bucket);
        char* strhash = (char*)malloc(BLAKE3_OUT_LEN << 1);
        malloc_failed_check(strhash);
        for (int i = 0; i < BLAKE3_OUT_LEN; ++i) {
            strhash[(i << 1) + 0] = "0123456789abcdef"[entry->hash[i] >> 4];
            strhash[(i << 1) + 1] = "0123456789abcdef"[entry->hash[i] & 15];
        }
        napi_create_string_latin1(env, strhash, BLAKE3_OUT_LEN << 1, &ret);
        free((void*)strhash);
        errcheckd();
    }

    return ret;
}

napi_value dbm_fetch_f(napi_env env, napi_callback_info info)
{
    napi_value ret;
    napi_get_undefined(env, &ret);
    errcheckd();

    genv = env;

    napi_value thisarg;
    napi_value argv[3];
    size_t argc = 3;

    db_wrapper_t* db = nullptr;

    status = napi_get_cb_info(env, info, &argc, argv, &thisarg, (void**)&db);
    errcheckd();

    if (db == nullptr) {
        napi_throw_error(env, nullptr, "Invalid callback.");
        return ret;
    }

    uint8_t hashstr[BLAKE3_OUT_LEN * 2 + 1] = "";
    size_t hashstr_len = 0;

    status = napi_get_value_string_latin1(env, argv[0], (char*)hashstr, sizeof(hashstr), &hashstr_len);
    errcheck("Hash must be a string.");

    bool do_decompress = false;
    status = napi_get_value_bool(env, argv[1], &do_decompress);
    // ignore invalid type, default to false

    bool do_dereference = false;
    status = napi_get_value_bool(env, argv[2], &do_dereference);
    // ignore invalid type, default to false

    for (int i = 0; i < 32; ++i) {
        uint8_t c = hashstr[(i << 1) + 0];
        if (c >= 'a') {
            c -= 'a' - 10;
        } else if (c >= 'A') {
            c -= 'A' - 10;
        } else if (c >= '0') {
            c -= '0';
        }
        uint8_t d = hashstr[(i << 1) + 1];
        if (d >= 'a') {
            d -= 'a' - 10;
        } else if (d >= 'A') {
            d -= 'A' - 10;
        } else if (d >= '0') {
            d -= '0';
        }
        hashstr[i] = (c << 4) | d;
    }

    for (db_wrapper_t* dbc = db; dbc != nullptr; dbc = dbc->rodb) {
        uint32_t bucket_index = ((*((uint32_t*)hashstr)) % (dbc->RO->size >> INDEX_SIZE_SHIFT));
        uint32_t bucket = dbc->RO->buckets[bucket_index];

        while (bucket != 0) {

            db_entry_t* entry = bucket_to_entry_f(dbc->RO, bucket);

            if (!memcmp(entry->hash, hashstr, BLAKE3_OUT_LEN)) {
                if (do_dereference) {
                    if (entry->val) {
                        bucket = entry->val;
                        entry = bucket_to_entry_f(dbc->RO, entry->val);
                    } else {
                        break;
                    }
                }
                if (!strcmp(entry->magic, DB_ENTRY_ARRAY_MAGIC_NUMBER)) {
                    db_entry_array_t* array = (db_entry_array_t*)calloc(1, entry->len);
                    malloc_failed_check(array);
                    if (decompress_f(entry->data, entry->size, (uint8_t*)array, entry->len) < sizeof(db_entry_array_t)) {
                        napi_throw_error(env, nullptr, "Invalid entry array.");
                        free((void*)array);
                        return ret;
                    }

                    uint8_t* decompressed = nullptr;
                    uint32_t len_already_read = 0;
                    status = napi_create_buffer(env, array->data_length, (void**)&decompressed, &ret);
                    if ((status != napi_ok) || (decompressed == nullptr)) {
                        free((void*)array);
                        napi_throw_error(env, nullptr, "Cannot allocate buffer.");
                        return ret;
                    }
                    for (uint32_t i = 0; i < array->array_length; ++i) {
                        db_entry_t* e = bucket_to_entry_f(dbc->RO, array->buckets[i]);
                        if (array->data_length < (len_already_read + e->len)) {
                            napi_throw_error(env, nullptr, "Invalid entry array.");
                            free((void*)array);
                            return ret;
                        }
                        len_already_read += decompress_f(e->data, e->size, decompressed + len_already_read, array->data_length - len_already_read);
                    }

                    if (!do_decompress) {
                        void* compressed = malloc(len_already_read);
                        uint32_t compressed_len = compress_f(decompressed, array->data_length, compressed, len_already_read);
                        void* compressed_buffer = nullptr;

                        status = napi_create_buffer_copy(env, compressed_len, (void*)compressed, &compressed_buffer, &ret);
                        free(compressed);
                    }

                    free((void*)array);
                } else {
                    if (do_decompress) {
                        uint8_t* decompressed = nullptr;
                        status = napi_create_buffer(env, entry->len, (void**)&decompressed, &ret);
                        malloc_failed_check(decompressed);
                        errcheckd();
                        decompress_f(entry->data, entry->size, decompressed, entry->len);
                    } else {
                        status = napi_create_external_buffer(env, entry->size, entry->data, nullptr, nullptr, &ret);
                        errcheckd();
                    }
                }
                return ret;
            } else {
                bucket = entry->next;
            }
        }
    }

    return ret;
}

napi_value dbm_associate_f(napi_env env, napi_callback_info info)
{
    napi_value ret;

    status = napi_get_boolean(env, true, &ret);

    napi_value thisarg;
    napi_value argv[2];
    size_t argc = 2;

    db_wrapper_t* db = nullptr;

    status = napi_get_cb_info(env, info, &argc, argv, &thisarg, (void**)&db);
    errcheckd();

    if (db == nullptr) {
        napi_throw_error(env, nullptr, "Invalid callback.");
        return ret;
    }

    uint8_t* key_data = nullptr;
    size_t key_len = 0;

    status = napi_get_buffer_info(env, argv[0], (void**)&key_data, &key_len);
    errcheckd();

    uint32_t key = dbw_insert_buffer_f(db, key_data, key_len);

    if (key == 0) {
        napi_get_boolean(env, false, &ret);
        return ret;
    }

    uint8_t* val_data = nullptr;
    size_t val_len = 0;

    status = napi_get_buffer_info(env, argv[1], (void**)&val_data, &val_len);
    errcheckd();

    uint32_t val = val_data ? val_len ? dbw_insert_buffer_f(db, val_data, val_len) : 0 : 0;

    for (db_wrapper_t* dbc = db; dbc != nullptr; dbc = dbc->copy) {
        db_entry_t* entry = bucket_to_entry_f(dbc->RW, key);

        entry->val = val;
    }

    return ret;
}

napi_value db_init_f(napi_env env, napi_callback_info info)
{
    napi_value ret;

    size_t argc = 1;
    napi_value argv[1];
    napi_value thisarg;

    status = napi_get_cb_info(env, info, &argc, argv, &thisarg, nullptr);
    errcheckd();

    if (!argc) {
        napi_throw_error(env, nullptr, "Database constructor must be passed a valid options object.");
    }

    napi_value storage_file_jsstr;
    status = napi_get_named_property(env, argv[0], "storage_file", &storage_file_jsstr);
    errcheckd();

    char storage_file_name[256] = "";
    size_t storage_file_len = 0;
    status = napi_get_value_string_utf8(env, storage_file_jsstr, storage_file_name, sizeof(storage_file_name), &storage_file_len);
    errcheck("Database options must include a field named 'storage_file'");

    napi_value storage_file_size_jsnum;
    status = napi_get_named_property(env, argv[0], "size", &storage_file_size_jsnum);
    errcheckd();

    int64_t storage_file_size = 0;
    status = napi_get_value_int64(env, storage_file_size_jsnum, &storage_file_size);
    errcheck("Database options must include a field named 'size'");

    db_wrapper_t* db = db_alloc_f(storage_file_name, (ssize_t)storage_file_size, false);

    napi_value tmp;
    status = napi_get_named_property(env, argv[0], "__copies", &tmp);
    errcheckd();

    db_alloc_sub_f(env, tmp, db, (ssize_t)storage_file_size, false);

    status = napi_get_named_property(env, argv[0], "__rocopies", &tmp);
    errcheckd();

    db_alloc_sub_f(env, tmp, db, (ssize_t)storage_file_size, true);

    status = napi_create_object(env, &ret);
    errcheckd();

    napi_value buffer;
    status = napi_create_external_buffer(env, QUERY_SIZE, db->query, db_destroy_f, nullptr, &buffer);
    errcheckd();

    status = napi_set_named_property(env, ret, "query", buffer);
    errcheckd();

    napi_value store_f;
    status = napi_create_function(env, "store", NAPI_AUTO_LENGTH, dbm_store_f, nullptr, &store_f);
    errcheckd();

    status = napi_set_named_property(env, ret, "store", store_f);
    errcheckd();

    napi_value fetch_f;
    status = napi_create_function(env, "fetch", NAPI_AUTO_LENGTH, dbm_fetch_f, (void*)db, &fetch_f);
    errcheckd();

    status = napi_set_named_property(env, ret, "fetch", fetch_f);
    errcheckd();

    napi_value associate_f;
    status = napi_create_function(env, "associate", NAPI_AUTO_LENGTH, dbm_associate_f, (void*)db, &associate_f);
    errcheckd();

    status = napi_set_named_property(env, ret, "associate", associate_f);
    errcheckd();

    return ret;
}

napi_value init(napi_env env, napi_value ret)
{
    napi_value db_init;

    status = napi_create_function(env, "db_init", sizeof("db_init"), db_init_f, nullptr, &db_init);
    errcheckd();

    status = napi_set_named_property(env, ret, "db_init", db_init);
    errcheckd();

    compressor = libdeflate_alloc_compressor(USED_COMPRESSION_LEVEL);
    if (compressor == NULL) {
        napi_throw_error(env, nullptr, "Could not allocate compressor: malloc() failed");
    }

    decompressor = libdeflate_alloc_decompressor();
    if (decompressor == NULL) {
        napi_throw_error(env, nullptr, "Could not allocate decompressor: malloc() failed");
    }

    return ret;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init);
