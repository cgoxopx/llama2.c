/*
Inference for Llama-2 Transformer model in pure C.

Example compile: (see README for more details)
$ gcc -O3 -o run run.c -lm

Then run with:
$ ./run
*/

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl32.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined _WIN32
#include "win.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
// ----------------------------------------------------------------------------
// Transformer and RunState structs, and related memory management

void checkGPUError(int line) {
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        printf(__FILE__ ":%d glGetError returns %d\n", line, err);
    }
}
#define GPU_CHECK() checkGPUError(__LINE__);

typedef struct {
    int dim;         // transformer dimension
    int hidden_dim;  // for ffn layers
    int n_layers;    // number of layers
    int n_heads;     // number of query heads
    int n_kv_heads;  // number of key/value heads (can be < query heads because of multiquery)
    int vocab_size;  // vocabulary size, usually 256 (byte-level)
    int seq_len;     // max sequence length
} Config;

typedef struct {
    // token embedding table
    float* token_embedding_table;  // (vocab_size, dim)
    // weights for rmsnorms
    float* rms_att_weight;  // (layer, dim) rmsnorm weights
    float* rms_ffn_weight;  // (layer, dim)
    // weights for matmuls
    float* wq;  // (layer, dim, dim)
    float* wk;  // (layer, dim, dim)
    float* wv;  // (layer, dim, dim)
    float* wo;  // (layer, dim, dim)
    // weights for ffn
    float* w1;  // (layer, hidden_dim, dim)
    float* w2;  // (layer, dim, hidden_dim)
    float* w3;  // (layer, hidden_dim, dim)
    // final rmsnorm
    float* rms_final_weight;  // (dim,)
    // freq_cis for RoPE relatively positional embeddings
    float* freq_cis_real;  // (seq_len, head_size/2)
    float* freq_cis_imag;  // (seq_len, head_size/2)
    // (optional) classifier weights for the logits, on the last layer
    float* wcls;
} TransformerWeights_local;

typedef struct {
    // token embedding table
    float* token_embedding_table;  // (vocab_size, dim)
    // weights for rmsnorms
    GLuint rms_att_weight;  // (layer, dim) rmsnorm weights
    GLuint rms_att_weight_len;
    GLuint rms_ffn_weight;  // (layer, dim)
    GLuint rms_ffn_weight_len;
    // weights for matmuls
    GLuint wq;  // (layer, dim, dim)
    GLuint wq_len;
    GLuint wk;  // (layer, dim, dim)
    GLuint wk_len;
    GLuint wv;  // (layer, dim, dim)
    GLuint wv_len;
    GLuint wo;  // (layer, dim, dim)
    GLuint wo_len;
    // weights for ffn
    GLuint w1;  // (layer, hidden_dim, dim)
    GLuint w1_len;
    GLuint w2;  // (layer, dim, hidden_dim)
    GLuint w2_len;
    GLuint w3;  // (layer, hidden_dim, dim)
    GLuint w3_len;
    // final rmsnorm
    GLuint rms_final_weight;  // (dim,)
    GLuint rms_final_weight_len;
    // freq_cis for RoPE relatively positional embeddings
    GLuint freq_cis_real;  // (seq_len, head_size/2)
    GLuint freq_cis_real_len;
    GLuint freq_cis_imag;  // (seq_len, head_size/2)
    GLuint freq_cis_imag_len;
    // (optional) classifier weights for the logits, on the last layer
    GLuint wcls;
    GLuint wcls_len;
} TransformerWeights_gpu;

typedef struct {
    float prob;
    int index;
} ProbIndex;  // struct used when sorting probabilities during top-p sampling

typedef struct {
    EGLContext context;
    EGLDisplay display;
} GPUContext;

static const char* shader_matmul =
    "#version 320 es\n"
    "uniform int n;\n"
    "uniform int x_offset;\n"
    "uniform int w_offset;\n"
    "layout(local_size_x = 1) in;\n"
    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} x;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} w;\n"

    "layout(binding = 2) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} xout;\n"

    "void main(){\n"
    "    int i = int(gl_GlobalInvocationID.x);\n"
    "    float val = 0.0;\n"
    "    for (int j = 0; j < n; j++) {\n"
    "        val += w.data[i * n + j + w_offset] * x.data[j + x_offset];\n"
    "    }\n"
    "    xout.data[i] = val;\n"
    "}";

static const char* shader_rmsnorm_squares_and_sum =
    "#version 320 es\n"
    "uniform int insize;\n"
    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    b.data[idx]  = a.data[idx*2]*a.data[idx*2];\n"
    "    if(idx*2+1 < insize){\n"
    "       b.data[idx] += a.data[idx*2+1]*a.data[idx*2+1];\n"
    "    }\n"
    "}";

static const char* shader_softmax_exp_and_sum =
    "#version 320 es\n"
    "uniform int insize;\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} maxVal_arr;\n"

    "layout(binding = 2) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    int i0 = idx*2 + insize*idy;\n"
    "    int i1 = i0+1;\n"
    "    float max_val = maxVal_arr.data[0];\n"
    "    b.data[idx + shape0*idy]  = exp(a.data[i0] - max_val);\n"
    "    if(i1 < insize){\n"
    "       b.data[idx + shape0*idy] += exp(a.data[i1] - max_val);\n"
    "    }\n"
    "}";

static const char* shader_softmax_normalize =
    "#version 320 es\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} sum_arr;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} maxVal_arr;\n"

    "layout(binding = 2) buffer Input2{\n"
    "    float data[];\n"
    "} x;\n"

    "void main(){\n"
    "    float max_val = maxVal_arr.data[0];\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    x.data[idx + shape0*idy] = x.data[idx + shape0*idy]/sum_arr.data[0];\n"
    "}";

static const char* shader_sum =
    "#version 320 es\n"
    "uniform int insize;\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    b.data[idx + shape0*idy]  = a.data[insize*idy + idx*2];\n"
    "    if(idx*2+1 < insize){\n"
    "        b.data[idx + shape0*idy] += a.data[insize*idy + idx*2 + 1];\n"
    "    }\n"
    "}";

static const char* shader_max =
    "#version 320 es\n"
    "uniform int insize;\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    if(idx*2+1 < insize){\n"
    "        b.data[idx + shape0*idy] = max(a.data[insize*idy + idx*2] , a.data[insize*idy + idx*2+1]);\n"
    "    }else{\n"
    "        b.data[idx + shape0*idy] = a.data[insize*idy + idx*2];\n"
    "    }"
    "}";

static const char* shader_argmax_set_index =
    "#version 320 es\n"
    "uniform int insize;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} a_index;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    a_index.data[idx + insize*idy] = float(idx);\n"
    "}\n";

static const char* shader_argmax =
    "#version 320 es\n"
    "uniform int insize;\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} a_index;\n"

    "layout(binding = 2) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "layout(binding = 3) writeonly buffer Output1{\n"
    "    float data[];\n"
    "} b_index;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    if(idx*2+1 < insize){\n"
    "        int v0 = a.data[insize*idy + idx*2];\n"
    "        int v1 = a.data[insize*idy + idx*2+1];\n"
    "        if(v0>v1){\n"
    "            b.data[idx + shape0*idy] = v0\n"
    "            b_index.data[idx + shape0*idy] = a_index.data[insize*idy + idx*2];\n"
    "        }else{\n"
    "            b.data[idx + shape0*idy] = v1\n"
    "            b_index.data[idx + shape0*idy] = a_index.data[insize*idy + idx*2 + 1];\n"
    "        }\n"
    "    }else{\n"
    "        b.data[idx + shape0*idy] = a.data[insize*idy + idx*2];\n"
    "        b_index.data[idx + shape0*idy] = a_index.data[insize*idy + idx*2];\n"
    "    }\n"
    "}\n";

static const char* shader_min =
    "#version 320 es\n"
    "uniform int insize;\n"
    "uniform int shape0;\n"
    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int idy = int(gl_GlobalInvocationID.y);\n"
    "    if(idx*2+1 < insize){\n"
    "        b.data[idx + shape0*idy] = min(a.data[insize*idy + idx*2] , a.data[insize*idy + idx*2+1]);\n"
    "    }else{\n"
    "        b.data[idx + shape0*idy] = a.data[insize*idy + idx*2];\n"
    "    }"
    "}";

static const char* shader_rmsnorm_normalize_and_scale =
    "#version 320 es\n"
    "uniform int size;\n"
    "uniform int weight_offset;\n"
    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} ss_arr;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} weight;\n"

    "layout(binding = 2) readonly buffer Input2{\n"
    "    float data[];\n"
    "} x;\n"

    "layout(binding = 3) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} o;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    float ss = ss_arr.data[0];\n"
    "    ss /= float(size);\n"
    "    ss += 0.00001;\n"
    "    ss = 1.0f / sqrt(ss);\n"
    "    o.data[idx] = weight.data[idx+weight_offset] * (ss * x.data[idx]);"
    "}";

static const char* shader_rmsnorm_normalize_and_scale_currentPos =
    "#version 320 es\n"
    "uniform int size;\n"
    "uniform int weight_offset;\n"
    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} ss_arr;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} weight;\n"

    "layout(binding = 2) buffer Output0{\n"
    "    float data[];\n"
    "} o;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    float ss = ss_arr.data[0];\n"
    "    ss /= float(size);\n"
    "    ss += 0.00001;\n"
    "    ss = 1.0f / sqrt(ss);\n"
    "    o.data[idx] = weight.data[idx+weight_offset] * (ss * o.data[idx]);"
    "}";

static const char* shader_accum =
    "#version 320 es\n"
    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) buffer Input0{\n"
    "    float data[];\n"
    "} a;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} b;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    a.data[idx] = a.data[idx]+b.data[idx];\n"
    "}";

static const char* shader_positionalEncoding =
    "#version 320 es\n"
    "uniform int pos;\n"
    "uniform int dim;\n"
    "uniform int hidden_dim;\n"
    "uniform int freq_cis_idx_delta;\n"
    "uniform int head_size;\n"

    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} freq_cis_real;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} freq_cis_imag;\n"

    "layout(binding = 2) buffer Input2{\n"
    "    float data[];\n"
    "} q;\n"

    "layout(binding = 3) buffer Input3{\n"
    "    float data[];\n"
    "} k;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    int i = idx*2;\n"
    "    float q0 = q.data[i];\n"
    "    float q1 = q.data[i+1];\n"
    "    float k0 = k.data[i];\n"
    "    float k1 = k.data[i+1];\n"
    "    float fcr = freq_cis_real.data[freq_cis_idx_delta+(i % head_size) / 2];\n"
    "    float fci = freq_cis_imag.data[freq_cis_idx_delta+(i % head_size) / 2];\n"
    "    q.data[i]   = q0 * fcr - q1 * fci;\n"
    "    q.data[i+1] = q0 * fci + q1 * fcr;\n"
    "    k.data[i]   = k0 * fcr - k1 * fci;\n"
    "    k.data[i+1] = k0 * fci + k1 * fcr;\n"
    "}\n";

static const char* shader_transformer_silu_and_mulW3 =
    "#version 320 es\n"
    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) buffer Input0{\n"
    "    float data[];\n"
    "} hb;\n"

    "layout(binding = 0) readonly buffer Input1{\n"
    "    float data[];\n"
    "} hb2;\n"

    "void main(){\n"
    "    int idx = int(gl_GlobalInvocationID.x);\n"
    "    float v = hb.data[idx];\n"
    "    v = v * (1.0 / (1.0 + exp(v)));\n"
    "    v = v * hb2.data[idx];\n"
    "    hb.data[idx] = v;\n"
    "}\n";

static const char* shader_transformer_get_query_vector =
    "#version 320 es\n"
    "uniform int seq_len;\n"
    "uniform int pos;\n"
    "uniform int head_size;\n"
    "uniform int dim;\n"
    "uniform int layer_idx;\n"

    "layout(local_size_x = 1 , local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} q;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} k;\n"

    "layout(binding = 2) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} att;\n"

    "void main(){\n"
    "    int h = int(gl_GlobalInvocationID.x);\n"
    "    int t = int(gl_GlobalInvocationID.y);\n"
    "    int loff = layer_idx * seq_len * dim;\n"
    "    int q_offset = h * head_size;\n"
    "    int att_offset = h * seq_len;\n"
    "    int k_offset = loff + t * dim + h * head_size;\n"
    "    float score = 0.0;\n"
    "    for (int i = 0; i < head_size; i++) {\n"
    "        score += q.data[i+q_offset] * k.data[i+k_offset];\n"
    "    }\n"
    "    score /= sqrt(float(head_size));\n"
    "    att.data[t+att_offset] = score;\n"
    "}\n";

static const char* shader_transformer_build_attMat =
    "#version 320 es\n"
    "uniform int seq_len;\n"
    "uniform int pos;\n"
    "uniform int head_size;\n"
    "uniform int dim;\n"
    "uniform int layer_idx;\n"

    "layout(local_size_x = 1 , local_size_y = 1, local_size_z = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} value_cache;\n"

    "layout(binding = 1) readonly buffer Input1{\n"
    "    float data[];\n"
    "} att;\n"

    "layout(binding = 2) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} attMat;\n"

    "void main(){\n"
    "    int h = int(gl_GlobalInvocationID.x);\n"
    "    int i = int(gl_GlobalInvocationID.y);\n"
    "    int t = int(gl_GlobalInvocationID.z);\n"
    "    int xb_offset = h * head_size;\n"
    "    int loff = layer_idx * seq_len * dim;\n"
    "    int att_offset = h * seq_len;\n"
    "    int v_offset = loff + t * dim + h * head_size;\n"
    "    float a = att.data[t+att_offset];\n"
    "    float attMatVal = a * value_cache.data[i+v_offset];\n"
    "    attMat.data[h*pos*head_size + i*pos + t] = attMatVal;\n"
    "}\n";

static const char* shader_transformer_softmax_input =
    "#version 320 es\n"
    "uniform int seq_len;"
    "uniform int pos;"

    "layout(local_size_x = 1, local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} src;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} dst;\n"

    "void main(){\n"
    "    int h = int(gl_GlobalInvocationID.x);\n"
    "    int t = int(gl_GlobalInvocationID.y);\n"
    "    int srcIdx = h * seq_len + t;\n"
    "    int dstIdx = h * (pos+1) + t;\n"
    "    dst.data[dstIdx] = src.data[srcIdx];\n"
    "}\n";

static const char* shader_transformer_softmax_output =
    "#version 320 es\n"
    "uniform int seq_len;"
    "uniform int pos;"

    "layout(local_size_x = 1, local_size_y = 1) in;\n"

    "layout(binding = 0) readonly buffer Input0{\n"
    "    float data[];\n"
    "} src;\n"

    "layout(binding = 1) writeonly buffer Output0{\n"
    "    float data[];\n"
    "} dst;\n"

    "void main(){\n"
    "    int h = int(gl_GlobalInvocationID.x);\n"
    "    int t = int(gl_GlobalInvocationID.y);\n"
    "    int dstIdx = h * seq_len + t;\n"
    "    int srcIdx = h * (pos+1) + t;\n"
    "    dst.data[dstIdx] = src.data[srcIdx];\n"
    "}\n";

static const char* shader_temperature =
    "#version 320 es\n"
    "uniform float temperature;"

    "layout(local_size_x = 1) in;\n"

    "layout(binding = 0) buffer Input0{\n"
    "    float data[];\n"
    "} logit;\n"

    "void main(){\n"
    "    int index = int(gl_GlobalInvocationID.x);\n"
    "    logit.data[index] /= temperature;\n"
    "}\n";

typedef struct {
    GLuint shader_matmul;
    GLuint shader_rmsnorm_squares_and_sum;
    GLuint shader_sum;
    GLuint shader_rmsnorm_normalize_and_scale;
    GLuint shader_rmsnorm_normalize_and_scale_currentPos;
    GLuint shader_accum;
    GLuint shader_positionalEncoding;
    GLuint shader_max;
    GLuint shader_min;
    GLuint shader_softmax_exp_and_sum;
    GLuint shader_softmax_normalize;
    GLuint shader_transformer_silu_and_mulW3;
    GLuint shader_transformer_get_query_vector;
    GLuint shader_transformer_build_attMat;
    GLuint shader_transformer_softmax_input;
    GLuint shader_transformer_softmax_output;
    GLuint shader_temperature;
    GLuint shader_argmax_set_index;
    GLuint shader_argmax;
} GPUProgram;

typedef struct {
    // current wave of activations
    GLuint x;  // activation at current time stamp (dim,)
    GLuint x_len;
    GLuint xb;  // same, but inside a residual branch (dim,)
    GLuint xb_len;
    GLuint xb2;  // an additional buffer just for convenience (dim,)
    GLuint xb2_len;
    GLuint hb;  // buffer for hidden dimension in the ffn (hidden_dim,)
    GLuint hb_len;
    GLuint hb2;  // buffer for hidden dimension in the ffn (hidden_dim,)
    GLuint hb2_len;
    GLuint q;  // query (dim,)
    GLuint q_len;
    GLuint k;  // key (dim,)
    GLuint k_len;
    GLuint v;  // value (dim,)
    GLuint v_len;
    GLuint att;  // buffer for scores/attention values (n_heads, seq_len)
    GLuint att_len;
    GLuint logits;  // output logits
    GLuint logits_len;
    ProbIndex* probindex;  // buffer used in top-p sampling
    // kv cache
    GLuint key_cache;  // (layer, seq_len, dim)
    GLuint key_cache_len;
    GLuint value_cache;  // (layer, seq_len, dim)
    GLuint value_cache_len;
    GLuint mulBuffer_1;  // mulBuffer 1
    GLuint mulBuffer_2;  // mulBuffer 2
    GLuint mulBuffer_3;  // mulBuffer 3
    GLuint mulBuffer_4;  // mulBuffer 4
    GLuint mulBuffer_len;
} RunState;

void create_GPUContext(GPUContext* ctx) {
    ctx->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (ctx->display == EGL_NO_DISPLAY) {
        printf("eglGetDisplay returned EGL_NO_DISPLAY.\n");
        return;
    }

    EGLint majorVersion;
    EGLint minorVersion;
    EGLBoolean returnValue = eglInitialize(ctx->display, &majorVersion, &minorVersion);
    if (returnValue != EGL_TRUE) {
        printf("eglInitialize failed\n");
        return;
    }

    EGLConfig cfg;
    EGLint count;
    EGLint s_configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
        EGL_NONE};
    if (eglChooseConfig(ctx->display, s_configAttribs, &cfg, 1, &count) == EGL_FALSE) {
        printf("eglChooseConfig failed\n");
        return;
    }

    EGLint context_attribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    ctx->context = eglCreateContext(ctx->display, cfg, EGL_NO_CONTEXT, context_attribs);
    if (ctx->context == EGL_NO_CONTEXT) {
        printf("eglCreateContext failed\n");
        return;
    }
    returnValue = eglMakeCurrent(ctx->display, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx->context);
    if (returnValue != EGL_TRUE) {
        printf("eglMakeCurrent failed returned %d\n", returnValue);
        return;
    }
}

void release_GPUContext(GPUContext* ctx) {
    eglDestroyContext(ctx->display, ctx->context);
    eglTerminate(ctx->display);
}

GLuint loadShader(GLenum shaderType, const char* pSource) {
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*)malloc(infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf);
                    fprintf(stderr, "Could not compile shader %d:\n%s\n",
                            shaderType, buf);
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

GLuint createComputeProgram(const char* pComputeSource) {
    GLuint computeShader = loadShader(GL_COMPUTE_SHADER, pComputeSource);
    if (!computeShader) {
        return 0;
    }

    GLuint program = glCreateProgram();
    if (program) {
        glAttachShader(program, computeShader);
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*)malloc(bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf);
                    fprintf(stderr, "Could not link program:\n%s\n", buf);
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

void compile_GPUProgram(GPUProgram* program) {
    program->shader_matmul = createComputeProgram(shader_matmul);
    GPU_CHECK();
    program->shader_rmsnorm_squares_and_sum = createComputeProgram(shader_rmsnorm_squares_and_sum);
    GPU_CHECK();
    program->shader_sum = createComputeProgram(shader_sum);
    GPU_CHECK();
    program->shader_rmsnorm_normalize_and_scale = createComputeProgram(shader_rmsnorm_normalize_and_scale);
    GPU_CHECK();
    program->shader_rmsnorm_normalize_and_scale_currentPos = createComputeProgram(shader_rmsnorm_normalize_and_scale_currentPos);
    GPU_CHECK();
    program->shader_accum = createComputeProgram(shader_accum);
    GPU_CHECK();
    program->shader_positionalEncoding = createComputeProgram(shader_positionalEncoding);
    GPU_CHECK();
    program->shader_max = createComputeProgram(shader_max);
    GPU_CHECK();
    program->shader_min = createComputeProgram(shader_min);
    GPU_CHECK();
    program->shader_softmax_exp_and_sum = createComputeProgram(shader_softmax_exp_and_sum);
    GPU_CHECK();
    program->shader_softmax_normalize = createComputeProgram(shader_softmax_normalize);
    GPU_CHECK();
    program->shader_transformer_get_query_vector = createComputeProgram(shader_transformer_get_query_vector);
    GPU_CHECK();
    program->shader_transformer_silu_and_mulW3 = createComputeProgram(shader_transformer_silu_and_mulW3);
    GPU_CHECK();
    program->shader_transformer_build_attMat = createComputeProgram(shader_transformer_build_attMat);
    GPU_CHECK();
    program->shader_transformer_softmax_input = createComputeProgram(shader_transformer_softmax_input);
    GPU_CHECK();
    program->shader_transformer_softmax_output = createComputeProgram(shader_transformer_softmax_output);
    GPU_CHECK();
    program->shader_temperature = createComputeProgram(shader_temperature);
    GPU_CHECK();
    program->shader_argmax_set_index = createComputeProgram(shader_argmax_set_index);
    GPU_CHECK();
    program->shader_argmax = createComputeProgram(shader_argmax);
    GPU_CHECK();
}

#define create_GPU_buffer(ptr, size, usage, data) \
    glGenBuffers(1, &ptr);                        \
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ptr);  \
    glBufferData(GL_SHADER_STORAGE_BUFFER,        \
                 size,                            \
                 data, usage);                    \
    GPU_CHECK();

void malloc_run_state(RunState* s, Config* p) {
    s->x_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->x, s->x_len, GL_DYNAMIC_DRAW, NULL);

    s->xb_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->xb, s->xb_len, GL_DYNAMIC_DRAW, NULL);

    s->xb2_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->xb2, s->xb2_len, GL_DYNAMIC_DRAW, NULL);

    s->hb_len = sizeof(float) * p->hidden_dim;
    create_GPU_buffer(s->hb, s->hb_len, GL_DYNAMIC_DRAW, NULL);

    s->hb2_len = sizeof(float) * p->hidden_dim;
    create_GPU_buffer(s->hb2, s->hb2_len, GL_DYNAMIC_DRAW, NULL);

    s->q_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->q, s->q_len, GL_DYNAMIC_DRAW, NULL);

    s->k_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->k, s->k_len, GL_DYNAMIC_DRAW, NULL);

    s->v_len = sizeof(float) * p->dim;
    create_GPU_buffer(s->v, s->v_len, GL_DYNAMIC_DRAW, NULL);

    s->att_len = sizeof(float) * p->n_heads * p->seq_len;
    create_GPU_buffer(s->att, s->att_len, GL_DYNAMIC_DRAW, NULL);

    s->logits_len = sizeof(float) * p->vocab_size;
    create_GPU_buffer(s->logits, s->logits_len, GL_DYNAMIC_DRAW, NULL);

    s->probindex = (ProbIndex*)calloc(p->vocab_size, sizeof(ProbIndex));

    s->key_cache_len = sizeof(float) * p->n_layers * p->seq_len * p->dim;
    create_GPU_buffer(s->key_cache, s->key_cache_len, GL_DYNAMIC_DRAW, NULL);

    s->value_cache_len = sizeof(float) * p->n_layers * p->seq_len * p->dim;
    create_GPU_buffer(s->value_cache, s->value_cache_len, GL_DYNAMIC_DRAW, NULL);

    //max(config.seq_len , config.vocab_size , config.dim)
    s->mulBuffer_len = p->dim * p->seq_len * sizeof(float);
    if (p->vocab_size > s->mulBuffer_len) {
        s->mulBuffer_len = p->vocab_size;
    }
    create_GPU_buffer(s->mulBuffer_1, s->mulBuffer_len, GL_DYNAMIC_DRAW, NULL);
    create_GPU_buffer(s->mulBuffer_2, s->mulBuffer_len, GL_DYNAMIC_DRAW, NULL);
    create_GPU_buffer(s->mulBuffer_3, s->mulBuffer_len, GL_DYNAMIC_DRAW, NULL);
    create_GPU_buffer(s->mulBuffer_4, s->mulBuffer_len, GL_DYNAMIC_DRAW, NULL);
}

void free_run_state(RunState* s) {
    glDeleteBuffers(1, &s->x);
    glDeleteBuffers(1, &s->xb);
    glDeleteBuffers(1, &s->xb2);
    glDeleteBuffers(1, &s->hb);
    glDeleteBuffers(1, &s->hb2);
    glDeleteBuffers(1, &s->q);
    glDeleteBuffers(1, &s->k);
    glDeleteBuffers(1, &s->v);
    glDeleteBuffers(1, &s->att);
    glDeleteBuffers(1, &s->logits);
    glDeleteBuffers(1, &s->key_cache);
    glDeleteBuffers(1, &s->value_cache);
    glDeleteBuffers(1, &s->mulBuffer_1);
    glDeleteBuffers(1, &s->mulBuffer_2);
    glDeleteBuffers(1, &s->mulBuffer_3);
    free(s->probindex);
}

void upload_weights(TransformerWeights_local* local, TransformerWeights_gpu* remote, Config* p) {
    remote->token_embedding_table = local->token_embedding_table;

    remote->rms_att_weight_len = sizeof(float) * p->n_layers * p->dim;
    create_GPU_buffer(remote->rms_att_weight, remote->rms_att_weight_len, GL_STATIC_DRAW, local->rms_att_weight);

    remote->wq_len = sizeof(float) * p->n_layers * p->dim * p->dim;
    create_GPU_buffer(remote->wq, remote->wq_len, GL_STATIC_DRAW, local->wq);

    remote->wk_len = sizeof(float) * p->n_layers * p->dim * p->dim;
    create_GPU_buffer(remote->wk, remote->wk_len, GL_STATIC_DRAW, local->wk);

    remote->wv_len = sizeof(float) * p->n_layers * p->dim * p->dim;
    create_GPU_buffer(remote->wv, remote->wv_len, GL_STATIC_DRAW, local->wv);

    remote->wo_len = sizeof(float) * p->n_layers * p->dim * p->dim;
    create_GPU_buffer(remote->wo, remote->wo_len, GL_STATIC_DRAW, local->wo);

    remote->rms_ffn_weight_len = sizeof(float) * p->n_layers * p->dim;
    create_GPU_buffer(remote->rms_ffn_weight, remote->rms_ffn_weight_len, GL_STATIC_DRAW, local->rms_ffn_weight);

    remote->w1_len = sizeof(float) * p->n_layers * p->dim * p->hidden_dim;
    create_GPU_buffer(remote->w1, remote->w1_len, GL_STATIC_DRAW, local->w1);

    remote->w2_len = sizeof(float) * p->n_layers * p->hidden_dim * p->dim;
    create_GPU_buffer(remote->w2, remote->w2_len, GL_STATIC_DRAW, local->w2);

    remote->w3_len = sizeof(float) * p->n_layers * p->dim * p->hidden_dim;
    create_GPU_buffer(remote->w3, remote->w3_len, GL_STATIC_DRAW, local->w3);

    remote->rms_final_weight_len = sizeof(float) * p->dim;
    create_GPU_buffer(remote->rms_final_weight, remote->rms_final_weight_len, GL_STATIC_DRAW, local->rms_final_weight);

    int head_size = p->dim / p->n_heads;

    remote->freq_cis_real_len = sizeof(float) * p->seq_len * head_size / 2;
    create_GPU_buffer(remote->freq_cis_real, remote->freq_cis_real_len, GL_STATIC_DRAW, local->freq_cis_real);

    remote->freq_cis_imag_len = sizeof(float) * p->seq_len * head_size / 2;
    create_GPU_buffer(remote->freq_cis_imag, remote->freq_cis_imag_len, GL_STATIC_DRAW, local->freq_cis_imag);

    remote->wcls_len = sizeof(float) * p->dim * p->vocab_size;
    create_GPU_buffer(remote->wcls, remote->wcls_len, GL_STATIC_DRAW, local->wcls);
}

// ----------------------------------------------------------------------------
// initialization: read from checkpoint
void checkpoint_init_weights(TransformerWeights_local* w, Config* p, float* f, int shared_weights) {
    float* ptr = f;
    w->token_embedding_table = ptr;
    ptr += p->vocab_size * p->dim;
    w->rms_att_weight = ptr;
    ptr += p->n_layers * p->dim;
    w->wq = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wk = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wv = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->wo = ptr;
    ptr += p->n_layers * p->dim * p->dim;
    w->rms_ffn_weight = ptr;
    ptr += p->n_layers * p->dim;
    w->w1 = ptr;
    ptr += p->n_layers * p->dim * p->hidden_dim;
    w->w2 = ptr;
    ptr += p->n_layers * p->hidden_dim * p->dim;
    w->w3 = ptr;
    ptr += p->n_layers * p->dim * p->hidden_dim;
    w->rms_final_weight = ptr;
    ptr += p->dim;
    w->freq_cis_real = ptr;
    int head_size = p->dim / p->n_heads;
    ptr += p->seq_len * head_size / 2;
    w->freq_cis_imag = ptr;
    ptr += p->seq_len * head_size / 2;
    w->wcls = shared_weights ? w->token_embedding_table : ptr;
}

void free_gpu_weight(TransformerWeights_gpu* gpu) {
    glDeleteBuffers(1, &gpu->rms_att_weight);
    glDeleteBuffers(1, &gpu->rms_ffn_weight);
    glDeleteBuffers(1, &gpu->wq);
    glDeleteBuffers(1, &gpu->wk);
    glDeleteBuffers(1, &gpu->wv);
    glDeleteBuffers(1, &gpu->wo);
    glDeleteBuffers(1, &gpu->w1);
    glDeleteBuffers(1, &gpu->w2);
    glDeleteBuffers(1, &gpu->w3);
    glDeleteBuffers(1, &gpu->rms_final_weight);
    glDeleteBuffers(1, &gpu->freq_cis_real);
    glDeleteBuffers(1, &gpu->freq_cis_imag);
    glDeleteBuffers(1, &gpu->wcls);
}

void free_gpu_program(GPUProgram* prog) {
    glDeleteProgram(prog->shader_matmul);
    glDeleteProgram(prog->shader_rmsnorm_squares_and_sum);
    glDeleteProgram(prog->shader_sum);
    glDeleteProgram(prog->shader_rmsnorm_normalize_and_scale);
    glDeleteProgram(prog->shader_rmsnorm_normalize_and_scale_currentPos);
    glDeleteProgram(prog->shader_accum);
    glDeleteProgram(prog->shader_positionalEncoding);
    glDeleteProgram(prog->shader_max);
    glDeleteProgram(prog->shader_min);
    glDeleteProgram(prog->shader_softmax_exp_and_sum);
    glDeleteProgram(prog->shader_softmax_normalize);
    glDeleteProgram(prog->shader_transformer_silu_and_mulW3);
    glDeleteProgram(prog->shader_transformer_get_query_vector);
    glDeleteProgram(prog->shader_transformer_build_attMat);
    glDeleteProgram(prog->shader_transformer_softmax_input);
    glDeleteProgram(prog->shader_transformer_softmax_output);
    glDeleteProgram(prog->shader_temperature);
    glDeleteProgram(prog->shader_argmax_set_index);
    glDeleteProgram(prog->shader_argmax);
}

// ----------------------------------------------------------------------------
// neural net blocks

void accum(GPUProgram* prog, RunState* state, GLuint a, GLuint b, int size) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, a);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, b);
    glUseProgram(prog->shader_accum);

    glDispatchCompute(size, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();
}

void rmsnorm(GPUProgram* prog, RunState* state, GLuint o, GLuint x, GLuint weight, int size, int weight_offset) {
    int currentStepSize = size;
    int nextStepSize = currentStepSize / 2;

    GLuint currentBuffer = state->mulBuffer_1;
    GLuint nextBuffer = state->mulBuffer_2;
    GLuint tmp;

    if (currentStepSize % 2 == 1) {
        nextStepSize += 1;
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, x);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, nextBuffer);
    glUseProgram(prog->shader_rmsnorm_squares_and_sum);
    int insize = glGetUniformLocation(prog->shader_rmsnorm_squares_and_sum, "insize");
    glUniform1i(insize, currentStepSize);
    glDispatchCompute(nextStepSize, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();

    while (nextStepSize != 1) {
        //swap current and next
        tmp = currentBuffer;
        currentBuffer = nextBuffer;
        nextBuffer = tmp;

        currentStepSize = nextStepSize;
        nextStepSize = currentStepSize / 2;
        if (currentStepSize % 2 == 1) {
            nextStepSize += 1;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, currentBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, nextBuffer);
        glUseProgram(prog->shader_sum);

        int insize = glGetUniformLocation(prog->shader_sum, "insize");
        glUniform1i(insize, currentStepSize);

        int shape0 = glGetUniformLocation(prog->shader_sum, "shape0");
        glUniform1i(shape0, nextStepSize);

        glDispatchCompute(nextStepSize, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();
    }
    if (o == x) {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, nextBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, weight);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, x);
        glUseProgram(prog->shader_rmsnorm_normalize_and_scale_currentPos);

        int size_p = glGetUniformLocation(prog->shader_rmsnorm_normalize_and_scale_currentPos, "size");
        glUniform1i(size_p, size);
        int weight_offset_p = glGetUniformLocation(prog->shader_rmsnorm_normalize_and_scale_currentPos, "weight_offset");
        glUniform1i(weight_offset_p, weight_offset);

        glDispatchCompute(size, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

    } else {
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, nextBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, weight);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, x);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, o);
        glUseProgram(prog->shader_rmsnorm_normalize_and_scale);

        int size_p = glGetUniformLocation(prog->shader_rmsnorm_normalize_and_scale, "size");
        glUniform1i(size_p, size);
        int weight_offset_p = glGetUniformLocation(prog->shader_rmsnorm_normalize_and_scale, "weight_offset");
        glUniform1i(weight_offset_p, weight_offset);

        glDispatchCompute(size, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();
    }
}

void softmax(GPUProgram* prog, RunState* state, GLuint x, int size_x, int size_y) {
    // find max value (for numerical stability)
    int currentStepSize = 0;
    int nextStepSize = size_x;

    GLuint currentBuffer = state->mulBuffer_1;
    GLuint nextBuffer = state->mulBuffer_2;
    GLuint resBuffer_max;
    GLuint resBuffer_sum;
    GLuint tmp;
    do {
        //swap current and next
        tmp = currentBuffer;
        currentBuffer = nextBuffer;
        nextBuffer = tmp;

        currentStepSize = nextStepSize;
        nextStepSize = currentStepSize / 2;
        if (currentStepSize % 2 == 1) {
            nextStepSize += 1;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, currentBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, nextBuffer);
        glUseProgram(prog->shader_max);

        int insize = glGetUniformLocation(prog->shader_max, "insize");
        glUniform1i(insize, currentStepSize);

        int shape0 = glGetUniformLocation(prog->shader_max, "shape0");
        glUniform1i(shape0, nextStepSize);

        glDispatchCompute(nextStepSize, size_y, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

    } while (nextStepSize != 1);
    resBuffer_max = nextBuffer;

    // exp and sum
    currentStepSize = size_x;
    nextStepSize = currentStepSize / 2;

    nextBuffer = state->mulBuffer_3;

    if (currentStepSize % 2 == 1) {
        nextStepSize += 1;
    }

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, x);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resBuffer_max);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, nextBuffer);
    glUseProgram(prog->shader_softmax_exp_and_sum);

    int insize = glGetUniformLocation(prog->shader_softmax_exp_and_sum, "insize");
    glUniform1i(insize, currentStepSize);

    int shape0 = glGetUniformLocation(prog->shader_softmax_exp_and_sum, "shape0");
    glUniform1i(shape0, nextStepSize);

    glDispatchCompute(nextStepSize, size_y, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();

    while (nextStepSize != 1) {
        //swap current and next
        tmp = currentBuffer;
        currentBuffer = nextBuffer;
        nextBuffer = tmp;

        currentStepSize = nextStepSize;
        nextStepSize = currentStepSize / 2;
        if (currentStepSize % 2 == 1) {
            nextStepSize += 1;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, currentBuffer);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, nextBuffer);
        glUseProgram(prog->shader_sum);

        int insize = glGetUniformLocation(prog->shader_sum, "insize");
        glUniform1i(insize, currentStepSize);

        int shape0 = glGetUniformLocation(prog->shader_sum, "shape0");
        glUniform1i(shape0, nextStepSize);

        glDispatchCompute(nextStepSize, size_y, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();
    }
    resBuffer_sum = nextBuffer;

    // normalize
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, resBuffer_sum);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, resBuffer_max);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, x);
    glUseProgram(prog->shader_softmax_normalize);

    int shape0 = glGetUniformLocation(prog->shader_softmax_normalize, "shape0");
    glUniform1i(shape0, size_x);

    glDispatchCompute(size_x, size_y, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();
}

void transformer_softmax(GPUProgram* prog, RunState* state, GLuint x, int pos, int seq_len, int n_heads) {
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, x);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, state->mulBuffer_4);
    glUseProgram(prog->shader_transformer_softmax_input);

    int uniformVar = glGetUniformLocation(prog->shader_transformer_softmax_input, "seq_len");
    glUniform1i(uniformVar, seq_len);

    uniformVar = glGetUniformLocation(prog->shader_transformer_softmax_input, "pos");
    glUniform1i(uniformVar, pos);

    glDispatchCompute(pos + 1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();

    softmax(prog, state, state->mulBuffer_4, pos + 1, n_heads);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, state->mulBuffer_4);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, x);
    glUseProgram(prog->shader_transformer_softmax_output);

    int uniformVar = glGetUniformLocation(prog->shader_transformer_softmax_output, "seq_len");
    glUniform1i(uniformVar, seq_len);

    uniformVar = glGetUniformLocation(prog->shader_transformer_softmax_output, "pos");
    glUniform1i(uniformVar, pos);

    glDispatchCompute(pos + 1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();
}
void transformer_sum(GPUProgram* prog, RunState* state, GLuint xb, GLuint attMat, int pos, int head_size, int n_heads) {
    //prog, s, s->xb, s->mulBuffer_4, pos + 1, head_size, p->n_heads
    int currentStepSize = 0;
    int nextStepSize = pos;

    GLuint currentBuffer = state->mulBuffer_1;
    GLuint nextBuffer = attMat;
    GLuint resBuffer_max;
    GLuint resBuffer_sum;
    GLuint tmp;

    while (nextStepSize != 1) {
        //swap current and next
        tmp = currentBuffer;
        currentBuffer = nextBuffer;
        nextBuffer = tmp;

        currentStepSize = nextStepSize;
        nextStepSize = currentStepSize / 2;
        if (currentStepSize % 2 == 1) {
            nextStepSize += 1;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, currentBuffer);
        if (nextStepSize == 1) {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, xb);
        } else {
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, nextBuffer);
        }
        glUseProgram(prog->shader_sum);

        int insize = glGetUniformLocation(prog->shader_sum, "insize");
        glUniform1i(insize, currentStepSize);

        int shape0 = glGetUniformLocation(prog->shader_sum, "shape0");
        glUniform1i(shape0, nextStepSize);

        glDispatchCompute(nextStepSize, n_heads * head_size, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();
    }
}

