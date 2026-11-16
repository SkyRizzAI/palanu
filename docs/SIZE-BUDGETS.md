# PalanuOS size and measurement budgets

This is the one place where measured numbers are kept. Every run of `./pal hwtest` (or `bun forge/tools/hwtest.ts`)
appends a new dated section below; old sections are never changed. Targets come from
`forge/tools/perf-targets.json`.

Simulator numbers do not represent the board. Each section names the board or simulator used.



## 2026-11-13 03:39 UTC · idle (commit acd07bd)

- Board: hhv2 (ESP32-S31), ver 0.1, build acd07bd-dirty
- Link: serial /dev/cu.usbserial-10
- Scenario: idle, no radios
- Status: measured

| metric | value | target |
|---|---|---|
| Key to screen, system and app screens | p95 19.1 ms | p95 ≤ 30 ms |
| Full-screen send | 1.4 ms | ≤ 20 ms |
| Boot until home is shown | 1957 ms | ≤ 2.5 s (measured first in T0.4, then fixed) |
| CPU at idle on home | 0 % | ≤ 2 % |
| Opening a built-in app | n/a | ≤ 50 ms |
| Opening a 100 KB .papp app | n/a | ≤ 150 ms including file read |
| Terminal output to screen | n/a | ≥ 50 KB/s without input stalls |
| Palanu Link over Wi-Fi | n/a | ≥ 200 KB/s file write |
| Free internal RAM, everything on | 138 KB | ≥ 86 KB, blok terbesar ≥ 32 KB |
| Key p50 | 18.5 ms | - |
| Key max | 20.2 ms | - |
| Internal RAM, largest block | 68 KB | - |
| Internal RAM, minimum free | 98 KB | - |
| DMA RAM free | 106 KB | - |
| PSRAM free | 16256 KB | - |
| Boot total | n/a | - |
| Draw | 1.4 ms | - |
| FPS | 0 fps | - |

<details><summary>Raw device output</summary>

**ctl INFO**

```text
{
  "os": "PalanuOS",
  "ver": "0.1",
  "board": "hhv2",
  "chip": "ESP32-S31",
  "uptime": 10,
  "mem_free": 162479,
  "psram_free": 16653932,
  "slot": "ota_0",
  "build": "acd07bd-dirty",
  "ota": true
}
```

**boot**

```text
start             0 ms  (+0)
kernel            0 ms  (+0)
storage        1678 ms  (+1678)
display        1950 ms  (+272)
desktop        1957 ms  (+7)
```

**mem**

```text
memory     total   free    min  block
internal    364K   138K    98K    68K
dma         332K   106K    67K    68K
external  16384K 16256K 16255K 16128K
floor 32K kept free; dma is part of internal

owner        int+dma  external    stack  allocs
kernel             0     16448        0       9
net                0        24     3072       1
board          25600         0     3072       1
ramfs              0       452        0       7
posixfs            0       772        0       4
papp               0       368        0       4
display            0     76800        0       1
desktop            0         0     6144       0
console            0      2516     8192       1
link               0      4723    11264       4
sh                 0      2875    12288       2
sh                 0      4824    12288       5
mem                0        12     8192       1
```

**free**

```text
memory      total    free  largest min free
internal     364K    138K      68K      98K
external   16384K  16256K   16128K   16255K
floor         32K kept free for radio and DMA

owner         int     ext  stack   peak allocs fail
kernel       0.0K   16.1K   0.0K  16.1K     9    0
net          0.0K    0.0K   3.0K   0.0K     1    0
board       25.0K    0.0K   3.0K  25.0K     1    0
ramfs        0.0K    0.4K   0.0K   0.4K     7    0
posixfs      0.0K    0.8K   0.0K   0.8K     4    0
papp         0.0K    0.4K   0.0K   0.9K     4    0
display      0.0K   75.0K   0.0K  75.0K     1    0
desktop      0.0K    0.0K   6.0K   0.0K     0    0
console      0.0K    2.5K   8.0K   2.5K     1    0
link         0.0K    4.6K  11.0K   4.6K     4    0
sh           0.0K    2.8K  12.0K   5.1K     2    0
sh           0.0K    4.7K  12.0K   5.1K     6    0
free         0.0K    0.0K   8.0K   0.0K     1    0
```

