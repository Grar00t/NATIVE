/* niyah_core.c — NIYAH Inference Engine v3.0 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "niyah_core.h"
#include "niyah_publish_internal.h"
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <stdint.h>
#include <limits.h>

#if defined(__AVX2__) && defined(__FMA__)
#  include <immintrin.h>
#  define SIMD_AVX2 1
#elif defined(__ARM_NEON)
#  include <arm_neon.h>
#  define SIMD_NEON 1
#endif

typedef char _cfg_size_check[(sizeof(NiyahConfig) == 64) ? 1 : -1];

typedef struct {
    double weight;
    uint32_t token_id;
} NiyahSampleCandidate;

const char *niyah_simd_name(void) {
#if defined(SIMD_AVX2)
    return "AVX2+FMA";
#elif defined(SIMD_NEON)
    return "NEON";
#else
    return "Scalar";
#endif
}

static size_t checked_add(size_t a, size_t b) {
    if (b > SIZE_MAX - a) return SIZE_MAX;
    return a + b;
}
static size_t checked_mul(size_t a, size_t b) {
    if (a && b > SIZE_MAX / a) return SIZE_MAX;
    return a * b;
}

static void matvec(float * restrict y,const float * restrict A,const float * restrict x,size_t R,size_t C) {
#if defined(SIMD_AVX2)
    for (size_t r=0;r<R;r++) { const float *row=A+r*C; __m256 a0=_mm256_setzero_ps(),a1=_mm256_setzero_ps(); size_t c=0;
        for(;c+15<C;c+=16){a0=_mm256_fmadd_ps(_mm256_loadu_ps(row+c),_mm256_loadu_ps(x+c),a0);a1=_mm256_fmadd_ps(_mm256_loadu_ps(row+c+8),_mm256_loadu_ps(x+c+8),a1);} __m256 acc=_mm256_add_ps(a0,a1);__m128 lo=_mm256_castps256_ps128(acc),hi=_mm256_extractf128_ps(acc,1),s4=_mm_add_ps(lo,hi),s2=_mm_add_ps(s4,_mm_movehdup_ps(s4));float dot=_mm_cvtss_f32(_mm_add_ss(s2,_mm_movehl_ps(s2,s2)));for(;c<C;c++)dot+=row[c]*x[c];y[r]=dot; }
#elif defined(SIMD_NEON)
    for (size_t r=0;r<R;r++){const float*row=A+r*C;float32x4_t a0=vdupq_n_f32(0.f),a1=vdupq_n_f32(0.f);size_t c=0;for(;c+7<C;c+=8){a0=vfmaq_f32(a0,vld1q_f32(row+c),vld1q_f32(x+c));a1=vfmaq_f32(a1,vld1q_f32(row+c+4),vld1q_f32(x+c+4));}float32x4_t acc=vaddq_f32(a0,a1);float32x2_t lo2=vadd_f32(vget_low_f32(acc),vget_high_f32(acc));float dot=vget_lane_f32(vpadd_f32(lo2,lo2),0);for(;c<C;c++)dot+=row[c]*x[c];y[r]=dot;}
#else
    for (size_t r=0;r<R;r++){const float*row=A+r*C;float s0=0.f,s1=0.f,s2=0.f,s3=0.f;size_t c=0;for(;c+3<C;c+=4){s0+=row[c]*x[c];s1+=row[c+1]*x[c+1];s2+=row[c+2]*x[c+2];s3+=row[c+3]*x[c+3];}float dot=s0+s1+s2+s3;for(;c<C;c++)dot+=row[c]*x[c];y[r]=dot;}
#endif
}

void niyah_matvec_f32(float * restrict y,
                      const float * restrict A,
                      const float * restrict x,
                      size_t rows, size_t cols) {
    matvec(y, A, x, rows, cols);
}

static float dot_f32(const float * restrict a,const float * restrict b,size_t n){
#if defined(SIMD_AVX2)
    __m256 a0=_mm256_setzero_ps(),a1=_mm256_setzero_ps();size_t i=0;for(;i+15<n;i+=16){a0=_mm256_fmadd_ps(_mm256_loadu_ps(a+i),_mm256_loadu_ps(b+i),a0);a1=_mm256_fmadd_ps(_mm256_loadu_ps(a+i+8),_mm256_loadu_ps(b+i+8),a1);}__m256 acc=_mm256_add_ps(a0,a1);__m128 lo=_mm256_castps256_ps128(acc),hi=_mm256_extractf128_ps(acc,1),s4=_mm_add_ps(lo,hi),s2=_mm_add_ps(s4,_mm_movehdup_ps(s4));float d=_mm_cvtss_f32(_mm_add_ss(s2,_mm_movehl_ps(s2,s2)));for(;i<n;i++)d+=a[i]*b[i];return d;
#elif defined(SIMD_NEON)
    float32x4_t a0=vdupq_n_f32(0.f),a1=vdupq_n_f32(0.f);size_t i=0;for(;i+7<n;i+=8){a0=vfmaq_f32(a0,vld1q_f32(a+i),vld1q_f32(b+i));a1=vfmaq_f32(a1,vld1q_f32(a+i+4),vld1q_f32(b+i+4));}float32x4_t acc=vaddq_f32(a0,a1);float32x2_t lo2=vadd_f32(vget_low_f32(acc),vget_high_f32(acc));float d=vget_lane_f32(vpadd_f32(lo2,lo2),0);for(;i<n;i++)d+=a[i]*b[i];return d;
#else
    float d=0.f;for(size_t i=0;i<n;i++)d+=a[i]*b[i];return d;
#endif
}

static void axpy_f32(float * restrict dst,const float * restrict src,float scale,size_t n){
#if defined(SIMD_AVX2)
    __m256 s=_mm256_set1_ps(scale);size_t i=0;for(;i+15<n;i+=16){__m256 d0=_mm256_loadu_ps(dst+i),d1=_mm256_loadu_ps(dst+i+8);d0=_mm256_fmadd_ps(s,_mm256_loadu_ps(src+i),d0);d1=_mm256_fmadd_ps(s,_mm256_loadu_ps(src+i+8),d1);_mm256_storeu_ps(dst+i,d0);_mm256_storeu_ps(dst+i+8,d1);}for(;i<n;i++)dst[i]+=scale*src[i];
#elif defined(SIMD_NEON)
    float32x4_t s=vdupq_n_f32(scale);size_t i=0;for(;i+7<n;i+=8){float32x4_t d0=vld1q_f32(dst+i),d1=vld1q_f32(dst+i+4);d0=vfmaq_f32(d0,s,vld1q_f32(src+i));d1=vfmaq_f32(d1,s,vld1q_f32(src+i+4));vst1q_f32(dst+i,d0);vst1q_f32(dst+i+4,d1);}for(;i<n;i++)dst[i]+=scale*src[i];
#else
    for(size_t i=0;i<n;i++)dst[i]+=scale*src[i];
#endif
}

static void rmsnorm(float * restrict out,const float * restrict x,const float * restrict w,size_t n,float eps){
    if (n == 0u) return;
    const float ss = dot_f32(x, x, n);
    if (!isfinite(ss)) {
        double squares = 0;
        for (size_t k = 0; k < n; ++k) squares += (double)x[k] * x[k];
        const double inv = 1.0 / sqrt(squares / (double)n + eps);
        for (size_t k = 0; k < n; ++k) out[k] = (float)((double)x[k] * inv * w[k]);
        return;
    }
    const float scale = 1.0f / sqrtf(ss / (float)n + eps);
    for (size_t k = 0; k < n; k++) out[k] = x[k] * scale * w[k];
}
static inline float silu(float v){return v/(1.0f+expf(-v));}
static void rope(float*qk,uint32_t pos,uint32_t head_dim,float theta){for(uint32_t i=0;i+1<head_dim;i+=2){float angle=(float)pos/powf(theta,(float)i/(float)head_dim);float c=cosf(angle),s=sinf(angle),v0=qk[i],v1=qk[i+1];qk[i]=v0*c-v1*s;qk[i+1]=v0*s+v1*c;}}

static size_t weight_count(const NiyahConfig *c)
{
    const size_t d = c->embed_dim;
    const size_t kv = (d / c->n_heads) * c->n_kv_heads;
    const size_t f = checked_mul(d, c->ffn_mult);
    size_t layer = checked_mul(2, checked_mul(d, d));   /* query and output */
    layer = checked_add(layer, checked_mul(2, checked_mul(kv, d)));
    layer = checked_add(layer, checked_mul(3, checked_mul(f, d)));
    layer = checked_add(layer, checked_mul(2, d));
    const size_t embeddings = checked_mul(2, checked_mul(c->vocab_size, d));
    return checked_add(checked_mul(c->n_layers, layer), checked_add(embeddings, d));
}

size_t niyah_param_count(const NiyahModel *model)
{
    return model ? weight_count(&model->cfg) : 0;
}

size_t niyah_sample_workspace_bytes(uint32_t vocab_size) {
    if (vocab_size == 0u || SIZE_MAX / vocab_size < sizeof(NiyahSampleCandidate)) return 0u;
    return (size_t)vocab_size * sizeof(NiyahSampleCandidate);
}

typedef struct {
    size_t weights;
    size_t cache_per_side;
    size_t layer_offset;
    size_t pool_bytes;
    size_t sampler_bytes;
} ModelLayout;

static int model_layout(const NiyahConfig *c, ModelLayout *layout)
{
    if (!c || !c->embed_dim || !c->n_heads || !c->n_layers || !c->n_kv_heads ||
        c->n_kv_heads > c->n_heads || c->embed_dim % c->n_heads ||
        !c->ffn_mult || c->embed_dim > UINT32_MAX / c->ffn_mult ||
        !c->ctx_len || c->ctx_len > NIYAH_MAX_CTX ||
        !c->vocab_size || c->vocab_size > NIYAH_MAX_VOCAB) return 0;

    const size_t d = c->embed_dim;
    const size_t f = d * c->ffn_mult;
    const size_t kv = (d / c->n_heads) * c->n_kv_heads;
    const size_t attention = checked_mul(c->n_heads, c->ctx_len);
    size_t scratch = checked_add(checked_mul(6, d), checked_mul(2, f));
    scratch = checked_add(scratch, checked_add(attention, c->vocab_size));

    layout->weights = weight_count(c);
    layout->cache_per_side = checked_mul(checked_mul(c->n_layers, c->ctx_len), kv);
    size_t floats = checked_add(layout->weights, checked_mul(2, layout->cache_per_side));
    size_t bytes = checked_mul(checked_add(floats, scratch), sizeof(float));
    const size_t align = _Alignof(NiyahLayer);
    layout->layer_offset = checked_add(bytes, (align - bytes % align) % align);
    layout->sampler_bytes = niyah_sample_workspace_bytes(c->vocab_size);
    size_t layers = checked_mul(c->n_layers, sizeof(NiyahLayer));
    layout->pool_bytes = checked_add(layout->layer_offset, checked_add(layers, layout->sampler_bytes));
    return layout->pool_bytes <= PTRDIFF_MAX && layout->sampler_bytes != 0;
}

static float *take_floats(float **cursor, size_t count)
{
    float *start = *cursor;
    *cursor += count;
    return start;
}

static NiyahModel *alloc_model(const NiyahConfig *c, const ModelLayout *layout)
{
    NiyahModel *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->_pool = calloc(1, layout->pool_bytes);
    if (!m->_pool) { free(m); return NULL; }
    m->cfg = *c;
    m->_pool_bytes = layout->pool_bytes;
    m->head_dim = c->embed_dim / c->n_heads;
    m->kv_dim = c->n_kv_heads * m->head_dim;
    m->ffn_dim = c->embed_dim * c->ffn_mult;
    m->layers = (NiyahLayer *)((unsigned char *)m->_pool + layout->layer_offset);
    m->_sampler_workspace = m->layers + c->n_layers;
    m->_sampler_workspace_bytes = layout->sampler_bytes;

    const size_t d = c->embed_dim, kv = m->kv_dim, f = m->ffn_dim;
    float *p = m->_pool;
    for (uint32_t l = 0; l < c->n_layers; ++l) {
        NiyahLayer *layer = &m->layers[l];
        layer->wq = take_floats(&p, d * d);
        layer->wk = take_floats(&p, kv * d);
        layer->wv = take_floats(&p, kv * d);
        layer->wo = take_floats(&p, d * d);
        layer->w_gate = take_floats(&p, f * d);
        layer->w_up = take_floats(&p, f * d);
        layer->w_down = take_floats(&p, d * f);
        layer->rms_att = take_floats(&p, d);
        layer->rms_ffn = take_floats(&p, d);
    }
    m->token_embed = take_floats(&p, c->vocab_size * d);
    m->rms_final = take_floats(&p, d);
    m->lm_head = take_floats(&p, c->vocab_size * d);
    assert((size_t)(p - (float *)m->_pool) == layout->weights);
    m->kv_k = take_floats(&p, layout->cache_per_side);
    m->kv_v = take_floats(&p, layout->cache_per_side);
    m->scratch = p;
    m->_logits = p + 6 * d + 2 * f + (size_t)c->n_heads * c->ctx_len;
    return m;
}

