# 上游放音链路参考实现（从 HakiOS 2.3.4 取，仅作参考，未编译）

来源：`xm6661885/hakios-xnu-kernel-umi-fixes`（小米 10 移植，HakiOS 2.3.4）
这些文件**不在本项目构建里**，只作移植参考。

## 为什么放在这里

我们的树（HackiOS-Ace5）只有：

```
xnu/files/bsd/dev/redwood_adsp.c      120 KB   ADSP 启动
xnu/files/bsd/dev/redwood_audio.c      12 KB   /dev/redwood-audio 设备接口
```

**没有任何东西把声音送到扬声器。** 上游比我们多出这四段：

| 文件 | 大小 | 干什么 |
|---|---:|---|
| `hakios.speaker.c` | 43 KB | I2C 上的两个功放，及其温度/电池监控 |
| `hakios.sound.c` | 59 KB | 扬声器放音：告诉 DSP 服务什么、喂数据、限幅 |
| `hakios.mic.c` | 73 KB | 麦克风，经音频 DSP |
| `hakios.guard.c` | 13 KB | 功放保护，限制送进去的电平 |

## 移植到 Ace 5 要改什么

| 上游（小米 10 / POCO） | **Ace 5 实际硬件** |
|---|---|
| CS35L41 功放（I2C） | **SIA91xx**（`oplus_audio_sipa.ko`：`soc_sia91xx_init`、`sipa_i2c_probe`、`sipa_multi_channel_load_fw`） |
| WCD9385 编解码器 | **WCD9378**（`wcd9378_dlkm.ko`） |
| `umi_cs35l41_audio.h` 里的寄存器 | 要从 Ace 5 的 `/vendor/etc/acdbdata/cliffs_*` 和 SIPA 模块里挖 |

## 需要从手机提取的材料（尚未提取）

```
/vendor/lib/modules/oplus_audio_sipa.ko          ← SIA91xx 的 I2C 协议
/vendor/lib/modules/oplus_audio_sipa_tuning.ko
/vendor/lib/modules/wcd9378_dlkm.ko              ← 编解码器
/vendor/etc/acdbdata/cliffs_cdp/                 ← 音频校准库
/vendor/firmware_mnt/image/adsp.b00..b14         ← ADSP 固件
```

## 建议顺序

1. 先把 `adsp=1` 跑起来（`run-payload-adsp.bin`），确认 ADSP 固件能下载
2. 再对着 `hakios.speaker.c` 的结构，用 SIA91xx 的 I2C 协议重写功放探测
3. 最后接 `sound.c` 的喂数据路径

**工作量：周级。不是改几行的事。**