**top**

```text
top  up 00:00:11  cpu0   0% cpu1   0% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
```

**perf**

```text
ms             last     mean      max
draw           1.44    10.94    20.45
flush          1.43    10.93    20.43
fps          0  (1 s: draw 20.45, flush 20.43)
pixels/flush 40960
flushes      2 since reset
overlay      off
```

**perf reset**

```text
perf counters reset
```

**perf keys**

```text
keys recorded 100 (last 100 kept)
p50 18.46 ms
p95 19.05 ms
max 20.20 ms
```

</details>

## 2026-11-13 03:59 UTC · idle

- Board: hhv2 (ESP32-S31), ver 0.1, build 0b6c316-dirty
- Link: serial /dev/cu.usbserial-10
- Scenario: idle, no radios
- Status: measured

| metric | value | target |
|---|---|---|
| Key to screen, system and app screens | p95 19.4 ms | p95 ≤ 30 ms |
| Full-screen send | 1.4 ms | ≤ 20 ms |
| Boot until home is shown | 1959 ms | ≤ 2.5 s (measured first in T0.4, then fixed) |
| CPU at idle on home | 2.8 % | ≤ 2 % |
| Opening a built-in app | n/a | ≤ 50 ms |
| Opening a 100 KB .papp app | n/a | ≤ 150 ms including file read |
| Terminal output to screen | n/a | ≥ 50 KB/s without input stalls |
| Palanu Link over Wi-Fi | n/a | ≥ 200 KB/s file write |
| Free internal RAM, everything on | 138 KB | ≥ 86 KB, blok terbesar ≥ 32 KB |
| Key p50 | 18.5 ms | - |
| Key max | 21.1 ms | - |
| Internal RAM, largest block | 61 KB | - |
| Internal RAM, minimum free | 98 KB | - |
| DMA RAM free | 106 KB | - |
| PSRAM free | 16256 KB | - |
| Boot total | n/a | - |
| Draw | 1.4 ms | - |
| FPS | 0 fps | - |

<details><summary>Raw device output</summary>

**ctl INFO**

```text
{
  "os": "PalanuOS",
  "ver": "0.1",
  "board": "hhv2",
  "chip": "ESP32-S31",
  "uptime": 6,
  "mem_free": 162451,
  "psram_free": 16653932,
  "slot": "ota_0",
  "build": "0b6c316-dirty",
  "ota": true
}
```

**boot**

```text
start             0 ms  (+0)
kernel            0 ms  (+0)
storage        1679 ms  (+1679)
display        1953 ms  (+274)
desktop        1959 ms  (+6)
```

**mem**

```text
memory     total   free    min  block
internal    364K   138K    98K    61K
dma         332K   106K    67K    61K
external  16384K 16256K 16255K 16128K
floor 32K kept free; dma is part of internal

owner        int+dma  external    stack  allocs
kernel             0     16448        0       9
net                0        24     3072       1
board          25600         0     3072       1
ramfs              0       452        0       7
posixfs            0       772        0       4
papp               0       368        0       4
display            0     76800        0       1
desktop            0         0     6144       0
console            0      2516     8192       1
link               0      4723    11264       4
sh                 0      2875    12288       2
sh                 0      4824    12288       5
mem                0        12     8192       1
```

**free**

