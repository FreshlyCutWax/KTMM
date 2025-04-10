#!/usr/bin/python3

import time

limit = 2       # in gigabytes
size_x = 4      # number of kilobytes

block_size = (1024 * 1024) * size_x    # 1 kib x multiplier

allocated = []

total_blocks = 0
total_gib_size = 0

while total_gib_size <= limit:
    try:
        add_bytes = block_size * 1024

        total_blocks += add_bytes/block_size
        total_gib_size += total_blocks/(1024 * size_x)

        iteralloc = bytearray(add_bytes)
        allocated.append(iteralloc)

        print(f"Allocated: {total_blocks} blocks ({block_size} KiB), {total_gib_size} GiB")

        time.sleep(0.1)

    except MemoryError:
        print("\nMemory exceeded. Ouch!")

    except KeyboardInterrupt:
        print("\nMemory stress test ending...")
