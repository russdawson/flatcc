#include <stdio.h>
#include "emit_test_builder.h"
#include "support/hexdump.h"

int dbg_emitter(void *emit_context,
        const flatcc_iovec_t *iov, int iov_count,
        flatbuffers_soffset_t offset, size_t len)
{

    int i;
    printf("dbg: emit: iov_count: %d, offset: %ld, len: %ld\n",
            (int)iov_count, (long)offset, (long)len);

    for (i = 0; i < iov_count; ++i) {
        if (iov[i].iov_base == flatcc_builder_padding_base) {
            printf("dbg:  padding at: %ld, len: %ld\n",
                    (long)offset, (long)iov[i].iov_len);
        }
        if (iov[i].iov_base == 0) {
            printf("dbg:  null vector reserved at: %ld, len: %ld\n",
                    (long)offset, (long)iov[i].iov_len);
        }
        offset += iov[i].iov_len;
    }
    return 0;
}

int debug_test()
{
    flatcc_builder_t builder, *B;
    B = &builder;
    printf("dbg: output is generated by a custom emitter that doesn't actually build a buffer\n");
    flatcc_builder_custom_init(B, dbg_emitter, 0, 0, 0);
    /* We can create a null vector because we have a custom emitter. */
    main_create_as_root(B, 42, 1, flatbuffers_float_vec_create(B, 0, 10));
    flatcc_builder_clear(B);
    return 0;
}

/*
 * this assumes a very simple schema:
 * "table { time: long; device: ubyte; samples: [float]; }"
 */
int emit_test()
{
    /*
     * Note that there is some apparently unnecessary padding after 0x01
     * which is caused by the end of the buffer content excluding
     * vtables is forced to buffer alignment due to clustering and
     * because alignment happens before the buffer is fully generated.
     */
    unsigned char expect[] =
        "\x04\x00\x00\x00\xd4\xff\xff\xff\x2a\x00\x00\x00\x00\x00\x00\x00"
        "\x0c\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00\x00\x04\x00\x00\x00"
        "\x00\x00\x80\x3f\xcd\xcc\x8c\x3f\x9a\x99\x99\x3f\x66\x66\xa6\x3f"
        "\x0a\x00\x11\x00\x04\x00\x10\x00\x0c\x00";

    size_t size;
    uint8_t *buf;
    flatcc_emitter_t emitter, *E;
    flatcc_builder_t builder, *B;
    flatbuffers_float_vec_ref_t vref;
    float data[4] = { 1.0f, 1.1f, 1.2f, 1.3f };

    main_table_t mt;
    long time;

    B = &builder;

    flatcc_builder_init(B);

    /* Get the default emitter. */
    E = flatcc_builder_get_emit_context(B);


    vref = flatbuffers_float_vec_create(B, data, 4);
    //vref = 0;
    main_create_as_root(B, 42, 1, vref);



    /* We could also have used flatcc_builder API wrapper for this. */
    buf = flatcc_emitter_get_direct_buffer(E, &size);
    if (!buf) {
        return -1;
    }
    assert(size == flatcc_emitter_get_buffer_size(E));
    assert(size == flatcc_builder_get_buffer_size(B));
    flatcc_builder_clear(B);

    fprintf(stderr, "buffer size: %d\n", (int)size);
    hexdump("emit_test", buf, size, stderr);

    assert(size == 58);
    assert(sizeof(expect) - 1 == size);
    assert(0 == memcmp(buf, expect, size));

    mt = main_as_root(buf);
    time = main_time(mt);
    assert(time == 42);
    assert(main_device(mt) == 1);
    assert(flatbuffers_float_vec_len(main_samples(mt)) == 4);
    assert(flatbuffers_float_vec_at(main_samples(mt), 2) == 1.2f);

    return 0;
}

int main(int argc, char *argv[])
{
    int ret = 0;

    ret |= debug_test();
    ret |= emit_test();
    return ret;
}