```text
memory      total    free  largest min free
internal     364K    138K      61K      98K
external   16384K  16256K   16128K   16255K
floor         32K kept free for radio and DMA

owner         int     ext  stack   peak allocs fail
kernel       0.0K   16.1K   0.0K  16.1K     9    0
net          0.0K    0.0K   3.0K   0.0K     1    0
board       25.0K    0.0K   3.0K  25.0K     1    0
ramfs        0.0K    0.4K   0.0K   0.4K     7    0
posixfs      0.0K    0.8K   0.0K   0.8K     4    0
papp         0.0K    0.4K   0.0K   0.9K     4    0
display      0.0K   75.0K   0.0K  75.0K     1    0
desktop      0.0K    0.0K   6.0K   0.0K     0    0
console      0.0K    2.5K   8.0K   2.5K     1    0
link         0.0K    4.6K  11.0K   4.6K     4    0
sh           0.0K    2.8K  12.0K   5.1K     2    0
sh           0.0K    4.7K  12.0K   5.1K     6    0
free         0.0K    0.0K   8.0K   0.0K     1    0
```

**top**

```text
palanu:~$ epalanu:~$ ecpalanu:~$ echpalanu:~$ echopalanu:~$ echo palanu:~$ echo _palanu:~$ echo __palanu:~$ echo __Hpalanu:~$ echo __HWpalanu:~$ echo __HWBpalanu:~$ echo __HWB4palanu:~$ echo __HWB4_palanu:~$ echo __HWB4__palanu:~$ echo __HWB4__;palanu:~$ echo __HWB4__; palanu:~$ echo __HWB4__; tpalanu:~$ echo __HWB4__; topalanu:~$ echo __HWB4__; toppalanu:~$ echo __HWB4__; top;palanu:~$ echo __HWB4__; top; palanu:~$ echo __HWB4__; top; epalanu:~$ echo __HWB4__; top; ecpalanu:~$ echo __HWB4__; top; echpalanu:~$ echo __HWB4__; top; echopalanu:~$ echo __HWB4__; top; echo palanu:~$ echo __HWB4__; top; echo _palanu:~$ echo __HWB4__; top; echo __palanu:~$ echo __HWB4__; top; echo __Hpalanu:~$ echo __HWB4__; top; echo __HWpalanu:~$ echo __HWB4__; top; echo __HWEpalanu:~$ echo __HWB4__; top; echo __HWE4palanu:~$ echo __HWB4__; top; echo __HWE4_palanu:~$ echo __HWB4__; top; echo __HWE4__
__HWB4__
 top  up 00:00:07  cpu0   0% cpu1   0% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
q top  up 00:00:08  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:09  cpu0   5% cpu1   0% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:10  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:11  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:12  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:13  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:14  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:15  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:16  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:17  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:18  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:19  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:20  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
 top  up 00:00:21  cpu0   5% cpu1   1% 
int 138K/364K free  ext 16256K/16384K free  tasks 9
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   2 sh            4K    12K
  13 top           0K     8K
[no end marker after 15000 ms]
```

**perf**

```text
ms             last     mean      max
draw           1.45    10.10    18.76
flush          1.44    10.09    18.75
fps          0  (1 s: draw 0.00, flush 0.00)
pixels/flush 40960
flushes      2 since reset
overlay      off
```

**perf reset**

```text
perf counters reset
```

**perf keys**

```text
keys recorded 100 (last 100 kept)
p50 18.48 ms
p95 19.37 ms
max 21.07 ms
```

</details>

## 2026-11-13 05:06 UTC · idle

- Board: hhv2 (ESP32-S31), ver 0.1, build 6e01b2e-dirty
- Link: serial /dev/cu.usbserial-10
- Scenario: idle, no radios
- Status: measured

| metric | value | target |
|---|---|---|
| Key to screen, system and app screens | p95 18.9 ms | p95 ≤ 30 ms |
| Full-screen send | 1.4 ms | ≤ 20 ms |
| Boot until home is shown | 1820 ms | ≤ 2.5 s (measured first in T0.4, then fixed) |
| CPU at idle on home | 1.6 % | ≤ 2 % |
| Opening a built-in app | n/a | ≤ 50 ms |
| Opening a 100 KB .papp app | n/a | ≤ 150 ms including file read |
| Terminal output to screen | n/a | ≥ 50 KB/s without input stalls |
| Palanu Link over Wi-Fi | n/a | ≥ 200 KB/s file write |
| Free internal RAM, everything on | 116 KB | ≥ 86 KB, blok terbesar ≥ 32 KB |
| Key p50 | 18.0 ms | - |
| Key max | 21.4 ms | - |
| Internal RAM, largest block | 48 KB | - |
| Internal RAM, minimum free | 81 KB | - |
| DMA RAM free | 85 KB | - |
| PSRAM free | 16255 KB | - |
| Boot total | n/a | - |
| Draw | 1.4 ms | - |
| FPS | 0 fps | - |