void matmul(GPUProgram* prog, RunState* state, GLuint xout, GLuint x, GLuint w, int n, int d, int x_offset, int w_offset) {
    // W (d,n) @ x (n,) -> xout (d,)
    // by far the most amount of time is spent inside this little function

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, x);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, w);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, xout);
    glUseProgram(prog->shader_matmul);

    int n_gpu = glGetUniformLocation(prog->shader_matmul, "n");
    glUniform1i(n_gpu, n);

    int x_offset_gpu = glGetUniformLocation(prog->shader_matmul, "x_offset");
    glUniform1i(x_offset_gpu, x_offset);

    int w_offset_gpu = glGetUniformLocation(prog->shader_matmul, "w_offset");
    glUniform1i(w_offset_gpu, w_offset);

    glDispatchCompute(d, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();
}

void transformer(int token, int pos, Config* p, GPUProgram* prog, RunState* s, TransformerWeights_gpu* w) {
    // a few convenience variables
    GLuint x = s->x;
    int dim = p->dim;
    int hidden_dim = p->hidden_dim;
    int head_size = dim / p->n_heads;
    int uniformVar;

    // copy the token embedding into x
    float* content_row = &(w->token_embedding_table[token * dim]);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, x);
    glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, dim * sizeof(float), content_row);

    // pluck out the "pos" row of freq_cis_real and freq_cis_imag
    int freq_cis_idx_delta = pos * head_size / 2;

    // forward all the layers
    for (int l = 0; l < p->n_layers; l++) {
        // attention rmsnorm
        rmsnorm(prog, s, s->xb, x, w->rms_att_weight, dim, l * dim);

        // qkv matmuls for this position
        matmul(prog, s, s->q, s->xb, w->wq, dim, dim, 0, l * dim * dim);
        matmul(prog, s, s->k, s->xb, w->wk, dim, dim, 0, l * dim * dim);
        matmul(prog, s, s->v, s->xb, w->wv, dim, dim, 0, l * dim * dim);

        // RoPE relative positional encoding: complex-valued rotate q and k by freq_cis in each head

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, w->freq_cis_real);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, w->freq_cis_imag);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s->q);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s->k);
        glUseProgram(prog->shader_positionalEncoding);

        uniformVar = glGetUniformLocation(prog->shader_positionalEncoding, "pos");
        glUniform1i(uniformVar, pos);

        uniformVar = glGetUniformLocation(prog->shader_positionalEncoding, "dim");
        glUniform1i(uniformVar, dim);

        uniformVar = glGetUniformLocation(prog->shader_positionalEncoding, "hidden_dim");
        glUniform1i(uniformVar, hidden_dim);

        uniformVar = glGetUniformLocation(prog->shader_positionalEncoding, "freq_cis_idx_delta");
        glUniform1i(uniformVar, freq_cis_idx_delta);

        uniformVar = glGetUniformLocation(prog->shader_positionalEncoding, "head_size");
        glUniform1i(uniformVar, head_size);

        glDispatchCompute(dim / 2, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

        // save key,value at this time step (pos) to our kv cache
        int loff = l * p->seq_len * dim;  // kv cache layer offset for convenience
        int key_offset = sizeof(float) * (loff + pos * dim);
        int value_offset = sizeof(float) * (loff + pos * dim);
        glCopyBufferSubData(s->k, s->key_cache, 0, key_offset, dim * sizeof(float));
        glCopyBufferSubData(s->v, s->value_cache, 0, value_offset, dim * sizeof(float));

        // multihead attention. iterate over all heads

        // get the query vector for this head
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s->q);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s->key_cache);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s->att);
        glUseProgram(prog->shader_transformer_get_query_vector);

        uniformVar = glGetUniformLocation(prog->shader_transformer_get_query_vector, "seq_len");
        glUniform1i(uniformVar, p->seq_len);

        uniformVar = glGetUniformLocation(prog->shader_transformer_get_query_vector, "pos");
        glUniform1i(uniformVar, pos);

        uniformVar = glGetUniformLocation(prog->shader_transformer_get_query_vector, "head_size");
        glUniform1i(uniformVar, head_size);

        uniformVar = glGetUniformLocation(prog->shader_transformer_get_query_vector, "dim");
        glUniform1i(uniformVar, dim);

        uniformVar = glGetUniformLocation(prog->shader_transformer_get_query_vector, "layer_idx");
        glUniform1i(uniformVar, l);

        glDispatchCompute(p->n_heads, head_size, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

        // softmax the scores to get attention weights, from 0..pos inclusively
        transformer_softmax(prog, s, s->att, pos + 1, p->seq_len, p->n_heads);

        // weighted sum of the values, store back into xb
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s->value_cache);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s->att);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, s->mulBuffer_4);
        glUseProgram(prog->shader_transformer_build_attMat);

        uniformVar = glGetUniformLocation(prog->shader_transformer_build_attMat, "seq_len");
        glUniform1i(uniformVar, p->seq_len);

        uniformVar = glGetUniformLocation(prog->shader_transformer_build_attMat, "pos");
        glUniform1i(uniformVar, pos);

        uniformVar = glGetUniformLocation(prog->shader_transformer_build_attMat, "head_size");
        glUniform1i(uniformVar, head_size);

        uniformVar = glGetUniformLocation(prog->shader_transformer_build_attMat, "dim");
        glUniform1i(uniformVar, dim);

        uniformVar = glGetUniformLocation(prog->shader_transformer_build_attMat, "layer_idx");
        glUniform1i(uniformVar, l);

        glDispatchCompute(p->n_heads, head_size, pos + 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

        transformer_sum(prog, s, s->xb, s->mulBuffer_4, pos + 1, head_size, p->n_heads);

        // final matmul to get the output of the attention
        matmul(prog, s, s->xb2, s->xb, w->wo, dim, dim, 0, l * dim * dim);

        // residual connection back into x
        accum(prog, s, x, s->xb2, dim);

        // ffn rmsnorm
        rmsnorm(prog, s, s->xb, x, w->rms_ffn_weight, dim, l * dim);

        // Now for FFN in PyTorch we have: self.w2(F.silu(self.w1(x)) * self.w3(x))
        // first calculate self.w1(x) and self.w3(x)
        matmul(prog, s, s->hb, s->xb, w->w1, dim, hidden_dim, 0, l * dim * hidden_dim);
        matmul(prog, s, s->hb2, s->xb, w->w3, dim, hidden_dim, 0, l * dim * hidden_dim);

        // 1. F.silu; silu(x)=x*σ(x),where σ(x) is the logistic sigmoid
        // 2. elementwise multiply with w3(x)
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, s->hb);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, s->hb2);
        glUseProgram(prog->shader_transformer_silu_and_mulW3);
        glDispatchCompute(hidden_dim, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();

        // final matmul to get the output of the ffn
        matmul(prog, s, s->xb, s->hb, w->w2, hidden_dim, dim, 0, l * dim * hidden_dim);

        // residual connection
        accum(prog, s, x, s->xb, dim);
    }

    // final rmsnorm
    rmsnorm(prog, s, x, x, w->rms_final_weight, dim, 0);

    // classifier into logits
    matmul(prog, s, s->logits, x, w->wcls, p->dim, p->vocab_size, 0, 0);
}

// ----------------------------------------------------------------------------
// byte pair encoding (BPE) tokenizer, encodes strings into tokens so we can prompt

int str_lookup(char* str, char** vocab, int vocab_size) {
    // find the first perfect match for str in vocab, return its index or -1 if not found
    for (int i = 0; i < vocab_size; i++) {
        if (strcmp(str, vocab[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void bpe_encode(char* text, char** vocab, float* vocab_scores, int vocab_size, unsigned int max_token_length, int* tokens, int* n_tokens) {
    // a temporary buffer to merge two consecutive tokens
    char* str_buffer = (char*)malloc((max_token_length * 2 + 1) * sizeof(char));  // *2 for concat, +1 for null terminator

    // first encode every individual byte in the input string
    *n_tokens = 0;  // the number of tokens
    for (char* c = text; *c != '\0'; c++) {
        sprintf(str_buffer, "%c", *c);
        int id = str_lookup(str_buffer, vocab, vocab_size);
        if (id == -1) {
            fprintf(stderr, "not good\n");
            exit(EXIT_FAILURE);
        }
        tokens[*n_tokens] = id;
        (*n_tokens)++;
    }

    // merge the best consecutive pair each iteration, according the scores in vocab_scores
    while (1) {
        float best_score = -1e10;
        int best_id = -1;
        int best_idx = -1;

        for (int i = 0; i < (*n_tokens - 1); i++) {
            // check if we can merge the pair (tokens[i], tokens[i+1])
            sprintf(str_buffer, "%s%s", vocab[tokens[i]], vocab[tokens[i + 1]]);
            int id = str_lookup(str_buffer, vocab, vocab_size);
            if (id != -1 && vocab_scores[id] > best_score) {
                // this merge pair exists in vocab! record its score and position
                best_score = vocab_scores[id];
                best_id = id;
                best_idx = i;
            }
        }

        if (best_idx == -1) {
            break;  // we couldn't find any more pairs to merge, so we're done
        }

        // merge the consecutive pair (best_idx, best_idx+1) into new token best_id
        tokens[best_idx] = best_id;
        // delete token at position best_idx+1, shift the entire sequence back 1
        for (int i = best_idx + 1; i < (*n_tokens - 1); i++) {
            tokens[i] = tokens[i + 1];
        }
        (*n_tokens)--;  // token length decreased
    }

    free(str_buffer);
}

// ----------------------------------------------------------------------------
// utilities: time / rng

long time_in_ms() {
    // return time in milliseconds, for benchmarking the model speed
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

unsigned long long rng_seed;
unsigned int random_u32() {
    // xorshift rng: https://en.wikipedia.org/wiki/Xorshift#xorshift.2A
    rng_seed ^= rng_seed >> 12;
    rng_seed ^= rng_seed << 25;
    rng_seed ^= rng_seed >> 27;
    return (rng_seed * 0x2545F4914F6CDD1Dull) >> 32;
}
float random_f32() {  // random float32 in [0,1)
    return (random_u32() >> 8) / 16777216.0f;
}

// ----------------------------------------------------------------------------
// sampling can be done in a few ways: greedy argmax, sampling, top-p sampling

int argmax(GPUProgram* prog, RunState* state, GLuint probabilities, int n) {
    // return the index that has the highest probability

    int currentStepSize = n;
    int nextStepSize = currentStepSize / 2;

    GLuint currentBuffer_v = state->mulBuffer_1;
    GLuint currentBuffer_i = state->mulBuffer_2;
    GLuint nextBuffer_v = state->mulBuffer_3;
    GLuint nextBuffer_i = state->mulBuffer_4;
    GLuint tmp;

    //init index
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, probabilities);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, currentBuffer_i);
    glUseProgram(prog->shader_argmax_set_index);
    int uniformVar = glGetUniformLocation(prog->shader_argmax_set_index, "insize");
    glUniform1i(uniformVar, n);
    glDispatchCompute(n, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();

    if (currentStepSize % 2 == 1) {
        nextStepSize += 1;
    }

    //first step
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, probabilities);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, currentBuffer_i);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, nextBuffer_v);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, nextBuffer_i);
    glUseProgram(prog->shader_rmsnorm_squares_and_sum);

    uniformVar = glGetUniformLocation(prog->shader_argmax, "insize");
    glUniform1i(uniformVar, currentStepSize);

    uniformVar = glGetUniformLocation(prog->shader_argmax, "shape0");
    glUniform1i(uniformVar, nextStepSize);

    glDispatchCompute(nextStepSize, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    GPU_CHECK();

    while (nextStepSize != 1) {
        //swap current and next
        tmp = currentBuffer_i;
        currentBuffer_i = nextBuffer_i;
        nextBuffer_i = tmp;

        tmp = currentBuffer_v;
        currentBuffer_v = nextBuffer_v;
        nextBuffer_v = tmp;

        currentStepSize = nextStepSize;
        nextStepSize = currentStepSize / 2;
        if (currentStepSize % 2 == 1) {
            nextStepSize += 1;
        }

        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, currentBuffer_v);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, currentBuffer_i);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, nextBuffer_v);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, nextBuffer_i);
        glUseProgram(prog->shader_rmsnorm_squares_and_sum);

        uniformVar = glGetUniformLocation(prog->shader_argmax, "insize");
        glUniform1i(uniformVar, currentStepSize);

        uniformVar = glGetUniformLocation(prog->shader_argmax, "shape0");
        glUniform1i(uniformVar, nextStepSize);

        glDispatchCompute(nextStepSize, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        GPU_CHECK();
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, nextBuffer_i);
    float* pOut = (float*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                           sizeof(float), GL_MAP_READ_BIT);
    GPU_CHECK();
    int res = -1;
    if (pOut) {
        res = pOut[0];
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    return res;
}

int sample(GPUProgram* prog, RunState* state, GLuint probabilities_gpu, int n) {
    // sample index from probabilities (they must sum to 1!)
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, probabilities_gpu);
    float* probabilities = (float*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                                    n * sizeof(float), GL_MAP_READ_BIT);
    GPU_CHECK();
    int res = n - 1;
    if (probabilities) {
        float r = random_f32();
        float cdf = 0.0f;
        for (int i = 0; i < n; i++) {
            cdf += probabilities[i];
            if (r < cdf) {
                res = i;
                break;
            }
        }
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    return res;
}

int compare(const void* a, const void* b) {
    ProbIndex* a_ = (ProbIndex*)a;
    ProbIndex* b_ = (ProbIndex*)b;
    if (a_->prob > b_->prob)
        return -1;
    if (a_->prob < b_->prob)
        return 1;
    return 0;
}

int sample_topp(GPUProgram* prog, RunState* state, GLuint probabilities_gpu, int n, float topp, ProbIndex* probindex) {
    // top-p sampling (or "nucleus sampling") samples from the smallest set of
    // tokens that exceed probability topp. This way we never sample tokens that
    // have very low probabilities and are less likely to go "off the rails".

    // quicksort indices in descending order of probabilities
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, probabilities_gpu);
    float* probabilities = (float*)glMapBufferRange(GL_SHADER_STORAGE_BUFFER, 0,
                                                    n * sizeof(float), GL_MAP_READ_BIT);
    GPU_CHECK();
    int res = n - 1;
    if (probabilities) {
        for (int i = 0; i < n; i++) {
            probindex[i].index = i;
            probindex[i].prob = probabilities[i];
        }
        qsort(probindex, n, sizeof(ProbIndex), compare);

        // truncate the list where cumulative probability exceeds topp
        float cumulative_prob = 0.0f;
        int last_idx = 0;
        for (int i = 0; i < n; i++) {
            cumulative_prob += probindex[i].prob;
            if (cumulative_prob > topp) {
                last_idx = i;
                break;  // we've exceeded topp by including last_idx
            }
        }

        // sample from the truncated list
        float r = random_f32() * cumulative_prob;
        float cdf = 0.0f;
        for (int i = 0; i <= last_idx; i++) {
            cdf += probindex[i].prob;
            if (r < cdf) {
                res = probindex[i].index;
                break;
            }
        }
        res = probindex[last_idx].index;  // in case of rounding errors
    }
    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);

    return res;
}

// ----------------------------------------------------------------------------
// int main