NiyahModel *niyah_alloc(const NiyahConfig *cfg)
{
    ModelLayout layout;
    return model_layout(cfg, &layout) ? alloc_model(cfg, &layout) : NULL;
}

void niyah_free(NiyahModel *m)
{
    if (m) { free(m->_pool); free(m); }
}

int niyah_save(const NiyahModel *model, const char *path)
{
    ModelLayout layout;
    if (!model || !model->_pool || !path || !model_layout(&model->cfg, &layout) ||
        layout.pool_bytes != model->_pool_bytes) return -1;
    NiyahPublication stage = {0};
    FILE *file = niyah_atomic_open(path, &stage);
    if (!file) return -1;
    NiyahConfig header = model->cfg;
    header.magic = NIYAH_MAGIC;
    header.version = NIYAH_VER;
    int written = fwrite(&header, sizeof(header), 1, file) == 1 &&
                  fwrite(model->_pool, sizeof(float), layout.weights, file) == layout.weights;
    return niyah_atomic_finish(file, &stage, path, written ? NIYAH_OK : NIYAH_ERR_IO) == NIYAH_OK ? 0 : -1;
}

/* Reject an inconsistent regular file before allocating its advertised model.
   Streams receive the same length check through fread and the final EOF. */
static int file_fits_model(FILE *file, size_t expected)
{
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(file), &st)) return 0;
    if ((st.st_mode & _S_IFMT) != _S_IFREG) return 1;
#else
    struct stat st;
    if (fstat(fileno(file), &st)) return 0;
    if (!S_ISREG(st.st_mode)) return 1;
#endif
    return st.st_size >= 0 && (uintmax_t)st.st_size == expected;
}

int niyah_load(NiyahModel **out, const char *path)
{
    if (!out) return -1;
    *out = NULL;
    if (!path) return -1;
    FILE *file = niyah_file_open(path, "rb");
    if (!file) return -1;
    int rc = -1;
    NiyahConfig cfg;
    ModelLayout layout;
    NiyahModel *m = NULL;
    if (fread(&cfg, sizeof(cfg), 1, file) != 1 || cfg.magic != NIYAH_MAGIC) goto done;
    if (cfg.version != NIYAH_VER) { rc = -2; goto done; }
    if (!model_layout(&cfg, &layout) ||
        !file_fits_model(file, sizeof(cfg) + layout.weights * sizeof(float))) goto done;
    m = alloc_model(&cfg, &layout);
    if (!m || fread(m->_pool, sizeof(float), layout.weights, file) != layout.weights ||
        fgetc(file) != EOF || ferror(file)) goto done;
    rc = 0;
done:
    if (fclose(file)) rc = -1;
    if (rc == 0) *out = m;
    else niyah_free(m);
    return rc;
}

#define SCR_X(m)((m)->scratch)
#define SCR_XB(m)((m)->scratch+(m)->cfg.embed_dim)
#define SCR_XB2(m)((m)->scratch+(size_t)2*(m)->cfg.embed_dim)
#define SCR_HB(m)((m)->scratch+(size_t)3*(m)->cfg.embed_dim)
#define SCR_HB2(m)((m)->scratch+(size_t)3*(m)->cfg.embed_dim+(m)->ffn_dim)
#define SCR_Q(m)((m)->scratch+(size_t)3*(m)->cfg.embed_dim+(size_t)2*(m)->ffn_dim)
#define SCR_K(m)((m)->scratch+(size_t)4*(m)->cfg.embed_dim+(size_t)2*(m)->ffn_dim)
#define SCR_V(m)((m)->scratch+(size_t)4*(m)->cfg.embed_dim+(size_t)2*(m)->ffn_dim+(m)->kv_dim)
#define SCR_ATT(m)((m)->scratch+(size_t)4*(m)->cfg.embed_dim+(size_t)2*(m)->ffn_dim+(size_t)2*(m)->kv_dim)