<details><summary>Raw device output</summary>

**ctl INFO**

```text
{
  "os": "PalanuOS",
  "ver": "0.1",
  "board": "hhv2",
  "chip": "ESP32-S31",
  "uptime": 39,
  "mem_free": 136611,
  "psram_free": 16653116,
  "slot": "ota_0",
  "build": "6e01b2e-dirty",
  "ota": true
}
```

**boot**

```text
start             0 ms  (+0)
kernel            0 ms  (+0)
storage        1679 ms  (+1679)
display        1814 ms  (+135)
desktop        1820 ms  (+6)
```

**mem**

```text
memory     total   free    min  block
internal    357K   116K    81K    48K
dma         325K    85K    50K    48K
external  16384K 16255K 16163K 16128K
floor 32K kept free; dma is part of internal

owner        int+dma  external    stack  allocs      quota
kernel             0     16712        0      11          -
net                0        28     3072       1          -
board          25600         0     3072       1          -
ramfs              0       452        0       7          -
posixfs            0       772        0       4          -
papp               0       460        0       5          -
display            0     76800        0       1          -
desktop            0         0     6144       0          -
console            0      2524    13312       1          -
link               0      4731    11264       4          -
wasm               0        20        0       1          -
sh                 0      2875    12288       2  2048K/64K
sh                 0      4824    12288       5  2048K/64K
mem                0        12     8192       1  2048K/64K
```

**free**

```text
memory      total    free  largest min free
internal     357K    116K      48K      81K
external   16384K  16255K   16128K   16163K
floor         32K kept free for radio and DMA

owner         int     ext  stack   peak allocs fail      quota
kernel       0.0K   16.3K   0.0K  16.4K    11    0          -
net          0.0K    0.0K   3.0K   0.0K     1    0          -
board       25.0K    0.0K   3.0K  25.0K     1    0          -
ramfs        0.0K    0.4K   0.0K   0.4K     7    0          -
posixfs      0.0K    0.8K   0.0K   0.8K     4    0          -
papp         0.0K    0.4K   0.0K   0.9K     5    0          -
display      0.0K   75.0K   0.0K  75.0K     1    0          -
desktop      0.0K    0.0K   6.0K   0.0K     0    0          -
console      0.0K    2.5K  13.0K   2.5K     1    0          -
link         0.0K    4.6K  11.0K   4.6K     4    0          -
wasm         0.0K    0.0K   0.0K   0.0K     1    0          -
sh           0.0K    2.8K  12.0K   5.1K     2    0  2048K/64K
sh           0.0K    4.7K  12.0K   5.1K     6    0  2048K/64K
free         0.0K    0.0K   8.0K   0.0K     1    0  2048K/64K
ps           0.0K    0.0K   0.0K   8.3K     0    0  2048K/64K
```

**top**