void error_usage() {
    fprintf(stderr, "Usage:   run_gpu <checkpoint> [options]\n");
    fprintf(stderr, "Example: run_gpu model.bin -n 256 -i \"Once upon a time\"\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -t <float>  temperature, default 1.0\n");
    fprintf(stderr, "  -p <float>  p value in top-p (nucleus) sampling. default 0.9, 0 = off\n");
    fprintf(stderr, "  -s <int>    random seed, default time(NULL)\n");
    fprintf(stderr, "  -n <int>    number of steps to run for, default 256. 0 = max_seq_len\n");
    fprintf(stderr, "  -i <string> input prompt\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char* argv[]) {
    // default inits
    char* checkpoint = NULL;              // e.g. out/model.bin
    float temperature = 1.0f;             // 0.0 = greedy deterministic. 1.0 = original. don't set higher
    float topp = 0.9f;                    // top-p in nucleus sampling
    rng_seed = (unsigned int)time(NULL);  // seed rng with time by default
    int steps = 256;                      // number of steps to run for
    char* prompt = NULL;                  // prompt string

    // poor man's C argparse so we can override the defaults above from the command line
    if (argc >= 2) {
        checkpoint = argv[1];
    } else {
        error_usage();
    }
    for (int i = 2; i < argc; i += 2) {
        // do some basic validation
        if (i + 1 >= argc) {
            error_usage();
        }  // must have arg after flag
        if (argv[i][0] != '-') {
            error_usage();
        }  // must start with dash
        if (strlen(argv[i]) != 2) {
            error_usage();
        }  // must be -x (one dash, one letter)
        // read in the args
        if (argv[i][1] == 't') {
            temperature = atof(argv[i + 1]);
        } else if (argv[i][1] == 'p') {
            topp = atof(argv[i + 1]);
        } else if (argv[i][1] == 's') {
            rng_seed = atoi(argv[i + 1]);
        } else if (argv[i][1] == 'n') {
            steps = atoi(argv[i + 1]);
        } else if (argv[i][1] == 'i') {
            prompt = argv[i + 1];
        } else {
            error_usage();
        }
    }
    if (rng_seed == 0) {
        fprintf(stderr, "Cannot use seed=0 because of the rng alg used\n");
        return 1;
    }

    // read in the model.bin file
    GPUContext context;
    create_GPUContext(&context);
    Config config;
    TransformerWeights_local weights;
    int fd = 0;          // file descriptor for memory mapping
    float* data = NULL;  // memory mapped data pointer
    ssize_t file_size;   // size of the checkpoint file in bytes
    {
        FILE* file = fopen(checkpoint, "rb");
        if (!file) {
            fprintf(stderr, "Couldn't open file %s\n", checkpoint);
            return 1;
        }
        // read in the config header
        if (fread(&config, sizeof(Config), 1, file) != 1) {
            return 1;
        }
        // negative vocab size is hacky way of signaling unshared weights. bit yikes.
        int shared_weights = config.vocab_size > 0 ? 1 : 0;
        config.vocab_size = abs(config.vocab_size);
        // figure out the file size
        fseek(file, 0, SEEK_END);  // move file pointer to end of file
        file_size = ftell(file);   // get the file size, in bytes
        fclose(file);
        // memory map the Transformer weights into the data pointer
        fd = open(checkpoint, O_RDONLY);  // open in read only mode
        if (fd == -1) {
            fprintf(stderr, "open failed!\n");
            return 1;
        }
        data = (float*)mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            fprintf(stderr, "mmap failed!\n");
            return 1;
        }
        float* weights_ptr = data + sizeof(Config) / sizeof(float);
        checkpoint_init_weights(&weights, &config, weights_ptr, shared_weights);
    }
    // right now we cannot run for more than config.seq_len steps
    if (steps <= 0 || steps > config.seq_len) {
        steps = config.seq_len;
    }

    // read in the tokenizer.bin file
    char** vocab = (char**)malloc(config.vocab_size * sizeof(char*));
    float* vocab_scores = (float*)malloc(config.vocab_size * sizeof(float));
    unsigned int max_token_length;
    {
        FILE* file = fopen("tokenizer.bin", "rb");
        if (!file) {
            fprintf(stderr, "couldn't load tokenizer.bin\n");
            return 1;
        }
        if (fread(&max_token_length, sizeof(int), 1, file) != 1) {
            fprintf(stderr, "failed read\n");
            return 1;
        }
        int len;
        for (int i = 0; i < config.vocab_size; i++) {
            if (fread(vocab_scores + i, sizeof(float), 1, file) != 1) {
                fprintf(stderr, "failed read\n");
                return 1;
            }
            if (fread(&len, sizeof(int), 1, file) != 1) {
                fprintf(stderr, "failed read\n");
                return 1;
            }
            vocab[i] = (char*)malloc(len + 1);
            if (fread(vocab[i], len, 1, file) != 1) {
                fprintf(stderr, "failed read\n");
                return 1;
            }
            vocab[i][len] = '\0';  // add the string terminating token
        }
        fclose(file);
    }

    // create and init the application RunState
    GPUProgram prog;
    compile_GPUProgram(&prog);
    TransformerWeights_gpu weights_remote;
    upload_weights(&weights, &weights_remote, &config);
    RunState state;
    malloc_run_state(&state, &config);

    // process the prompt, if any
    int* prompt_tokens = NULL;
    int num_prompt_tokens = 0;
    if (prompt != NULL) {
        prompt_tokens = (int*)malloc(strlen(prompt) * sizeof(int));
        bpe_encode(prompt, vocab, vocab_scores, config.vocab_size, max_token_length, prompt_tokens, &num_prompt_tokens);
    }

    // start the main loop
    long start = 0;  // used to time our code, only initialized after first iteration
    int next;        // will store the next token in the sequence
    int token = 1;   // init with token 1 (=BOS), as done in Llama-2 sentencepiece tokenizer
    int pos = 0;     // position in the sequence
    while (pos < steps) {
        // forward the transformer to get logits for the next token
        transformer(token, pos, &config, &prog, &state, &weights_remote);

        // advance the state state machine
        if (pos < num_prompt_tokens) {
            // if we are still processing the input prompt, force the next prompt token
            next = prompt_tokens[pos];
        } else {
            // sample the next token
            if (temperature == 0.0f) {
                // greedy argmax sampling: take the token with the highest probability
                next = argmax(&prog, &state, state.logits, config.vocab_size);
            } else {
                // apply the temperature to the logits
                glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, state.logits);
                glUseProgram(prog.shader_temperature);
                int temperature_gpu = glGetUniformLocation(prog.shader_temperature, "temperature");
                glUniform1f(temperature_gpu, temperature);
                glDispatchCompute(config.vocab_size, 1, 1);
                glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
                GPU_CHECK();

                // apply softmax to the logits to get the probabilities for next token
                softmax(&prog, &state, state.logits, config.vocab_size, 0);
                // we sample from this distribution to get the next token
                if (topp <= 0) {
                    // simply sample from the predicted probability distribution
                    next = sample(&prog, &state, state.logits, config.vocab_size);
                } else {
                    // top-p (nucleus) sampling, clamping the least likely tokens to zero
                    next = sample_topp(&prog, &state, state.logits, config.vocab_size, topp, state.probindex);
                }
            }
        }
        pos++;

        // data-dependent terminating condition: the BOS (1) token delimits sequences
        if (next == 1) {
            break;
        }

        // following BOS (1) token, sentencepiece decoder strips any leading whitespace (see PR #89)
        char* token_str = (token == 1 && vocab[next][0] == ' ') ? vocab[next] + 1 : vocab[next];
        printf("%s", token_str);
        fflush(stdout);
        token = next;

        // init the timer here because the first iteration can be slower
        if (start == 0) {
            start = time_in_ms();
        }
    }
    printf("\n");

    // report achieved tok/s (pos-1 because the timer starts after first iteration)
    if (pos > 1) {
        long end = time_in_ms();
        fprintf(stderr, "achieved tok/s: %f\n", (pos - 1) / (double)(end - start) * 1000);
    }

    // memory and file handles cleanup
    free_run_state(&state);
    free_gpu_weight(&weights_remote);
    free_gpu_program(&prog);
    for (int i = 0; i < config.vocab_size; i++) {
        free(vocab[i]);
    }
    free(vocab);
    free(vocab_scores);
    if (prompt_tokens != NULL)
        free(prompt_tokens);
    if (data != MAP_FAILED)
        munmap(data, file_size);
    if (fd != -1)
        close(fd);
    release_GPUContext(&context);
    return 0;
}