float*niyah_forward(NiyahModel*m,uint32_t token,uint32_t pos){if(!m||!m->_pool||token>=m->cfg.vocab_size||pos>=m->cfg.ctx_len)return NULL;const NiyahConfig*c=&m->cfg;uint32_t d=c->embed_dim,hd=m->head_dim,nh=c->n_heads,nkv=c->n_kv_heads,xctx=c->ctx_len;float*x=SCR_X(m),*xb=SCR_XB(m),*xb2=SCR_XB2(m),*hb=SCR_HB(m),*hb2=SCR_HB2(m),*q=SCR_Q(m),*k=SCR_K(m),*v=SCR_V(m),*att=SCR_ATT(m);memcpy(x,m->token_embed+(size_t)token*d,d*sizeof(float));for(uint32_t l=0;l<c->n_layers;l++){const NiyahLayer*lw=&m->layers[l];rmsnorm(xb,x,lw->rms_att,d,c->rms_eps);matvec(q,lw->wq,xb,d,d);matvec(k,lw->wk,xb,m->kv_dim,d);matvec(v,lw->wv,xb,m->kv_dim,d);for(uint32_t h=0;h<nh;h++)rope(q+h*hd,pos,hd,c->rope_theta);for(uint32_t h=0;h<nkv;h++)rope(k+h*hd,pos,hd,c->rope_theta);size_t L=(size_t)nkv*xctx*hd;float*kc=m->kv_k+(size_t)l*L,*vc=m->kv_v+(size_t)l*L;for(uint32_t h=0;h<nkv;h++){float*dk=kc+(size_t)h*xctx*hd+(size_t)pos*hd;float*dv=vc+(size_t)h*xctx*hd+(size_t)pos*hd;memcpy(dk,k+h*hd,hd*sizeof(float));memcpy(dv,v+h*hd,hd*sizeof(float));}memset(att,0,(size_t)nh*xctx*sizeof(float));for(uint32_t h=0;h<nh;h++){uint32_t kvh=(h*nkv)/nh;float*ah=att+(size_t)h*xctx;for(uint32_t t=0;t<=pos;t++){float score=dot_f32(q+h*hd,kc+(size_t)kvh*xctx*hd+(size_t)t*hd,hd)/(sqrtf((float)hd));if(score>80.f)score=80.f;if(score<-80.f)score=-80.f;ah[t]=score;}float maxs=ah[0];for(uint32_t t=1;t<=pos;t++)if(ah[t]>maxs)maxs=ah[t];float den=0.f;for(uint32_t t=0;t<=pos;t++){ah[t]=expf(ah[t]-maxs);den+=ah[t];}den=den>0.f?den:1.f;for(uint32_t t=0;t<=pos;t++)ah[t]/=den;memset(xb2+h*hd,0,hd*sizeof(float));for(uint32_t t=0;t<=pos;t++){const float*vv=vc+(size_t)kvh*xctx*hd+(size_t)t*hd;axpy_f32(xb2+h*hd,vv,ah[t],hd);}}matvec(xb,lw->wo,xb2,d,d);for(uint32_t i=0;i<d;i++)x[i]+=xb[i];rmsnorm(xb,x,lw->rms_ffn,d,c->rms_eps);matvec(hb,lw->w_gate,xb,m->ffn_dim,d);matvec(hb2,lw->w_up,xb,m->ffn_dim,d);for(uint32_t i=0;i<m->ffn_dim;i++)hb[i]=silu(hb[i])*hb2[i];matvec(xb,lw->w_down,hb,d,m->ffn_dim);for(uint32_t i=0;i<d;i++)x[i]+=xb[i];}rmsnorm(xb,x,m->rms_final,d,c->rms_eps);matvec(m->_logits,m->lm_head,xb,c->vocab_size,d);return m->_logits;}

