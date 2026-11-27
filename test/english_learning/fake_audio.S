/*
 * 假的「编进固件」的音频数据，用来在主机侧测试音频查表逻辑。
 *
 * ESP-IDF 的 EMBED_FILES 会把被嵌入的文件转成汇编，符号长这样：
 *
 *     .section .rodata.<name>
 *     .global _binary_<name>_start
 * _binary_<name>_start:
 *     .incbin "<file>"
 *     .global _binary_<name>_end
 * _binary_<name>_end:
 *
 * 这里照抄这个布局，保证 _end 紧跟在 _start 后面（end > start）。
 * 如果用两个独立的 C 数组来伪造，链接器会把它们的先后顺序打乱，
 * end - start 变成负数，测出来的就不是真实行为了。
 */

    .section .rodata.fake_english_audio, "a"
    .align 4

/* 候选 0：_binary_<文件名>_start */
    .global _binary_en_u1_w01_ogg_start
    .global _binary_en_u1_w01_ogg_end
_binary_en_u1_w01_ogg_start:
    .byte 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
_binary_en_u1_w01_ogg_end:

/* 候选 1：_binary_assets_english_<文件名>_start */
    .align 4
    .global _binary_assets_english_en_u1_w02_ogg_start
    .global _binary_assets_english_en_u1_w02_ogg_end
_binary_assets_english_en_u1_w02_ogg_start:
    .byte 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17
_binary_assets_english_en_u1_w02_ogg_end:
