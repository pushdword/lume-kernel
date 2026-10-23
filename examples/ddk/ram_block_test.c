#include <lume/block.h>
#include <lume/abi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
extern struct lume_block lume_example_ram_block;
int lume_example_ram_block_start(void);
int lume_example_ram_block_stop(void);

int main(void)
{
    unsigned char input[1024], output[1024];
    for (unsigned i = 0; i < sizeof(input); ++i)
        input[i] = (unsigned char)(i * 37 + i / 17);
    assert(!lume_example_ram_block_start());
    assert(!lume_block_write(&lume_example_ram_block, 6, 2, input));
    assert(!lume_block_read(&lume_example_ram_block, 6, 2, output));
    assert(!memcmp(input, output, sizeof(input)));
    assert(lume_block_write(&lume_example_ram_block, 7, 2, input) == -LUME_EINVAL);
    assert(!lume_example_ram_block_stop());
    assert(lume_block_read(&lume_example_ram_block, 0, 1, output) == -LUME_EIO);
    assert(!lume_device_count());
    puts("RAM_BLOCK_DRIVER_PASS");
    return 0;
}