/* Stable descending probability order; equal scores use vocabulary ID as tiebreak. */
static int niyah_sample_candidate_compare(const void *lhs, const void *rhs)
{
    const NiyahSampleCandidate *a = (const NiyahSampleCandidate *)lhs;
    const NiyahSampleCandidate *b = (const NiyahSampleCandidate *)rhs;
    if (a->weight > b->weight) return -1;
    if (a->weight < b->weight) return 1;
    if (a->token_id < b->token_id) return -1;
    if (a->token_id > b->token_id) return 1;
    return 0;
}

/* Nucleus sampling: sort probabilities, choose the smallest prefix whose
 * cumulative mass reaches top_p, then sample from that prefix's mass.
 * This is O(vocab log vocab), with O(vocab) temporary memory. */
uint32_t niyah_sample_with_workspace(const float *logits, uint32_t vocab_size,
                                    NiyahSampler *s, void *workspace,
                                    size_t workspace_bytes)
{
    uint32_t best = 0u;
    float max_logit;
    NiyahSampleCandidate *candidates;
    double total = 0.0, prefix_mass = 0.0, threshold, target, running;
    size_t nucleus_size = 0u;
    uint32_t chosen;

    if (!logits || !s || vocab_size == 0u ||
        !isfinite(s->temperature) || s->temperature < 0.0f ||
        !isfinite(s->top_p)) return 0u;

    /* Reject non-finite inputs rather than propagating NaN through softmax. */
    if (!isfinite(logits[0])) return 0u;
    max_logit = logits[0];
    for (uint32_t i = 1u; i < vocab_size; ++i) {
        if (!isfinite(logits[i])) return 0u;
        if (logits[i] > max_logit) {
            max_logit = logits[i];
            best = i;
        }
    }
    if (s->temperature == 0.0f) return best;

    if (!workspace || workspace_bytes < niyah_sample_workspace_bytes(vocab_size) ||
        ((uintptr_t)workspace % _Alignof(NiyahSampleCandidate)) != 0u) return 0u;
    candidates = (NiyahSampleCandidate *)workspace;

    for (uint32_t i = 0u; i < vocab_size; ++i) {
        const double z = ((double)logits[i] - (double)max_logit) /
                         (double)s->temperature;
        const double weight = exp(z);
        candidates[i].weight = weight;
        candidates[i].token_id = i;
        total += weight;
    }
    if (!(total > 0.0) || !isfinite(total)) return 0u;

    qsort(candidates, (size_t)vocab_size, sizeof(*candidates),
          niyah_sample_candidate_compare);
    /* Keep the legacy top_p <= 0 fallback: full-distribution sampling. */
    threshold = (double)((s->top_p > 0.0f && s->top_p < 1.0f)
                         ? s->top_p : 1.0f) * total;
    do {
        prefix_mass += candidates[nucleus_size].weight;
        ++nucleus_size;
    } while (nucleus_size < (size_t)vocab_size && prefix_mass < threshold);

    s->seed = s->seed * UINT64_C(6364136223846793005) +
              UINT64_C(1442695040888963407);
    /* 53 high bits map deterministically into [0, 1). */
    target = ((double)(s->seed >> 11) / 9007199254740992.0) * prefix_mass;
    running = 0.0;
    chosen = candidates[nucleus_size - 1u].token_id;
    for (size_t i = 0u; i < nucleus_size; ++i) {
        running += candidates[i].weight;
        if (target < running) {
            chosen = candidates[i].token_id;
            break;
        }
    }
    return chosen;
}

/* Compatibility wrapper: callers using the old API still work.
 * The generation hot path calls niyah_sample_with_workspace instead. */
uint32_t niyah_sample(const float *logits, uint32_t vocab_size, NiyahSampler *s) {
    uint32_t result;
    size_t needed;
    void *memory;
    if (!logits || !s || vocab_size == 0u) return 0u;
    if (s->temperature == 0.0f)
        return niyah_sample_with_workspace(logits, vocab_size, s, NULL, 0u);
    needed = niyah_sample_workspace_bytes(vocab_size);
    if (needed == 0u) return 0u;
    memory = malloc(needed);
    if (!memory) return 0u;
    result = niyah_sample_with_workspace(logits, vocab_size, s, memory, needed);
    free(memory);
    return result;
}

