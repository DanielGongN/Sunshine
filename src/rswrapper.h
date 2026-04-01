/**
 * @file src/rswrapper.h
 * @brief Reed-Solomon纠错编码的C语言封装
 * 提供串流传输中的前向纠错编码(FEC)功能
 * 替换nanors库的rs.h，支持SIMD向量化加速
 */
#pragma once

#include <stdint.h>

#define DATA_SHARDS_MAX 255 // 数据分片最大数量

typedef struct _reed_solomon reed_solomon; // Reed-Solomon编解码器结构体

typedef reed_solomon *(*reed_solomon_new_t)(int data_shards, int parity_shards); // 创建编解码器的函数指针类型
typedef void (*reed_solomon_release_t)(reed_solomon *rs);
typedef int (*reed_solomon_encode_t)(reed_solomon *rs, uint8_t **shards, int nr_shards, int bs);
typedef int (*reed_solomon_decode_t)(reed_solomon *rs, uint8_t **shards, uint8_t *marks, int nr_shards, int bs);

extern reed_solomon_new_t reed_solomon_new_fn;
extern reed_solomon_release_t reed_solomon_release_fn;
extern reed_solomon_encode_t reed_solomon_encode_fn;
extern reed_solomon_decode_t reed_solomon_decode_fn;

#define reed_solomon_new reed_solomon_new_fn
#define reed_solomon_release reed_solomon_release_fn
#define reed_solomon_encode reed_solomon_encode_fn
#define reed_solomon_decode reed_solomon_decode_fn

/**
 * @brief This initializes the RS function pointers to the best vectorized version available.
 * @details The streaming code will directly invoke these function pointers during encoding.
 */
void reed_solomon_init(void);