```text
palanu:~$ epalanu:~$ ecpalanu:~$ echpalanu:~$ echopalanu:~$ echo palanu:~$ echo _palanu:~$ echo __palanu:~$ echo __Hpalanu:~$ echo __HWpalanu:~$ echo __HWBpalanu:~$ echo __HWB4palanu:~$ echo __HWB4_palanu:~$ echo __HWB4__palanu:~$ echo __HWB4__;palanu:~$ echo __HWB4__; palanu:~$ echo __HWB4__; tpalanu:~$ echo __HWB4__; topalanu:~$ echo __HWB4__; toppalanu:~$ echo __HWB4__; top;palanu:~$ echo __HWB4__; top; palanu:~$ echo __HWB4__; top; epalanu:~$ echo __HWB4__; top; ecpalanu:~$ echo __HWB4__; top; echpalanu:~$ echo __HWB4__; top; echopalanu:~$ echo __HWB4__; top; echo palanu:~$ echo __HWB4__; top; echo _palanu:~$ echo __HWB4__; top; echo __palanu:~$ echo __HWB4__; top; echo __Hpalanu:~$ echo __HWB4__; top; echo __HWpalanu:~$ echo __HWB4__; top; echo __HWEpalanu:~$ echo __HWB4__; top; echo __HWE4palanu:~$ echo __HWB4__; top; echo __HWE4_palanu:~$ echo __HWB4__; top; echo __HWE4__
__HWB4__
 top  up 00:00:40  cpu0   0% cpu1   0% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
q top  up 00:00:41  cpu0   3% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:42  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:43  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:44  cpu0   3% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:45  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:46  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:47  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:48  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:49  cpu0   3% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:50  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:51  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:52  cpu0   3% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:53  cpu0   2% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
 top  up 00:00:54  cpu0   3% cpu1   1% 
int 125K/357K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   7 sh            4K    12K
  18 top           8K     8K
[no end marker after 15000 ms]
```

**perf**

```text
ms             last     mean      max
draw           1.44    10.12    18.80
flush          1.43    10.11    18.80
fps          0  (1 s: draw 0.00, flush 0.00)
pixels/flush 40960
flushes      2 since reset
overlay      off
```

**perf reset**

```text
perf counters reset
```

**perf keys**

```text
keys recorded 100 (last 100 kept)
p50 18.01 ms
p95 18.91 ms
max 21.35 ms
```

</details>

## 2026-11-13 05:55 UTC · idle

- Board: hhv2 (ESP32-S31), ver 0.1, build 409bc6f-dirty
- Link: serial /dev/cu.usbserial-10
- Scenario: idle, no radios
- Status: measured

| metric | value | target |
|---|---|---|
| Key to screen, system and app screens | p95 19.2 ms | p95 ≤ 30 ms |
| Full-screen send | 1.4 ms | ≤ 20 ms |
| Boot until home is shown | 1916 ms | ≤ 2.5 s (measured first in T0.4, then fixed) |
| CPU at idle on home | 0 % | ≤ 2 % |
| Opening a built-in app | n/a | ≤ 50 ms |
| Opening a 100 KB .papp app | n/a | ≤ 150 ms including file read |
| Terminal output to screen | n/a | ≥ 50 KB/s without input stalls |
| Palanu Link over Wi-Fi | n/a | ≥ 200 KB/s file write |
| Free internal RAM, everything on | 108 KB | ≥ 86 KB, blok terbesar ≥ 32 KB |
| Key p50 | 18.2 ms | - |
| Key max | 20.4 ms | - |
| Internal RAM, largest block | 41 KB | - |
| Internal RAM, minimum free | 69 KB | - |
| DMA RAM free | 77 KB | - |
| PSRAM free | 16255 KB | - |
| Boot total | n/a | - |
| Draw | 1.4 ms | - |
| FPS | 0 fps | - |

<details><summary>Raw device output</summary>

**ctl INFO**

```text
{
  "os": "PalanuOS",
  "ver": "0.1",
  "board": "hhv2",
  "chip": "ESP32-S31",
  "uptime": 25,
  "mem_free": 128563,
  "psram_free": 16653188,
  "slot": "ota_0",
  "build": "409bc6f-dirty",
  "ota": true
}
```

**boot**

```text
start             0 ms  (+0)
kernel            1 ms  (+1)
storage        1679 ms  (+1678)
display        1909 ms  (+230)
desktop        1916 ms  (+7)
```

**mem**

