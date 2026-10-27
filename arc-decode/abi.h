/* ABI views derived from FFmpeg n6.0 and n8.0 public headers.
 * avcodec.h copyright (c) 2001 Fabrice Bellard.
 * Other FFmpeg files copyright their respective authors; LGPL-2.1-or-later.
 * See LICENSE at the repository root. These are modified, reduced ABI views, not upstream headers.
 * Source: https://github.com/FFmpeg/FFmpeg/tree/n6.0
 *         https://github.com/FFmpeg/FFmpeg/tree/n8.0
 * Only use with the runtime versions checked in bridge.c.
 * Allocations always belong to the library that created them.
 */
#ifndef ARC_ABI_H
#define ARC_ABI_H
#if !defined(__x86_64__) || __SIZEOF_POINTER__ != 8
#error "This experimental adapter requires the validated Linux x86-64 ABI"
#endif
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long uint64_t;
typedef long int64_t;
typedef unsigned long size_t;
#define AV_NUM_DATA_POINTERS 8
typedef struct { int num, den; } AVRational;
typedef struct { int order, nb_channels; union { uint64_t mask; void *map; } u; void *opaque; } AVChannelLayout;
typedef void AVClass;
typedef void AVBufferRef;
typedef void AVDictionary;
typedef struct { int type; uint8_t *data; size_t size; void *metadata, *buf; } SideData;
typedef struct { uint8_t *data; size_t size; int type; } PacketSideData;
typedef struct { void *buf; int64_t pts, dts; uint8_t *data; int size, stream_index, flags; PacketSideData *side_data; int side_data_elems; int64_t duration, pos; void *opaque, *opaque_ref; AVRational time_base; } Packet;
typedef struct Frame6 {


    uint8_t *data[AV_NUM_DATA_POINTERS];

    int linesize[AV_NUM_DATA_POINTERS];

    uint8_t **extended_data;

    int width, height;

    int nb_samples;

    int format;

    int key_frame;

    int pict_type;

    AVRational sample_aspect_ratio;

    int64_t pts;

    int64_t pkt_dts;

    AVRational time_base;

    
    int coded_picture_number;

    
    int display_picture_number;


    int quality;

    void *opaque;

    int repeat_pict;

    int interlaced_frame;

    int top_field_first;

    int palette_has_changed;

    
    int64_t reordered_opaque;


    int sample_rate;

    
    uint64_t channel_layout;


    AVBufferRef *buf[AV_NUM_DATA_POINTERS];

    AVBufferRef **extended_buf;

    int        nb_extended_buf;
    SideData **side_data;
    int            nb_side_data;





    int flags;

    int color_range;

    int color_primaries;

    int color_trc;

    int colorspace;
    int chroma_location;

    int64_t best_effort_timestamp;

    int64_t pkt_pos;

    
    int64_t pkt_duration;


    AVDictionary *metadata;

    int decode_error_flags;





    
    int channels;


    int pkt_size;

    AVBufferRef *hw_frames_ctx;

    AVBufferRef *opaque_ref;

    size_t crop_top;
    size_t crop_bottom;
    size_t crop_left;
    size_t crop_right;

    AVBufferRef *private_ref;

    AVChannelLayout ch_layout;

    int64_t duration;
} Frame6;
typedef struct Frame8 {


    uint8_t *data[AV_NUM_DATA_POINTERS];

    int linesize[AV_NUM_DATA_POINTERS];

    uint8_t **extended_data;

    int width, height;

    int nb_samples;

    int format;

    int pict_type;

    AVRational sample_aspect_ratio;

    int64_t pts;

    int64_t pkt_dts;

    AVRational time_base;

    int quality;

    void *opaque;

    int repeat_pict;

    int sample_rate;

    AVBufferRef *buf[AV_NUM_DATA_POINTERS];

    AVBufferRef **extended_buf;

    int        nb_extended_buf;
    SideData **side_data;
    int            nb_side_data;













    int flags;

    int color_range;

    int color_primaries;

    int color_trc;

    int colorspace;
    int chroma_location;

    int64_t best_effort_timestamp;

    AVDictionary *metadata;

    int decode_error_flags;





    AVBufferRef *hw_frames_ctx;

    AVBufferRef *opaque_ref;

    size_t crop_top;
    size_t crop_bottom;
    size_t crop_left;
    size_t crop_right;

    void *private_ref;

    AVChannelLayout ch_layout;

    int64_t duration;
} Frame8;
typedef struct Par6 {

    int codec_type;

    int   codec_id;

    uint32_t         codec_tag;

    uint8_t *extradata;

    int      extradata_size;

    int format;

    int64_t bit_rate;

    int bits_per_coded_sample;

    int bits_per_raw_sample;

    int profile;
    int level;

    int width;
    int height;

    AVRational sample_aspect_ratio;

    int                  field_order;

    int                  color_range;
    int              color_primaries;
    int color_trc;
    int                  color_space;
    int              chroma_location;

    int video_delay;

    
    uint64_t channel_layout;

    
    int      channels;


    int      sample_rate;

    int      block_align;

    int      frame_size;

    int initial_padding;

    int trailing_padding;

    int seek_preroll;

    AVChannelLayout ch_layout;
} Par6;
typedef struct Par8 {

    int codec_type;

    int   codec_id;

    uint32_t         codec_tag;

    uint8_t *extradata;

    int      extradata_size;

    PacketSideData *coded_side_data;

    int nb_coded_side_data;

    int format;

    int64_t bit_rate;

    int bits_per_coded_sample;

    int bits_per_raw_sample;

    int profile;
    int level;

    int width;
    int height;

    AVRational sample_aspect_ratio;

    AVRational framerate;

    int                  field_order;

    int                  color_range;
    int              color_primaries;
    int color_trc;
    int                  color_space;
    int              chroma_location;

    int video_delay;

    AVChannelLayout ch_layout;

    int      sample_rate;

    int      block_align;

    int      frame_size;

    int initial_padding;

    int trailing_padding;

    int seek_preroll;
} Par8;
typedef struct Ctx6 {
    
    const AVClass *av_class;
    int log_level_offset;
    int codec_type; 
    const struct AVCodec  *codec;
    int     codec_id; 
    
    unsigned int codec_tag;
    void *priv_data;

    
    struct AVCodecInternal *internal;

    
    void *opaque;
    
    int64_t bit_rate;
    
    int bit_rate_tolerance;
    
    int global_quality;

    
    int compression_level;


    
    int flags;
    
    int flags2;
    
    uint8_t *extradata;
    int extradata_size;
    
    AVRational time_base;
    
    int ticks_per_frame;
    
    int delay;

    
    
    int width, height;
    
    int coded_width, coded_height;
    
    int gop_size;
    int pix_fmt;
} Ctx6;
typedef struct Ctx8 {

    const AVClass *av_class;
    int log_level_offset;
    int codec_type; 
    const struct AVCodec  *codec;
    int     codec_id; 

    unsigned int codec_tag;
    void *priv_data;

    struct AVCodecInternal *internal;

    void *opaque;

    int64_t bit_rate;

    int flags;

    int flags2;

    uint8_t *extradata;
    int extradata_size;

    AVRational time_base;

    AVRational pkt_timebase;

    AVRational framerate;

    int delay;

    int width, height;

    int coded_width, coded_height;

    AVRational sample_aspect_ratio;

    int pix_fmt;

    int sw_pix_fmt;

    int color_primaries;

    int color_trc;

    int colorspace;

    int color_range;

    int chroma_sample_location;

    int field_order;

    int refs;

    int has_b_frames;

    int slice_flags;




    void (*draw_horiz_band)(struct Ctx8 *s,
                            const Frame8 *src, int offset[AV_NUM_DATA_POINTERS],
                            int y, int type, int height);

    int (*get_format)(struct Ctx8 *s, const int * fmt);

    int max_b_frames;

    float b_quant_factor;

    float b_quant_offset;

    float i_quant_factor;

    float i_quant_offset;

    float lumi_masking;

    float temporal_cplx_masking;

    float spatial_cplx_masking;

    float p_masking;

    float dark_masking;

     int nsse_weight;

    int me_cmp;

    int me_sub_cmp;

    int mb_cmp;

    int ildct_cmp;


















    int dia_size;

    int last_predictor_count;

    int me_pre_cmp;

    int pre_dia_size;

    int me_subpel_quality;

    int me_range;

    int mb_decision;




    uint16_t *intra_matrix;

    uint16_t *inter_matrix;

    uint16_t *chroma_intra_matrix;

    int intra_dc_precision;

    int mb_lmin;

    int mb_lmax;

    int bidir_refine;

    int keyint_min;

    int gop_size;

    int mv0_threshold;

    int slices;

    int sample_rate; 

    int sample_fmt;  

    AVChannelLayout ch_layout;

    int frame_size;

    int block_align;

    int cutoff;

    int audio_service_type;

    int request_sample_fmt;

    int initial_padding;

    int trailing_padding;

    int seek_preroll;

    int (*get_buffer2)(struct Ctx8 *s, Frame8 *frame, int flags);

    int bit_rate_tolerance;

    int global_quality;

    int compression_level;

    float qcompress;  
    float qblur;      

    int qmin;

    int qmax;

    int max_qdiff;

    int rc_buffer_size;

    int rc_override_count;
    void *rc_override;

    int64_t rc_max_rate;

    int64_t rc_min_rate;

    float rc_max_available_vbv_use;

    float rc_min_vbv_overflow_use;

    int rc_initial_buffer_occupancy;

    int trellis;

    char *stats_out;

    char *stats_in;

    int workaround_bugs;
















    int strict_std_compliance;

    int error_concealment;




    int debug;
















    int err_recognition;

    const struct AVHWAccel *hwaccel;

    void *hwaccel_context;

    AVBufferRef *hw_frames_ctx;

    AVBufferRef *hw_device_ctx;

} Ctx8;
#endif