NiyahAdam *niyah_adam_alloc(const NiyahModel *m)
{
    if (!m) return NULL;
    NiyahAdam *opt = calloc(1, sizeof(*opt));
    if (!opt) return NULL;
    opt->n_weights = niyah_param_count(m);
    opt->m = calloc(opt->n_weights, sizeof(float));
    opt->v = calloc(opt->n_weights, sizeof(float));
    if (!opt->m || !opt->v) { niyah_adam_free(opt); return NULL; }
    opt->lr = 3e-4f;
    opt->beta1 = 0.9f;
    opt->beta2 = 0.999f;
    opt->eps = 1e-8f;
    opt->wd = 0.01f;
    return opt;
}
void niyah_adam_free(NiyahAdam*opt){if(!opt)return;free(opt->m);free(opt->v);free(opt);}

float niyah_train_step(NiyahModel*m,NiyahAdam*opt,const uint32_t*tokens,uint32_t n){if(!m||!opt||!tokens||n<2u)return 0.f;if(n > m->cfg.ctx_len + 1u)return NAN;for(uint32_t i=0;i<n;i++)if(tokens[i]>=m->cfg.vocab_size)return NAN;size_t nw=weight_count(&m->cfg);float *grad = calloc(nw, sizeof(float));
    float *dL = malloc((size_t)m->cfg.vocab_size * sizeof(float));
    if (!grad || !dL) {free(grad); free(dL); return NAN;}
    float loss=0.f;uint32_t d=m->cfg.embed_dim;for(uint32_t t=0;t+1<n;t++){const float*logits=niyah_forward(m,tokens[t],t);if(!logits){free(dL);free(grad);return NAN;}uint32_t tgt=tokens[t+1];float mx=logits[0];for(uint32_t i=1;i<m->cfg.vocab_size;i++)if(logits[i]>mx)mx=logits[i];float lse_sum=0.f;for(uint32_t i=0;i<m->cfg.vocab_size;i++){float z=logits[i]-mx;if(z<-80.f)z=-80.f;lse_sum+=expf(z);}if(!(lse_sum>0.f)||!isfinite(lse_sum)){free(dL);free(grad);return NAN;}float lse=logf(lse_sum)+mx;loss+=lse-logits[tgt];for(uint32_t i=0;i<m->cfg.vocab_size;i++)dL[i]=expf(logits[i]-lse);dL[tgt]-=1.f;const float*xb=SCR_XB(m);size_t off=nw-(size_t)m->cfg.vocab_size*d;float*dW=grad+off;for(uint32_t i=0;i<m->cfg.vocab_size;i++){float dl=dL[i];for(uint32_t j=0;j<d;j++)dW[(size_t)i*d+j]+=dl*xb[j];}}
    loss/=(float)(n-1u);opt->step++;float bc1=1.f-powf(opt->beta1,(float)opt->step),bc2=1.f-powf(opt->beta2,(float)opt->step);if(!(bc1>0.f&&bc2>0.f)){free(dL);free(grad);return NAN;}size_t off=nw-(size_t)m->cfg.vocab_size*d;float*W=(float*)m->_pool+off;const float*gW=grad+off;float*mW=opt->m+off,*vW=opt->v+off;size_t cnt=(size_t)m->cfg.vocab_size*d;for(size_t i=0;i<cnt;i++){float g=gW[i]+opt->wd*W[i];mW[i]=opt->beta1*mW[i]+(1.f-opt->beta1)*g;vW[i]=opt->beta2*vW[i]+(1.f-opt->beta2)*g*g;float mh=mW[i]/bc1,vh=vW[i]/bc2;if(!isfinite(mh)||!isfinite(vh)){free(dL);free(grad);return NAN;}W[i]-=opt->lr*mh/(sqrtf(vh)+opt->eps);}free(dL);free(grad);return isfinite(loss)?loss:NAN;}

/* Dynamic GGUF tensor execution (mmap weights, static runtime pool). */
#include "niyah_gguf_infer.inc"