```text
memory     total   free    min  block
internal    350K   108K    69K    41K
dma         318K    77K    38K    41K
external  16384K 16255K 16240K 16128K
floor 32K kept free; dma is part of internal

owner        int+dma  external    stack  allocs      quota
kernel             0     16712        0      11          -
net                0        28     3072       1          -
board          25600         0     3072       1          -
ramfs              0       452        0       7          -
posixfs            0       772        0       4          -
papp               0       460        0       5          -
display            0     76800        0       1          -
desktop            0         0     6144       0          -
console            0      2524    13312       1          -
link               0      4731    11264       4          -
sh                 0      2875    12288       2  2048K/64K
sh                 0      4824    12288       5  2048K/64K
mem                0        12     8192       1  2048K/64K
```

**free**

```text
memory      total    free  largest min free
internal     350K    117K      49K      69K
external   16384K  16255K   16128K   16240K
floor         32K kept free for radio and DMA

owner         int     ext  stack   peak allocs fail      quota
kernel       0.0K   16.3K   0.0K  16.4K    11    0          -
net          0.0K    0.0K   3.0K   0.0K     1    0          -
board       25.0K    0.0K   3.0K  25.0K     1    0          -
ramfs        0.0K    0.4K   0.0K   0.4K     7    0          -
posixfs      0.0K    0.8K   0.0K   0.8K     4    0          -
papp         0.0K    0.4K   0.0K   0.9K     5    0          -
display      0.0K   75.0K   0.0K  75.0K     1    0          -
desktop      0.0K    0.0K   6.0K   0.0K     0    0          -
console      0.0K    2.5K  13.0K   2.5K     1    0          -
link         0.0K    4.6K  11.0K   4.6K     4    0          -
sh           0.0K    2.8K  12.0K   5.1K     2    0  2048K/64K
sh           0.0K    4.7K  12.0K   5.1K     6    0  2048K/64K
free         0.0K    0.0K   8.0K   0.0K     1    0  2048K/64K
```

**top**

```text
top  up 00:00:26  cpu0   0% cpu1   0% 
int 117K/350K free  ext 16255K/16384K free  tasks 11
 PID NAME        HEAP  STACK
   1 sh            2K    12K
   5 sh            4K    12K
  16 top           8K     8K
```

**perf**

```text
ms             last     mean      max
draw           1.45    11.03    20.60
flush          1.44    11.02    20.59
fps          0  (1 s: draw 0.00, flush 0.00)
pixels/flush 40960
flushes      2 since reset
overlay      off
```

**perf reset**

```text
perf counters reset
```

**perf keys**

```text
keys recorded 100 (last 100 kept)
p50 18.23 ms
p95 19.21 ms
max 20.40 ms
```

</details>

**crypto-bench (hhv2, 320 MHz, Monocypher + PSA/mbedTLS with accelerator, T2.6)**

| operation | result |
|---|---|
| x25519 (scalar multiplication) | 7.14 ms |
| ed25519 sign | 3.50 ms |
| ed25519 verify | 10.05 ms |
| sha-256, 64 KB | 1.33 ms (about 49 MB/s) |
| aes-128-gcm seal, 1.5 KB | 0.63 ms per packet (about 2.4 MB/s) |
| chacha20-poly1305 seal, 1.5 KB | 0.17 ms per packet (about 8.8 MB/s) |

SSH key exchange (x25519 + ed25519 host key verification + ed25519 auth signature): about 21 ms CPU, target ≤ 150 ms.

## 2026-11-13 · internal RAM cost of the radios (T1.5, hhv2, measured with `radio` on the board)

| state | free internal RAM | largest block |
|---|---|---|
| boot, no radio | 199 KB | 128 KB |
| BLE on (advertising) | 162 KB (cost 37 KB) | 92 KB |
| Wi-Fi + BLE (Wi-Fi not yet connected) | 123 KB (Wi-Fi cost 39 KB) | 61 KB |
| both turned off | 191 KB | 61 KB (fragmented) |

- The coexistence target (free ≥ 86 KB, block ≥ 32 KB) is met with Wi-Fi and BLE together: 123 KB and 61 KB.
- The all-time minimum when Wi-Fi started was 84 KB (a temporary peak during initialisation, 39 KB below the steady state).
- After both are turned off, RAM returns to 191 KB, 8 KB does not come back and the largest block stays at 61 KB (fragmentation); watch it in the endurance test.
- Wi-Fi connected to a network has not been measured yet (waiting for the user to connect Wi-Fi); connection buffers will add to the cost.

## 2026-11-13 · after F3 (supervisor, Link security, boot), build 7e30e65, hhv2

| metric | value | target |
|---|---|---|
| boot until services are done | 2403 ms (storage 1698, display +647 including the scan of 8 apps, desktop +15, services +42) | ≤ 2.5 s |
| free internal RAM, radios off | 179 KB (minimum 140 KB, largest block 104 KB) | ≥ 86 KB |
| total internal RAM | 333 KB (the kernel table, crypto and supervisor replace part of 364 KB) | – |

- The `display` stage jumped from 272 to 647 ms because `pal_papp_scan` verifies the hash of every installed package at boot (about 40 ms per app). With many apps installed this will exceed the boot budget; the scan needs to be lazy or in the background (note for F5/F6).

## 2026-11-13 · after F5 (UI, virtual keyboard), build 2f522f5+, hhv2, Wi-Fi on

| metric | value | target | status |
|---|---|---|---|
| boot until services are done | 2872 ms (storage 1699, display +770, desktop +12, services +391) | ≤ 2.5 s | **exceeded** |
| internal RAM | total 312 KB, free 116 KB, minimum 77 KB, largest block 44 KB | free ≥ 86 KB, block ≥ 32 KB | free passes; **minimum 77 KB is below 86 KB** |
| supervisor stack left | ≥ 4900 bytes of 6144 | > 1 KB | passes |

- Total internal RAM fell from 333 KB to 312 KB (21 KB) since F3: overlays, dialogs, the virtual keyboard, UI tables, themes, icons and new tasks. Trace it per owner with `mem`.
- Boot: `display` 770 ms (hash scan of the 9 installed apps at boot) and `services` 391 ms (Wi-Fi turns on at boot because `wifi on` is stored). The app scan must be lazy or in the background.

## 2026-11-14 · internal RAM exhausted after the UI port, UI data moved to PSRAM (hhv2, measured with `mem` on the board)

Symptom: `wifi on` was refused: "needs 74K free and a 21K block, have 43K and 31K". Cause: static data in internal RAM
had grown (`.bss` 178 KB, `.data` 70 KB), leaving only 200 KB of internal heap in total. The biggest consumers were static
UI buffers: 11 kui canvases of 160x120 (2.4 KB each), the home data (12 KB), the app list, the launcher, the notification
ring, and the Settings page store.

Fix: `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` and the `NEMA_EXT_BSS` macro (`nema/include/nema/types.h`) for
large UI data that starts as zero. The kernel table, crypto, kv and DMA buffers stay in internal RAM.

| size | before | after |
| --- | --- | --- |
| `.bss` in DIRAM | 178 840 B | 75 864 B |
| `.data` in DIRAM | 69 818 B | 51 530 B |
| total internal RAM (heap) | 200 KB | 319 KB |
| free after boot, Wi-Fi off | 43 KB (block 31 KB) | 169 KB (min 137 KB, block 104 KB) |
| free with Wi-Fi on | refused | 113 KB (min 81 KB, block 40 KB) |

Rule: new large static data in system/apps uses `NEMA_EXT_BSS` unless there is a written reason to keep it in internal RAM.
Note: safe mode on the board during the measurement came from repeated resets (flashing, opening the serial port) before a
boot survived 30 seconds (`PSYS_SAFE_BOOT_TRIES` 3), not from damage; `safemode off` clears it.

Follow-up (same day): the rest of the Settings store, canvases inside initialised structs (lock, splash, control, file menu),
and the diagnostic buffer were moved too. Internal `.bss` 41 KB, `.data` 41 KB. With Wi-Fi and BLE on together: free
120 KB, minimum 80 KB, largest block 44 KB. `./pal build` now fails when this budget is violated (`forge/tools/ramcheck.ts`).
