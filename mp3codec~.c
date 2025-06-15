#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "ext_systhread.h"
#include <lame/lame.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#define MP3_FRAME_SIZE 1152        // MPEG1 frame size in samples
#define MP3_BUFFER_SIZE 8192       // MP3 output buffer size
#define PCM_BUFFER_SIZE (MP3_FRAME_SIZE * 4)  // PCM buffer with headroom
#define DECODE_BUFFER_SIZE 16384   // Larger decode buffer for accumulation

// Quality to bitrate mapping (0=best, 9=worst) - More aggressive for low quality
// Note: LAME minimum CBR is 32 kbps - lower values get clamped to 32 kbps
static const int QUALITY_BITRATES[10] = {320, 256, 192, 160, 128, 112, 96, 64, 40, 32};

typedef struct _mp3codec {
    t_pxobject ob;
    
    // LAME encoder/decoder
    lame_global_flags *gfp;
    hip_t hip;
    
    // Parameters
    long quality;          // 0-9 LAME quality scale
    double input_gain;     // 0.0-4.0
    double output_gain;    // 0.0-4.0
    long bypass;           // 0/1
    
    // Individual aggressive compression toggles
    long enable_lowpass;   // 0/1 - 4kHz low-pass filter
    long enable_highpass;  // 0/1 - 100Hz high-pass filter
    long enable_ms_stereo; // 0/1 - Force mid/side stereo
    long enable_ath_only;  // 0/1 - ATH-only psychoacoustic model
    long enable_experimental; // 0/1 - Experimental compression modes
    long enable_emphasis;  // 0/1 - Pre-emphasis
    
    // Audio processing state
    long sample_rate;
    long channels;
    long initialized;
    
    // Buffers for encoding
    float *encode_buffer_left;
    float *encode_buffer_right;
    int encode_buffer_fill;
    
    // Buffers for decoding
    unsigned char *mp3_accumulator;
    int mp3_accumulator_fill;
    short *decode_pcm_left;
    short *decode_pcm_right;
    
    // Ring buffer for output smoothing
    float *output_ring_left;
    float *output_ring_right;
    int ring_write_pos;
    int ring_read_pos;
    int ring_size;
    
    // Latency tracking and compensation
    int total_latency_samples;
    double total_latency_ms;
    int lame_encoder_delay;
    int lame_decoder_delay;
    int buffer_latency_samples;
    int decode_delay_compensation;
    
    // Outlets
    void *analysis_outlet;
    void *status_outlet;
    
} t_mp3codec;

// Function prototypes
void *mp3codec_new(t_symbol *s, long argc, t_atom *argv);
void mp3codec_free(t_mp3codec *x);
void mp3codec_dsp64(t_mp3codec *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void mp3codec_perform64(t_mp3codec *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);
void mp3codec_assist(t_mp3codec *x, void *b, long m, long a, char *s);

// Parameter control
void mp3codec_quality(t_mp3codec *x, long n);
void mp3codec_bypass(t_mp3codec *x, long n);
void mp3codec_reset(t_mp3codec *x);

// Individual compression toggles
void mp3codec_lowpass(t_mp3codec *x, long n);
void mp3codec_highpass(t_mp3codec *x, long n);
void mp3codec_msstereo(t_mp3codec *x, long n);
void mp3codec_athonly(t_mp3codec *x, long n);
void mp3codec_experimental(t_mp3codec *x, long n);
void mp3codec_emphasis(t_mp3codec *x, long n);

// Latency reporting
void mp3codec_latency(t_mp3codec *x);

// Internal functions
int mp3codec_init_processor(t_mp3codec *x);
void mp3codec_cleanup_processor(t_mp3codec *x);

// Helper functions
static inline short float_to_short(float sample) {
    int value = (int)(sample * 32767.0f);
    if (value > 32767) value = 32767;
    if (value < -32768) value = -32768;
    return (short)value;
}

static inline float short_to_float(short sample) {
    return sample / 32767.0f;
}

static t_class *mp3codec_class;

void ext_main(void *r)
{
    t_class *c;
    
    c = class_new("mp3codec~", (method)mp3codec_new, (method)mp3codec_free,
                  sizeof(t_mp3codec), NULL, A_GIMME, 0);
    
    class_addmethod(c, (method)mp3codec_dsp64, "dsp64", A_CANT, 0);
    class_addmethod(c, (method)mp3codec_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)mp3codec_quality, "quality", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_bypass, "bypass", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_reset, "reset", 0);
    class_addmethod(c, (method)mp3codec_latency, "latency", 0);
    
    // Individual compression toggle methods
    class_addmethod(c, (method)mp3codec_lowpass, "lowpass", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_highpass, "highpass", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_msstereo, "msstereo", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_athonly, "athonly", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_experimental, "experimental", A_LONG, 0);
    class_addmethod(c, (method)mp3codec_emphasis, "emphasis", A_LONG, 0);
    
    // Attributes
    CLASS_ATTR_LONG(c, "quality", 0, t_mp3codec, quality);
    CLASS_ATTR_FILTER_MIN(c, "quality", 0);
    CLASS_ATTR_FILTER_MAX(c, "quality", 9);
    
    CLASS_ATTR_DOUBLE(c, "input_gain", 0, t_mp3codec, input_gain);
    CLASS_ATTR_FILTER_MIN(c, "input_gain", 0.0);
    CLASS_ATTR_FILTER_MAX(c, "input_gain", 4.0);
    
    CLASS_ATTR_DOUBLE(c, "output_gain", 0, t_mp3codec, output_gain);
    CLASS_ATTR_FILTER_MIN(c, "output_gain", 0.0);
    CLASS_ATTR_FILTER_MAX(c, "output_gain", 4.0);
    
    CLASS_ATTR_LONG(c, "bypass", 0, t_mp3codec, bypass);
    CLASS_ATTR_FILTER_MIN(c, "bypass", 0);
    CLASS_ATTR_FILTER_MAX(c, "bypass", 1);
    
    class_dspinit(c);
    class_register(CLASS_BOX, c);
    mp3codec_class = c;
}

void *mp3codec_new(t_symbol *s, long argc, t_atom *argv)
{
    t_mp3codec *x = (t_mp3codec *)object_alloc(mp3codec_class);
    
    if (x) {
        dsp_setup((t_pxobject *)x, 2);  // Stereo input
        
        // Create outlets (in reverse order)
        x->status_outlet = outlet_new((t_object *)x, NULL);
        x->analysis_outlet = outlet_new((t_object *)x, NULL);
        outlet_new((t_object *)x, "signal");  // Right output
        outlet_new((t_object *)x, "signal");  // Left output
        
        // Initialize parameters
        x->quality = 5;         // Default: 128 kbps
        x->input_gain = 1.0;
        x->output_gain = 1.0;
        x->bypass = 0;
        
        // Initialize compression toggles (all aggressive settings enabled by default)
        x->enable_lowpass = 1;
        x->enable_highpass = 1;
        x->enable_ms_stereo = 1;
        x->enable_ath_only = 1;
        x->enable_experimental = 1;
        x->enable_emphasis = 1;
        
        x->sample_rate = 44100;
        x->channels = 2;
        x->initialized = 0;
        
        // Process constructor arguments
        if (argc >= 1) x->quality = CLAMP((long)atom_getfloat(argv), 0, 9);
        if (argc >= 2) x->input_gain = CLAMP(atom_getfloat(argv+1), 0.0, 4.0);
        if (argc >= 3) x->output_gain = CLAMP(atom_getfloat(argv+2), 0.0, 4.0);
        if (argc >= 4) x->bypass = (atom_getlong(argv+3) != 0);
        
        // Process attributes
        attr_args_process(x, argc, argv);
        
        // Initialize all pointers to NULL
        x->gfp = NULL;
        x->hip = NULL;
        x->encode_buffer_left = NULL;
        x->encode_buffer_right = NULL;
        x->mp3_accumulator = NULL;
        x->decode_pcm_left = NULL;
        x->decode_pcm_right = NULL;
        x->output_ring_left = NULL;
        x->output_ring_right = NULL;
        
        // Initialize processor
        if (mp3codec_init_processor(x) < 0) {
            error("mp3codec~: Failed to initialize MP3 processor");
            mp3codec_cleanup_processor(x);
        }
        
        post("mp3codec~: Initialized - Quality %ld (%d kbps CBR)", 
             x->quality, QUALITY_BITRATES[x->quality]);
    }
    
    return x;
}

void mp3codec_free(t_mp3codec *x)
{
    mp3codec_cleanup_processor(x);
    dsp_free((t_pxobject *)x);
}

int mp3codec_init_processor(t_mp3codec *x)
{
    mp3codec_cleanup_processor(x);
    
    // Initialize encoder
    x->gfp = lame_init();
    if (!x->gfp) {
        error("mp3codec~: Failed to initialize LAME encoder");
        return -1;
    }
    
    // Basic audio parameters
    lame_set_num_channels(x->gfp, x->channels);
    lame_set_in_samplerate(x->gfp, x->sample_rate);
    lame_set_out_samplerate(x->gfp, x->sample_rate);
    
    // CRITICAL: Use CBR mode and set bitrate FIRST
    lame_set_VBR(x->gfp, vbr_off);
    lame_set_brate(x->gfp, QUALITY_BITRATES[x->quality]);
    
    // Set quality parameter (affects psychoacoustic model)
    lame_set_quality(x->gfp, x->quality);
    
    // Apply user-controlled aggressive compression settings
    
    // Always use joint stereo for low bitrates, unless overridden
    if (QUALITY_BITRATES[x->quality] <= 128) {
        lame_set_mode(x->gfp, JOINT_STEREO);
    } else {
        lame_set_mode(x->gfp, x->channels == 2 ? STEREO : MONO);
    }
    
    // Apply individual toggles
    if (x->enable_ms_stereo) {
        lame_set_force_ms(x->gfp, 1);           // Force mid/side stereo
        post("mp3codec~: Enabled forced mid/side stereo");
    }
    
    if (x->enable_ath_only) {
        lame_set_ATHonly(x->gfp, 1);            // Use ATH only (more aggressive)
        lame_set_ATHshort(x->gfp, 1);           // Use ATH for short blocks
        lame_set_no_short_blocks(x->gfp, 0);    // Allow short blocks
        post("mp3codec~: Enabled ATH-only psychoacoustic model");
    }
    
    if (x->enable_emphasis) {
        lame_set_emphasis(x->gfp, 1);           // Add emphasis
        post("mp3codec~: Enabled pre-emphasis");
    }
    
    if (x->enable_experimental) {
        lame_set_experimentalX(x->gfp, 9);      // Most aggressive experimental settings
        lame_set_experimentalY(x->gfp, 1);      // Additional experimental compression
        post("mp3codec~: Enabled experimental compression modes");
    }
    
    if (x->enable_lowpass) {
        // Apply aggressive low-pass filtering based on quality
        if (QUALITY_BITRATES[x->quality] <= 32) {
            lame_set_lowpassfreq(x->gfp, 4000);  // Telephone quality
        } else if (QUALITY_BITRATES[x->quality] <= 64) {
            lame_set_lowpassfreq(x->gfp, 6000);  // Harsh filtering
        } else {
            lame_set_lowpassfreq(x->gfp, 8000);  // Moderate filtering
        }
        post("mp3codec~: Enabled low-pass filter (%d Hz)", 
             QUALITY_BITRATES[x->quality] <= 32 ? 4000 : 
             QUALITY_BITRATES[x->quality] <= 64 ? 6000 : 8000);
    }
    
    if (x->enable_highpass) {
        lame_set_highpassfreq(x->gfp, 100);     // Cut bass
        post("mp3codec~: Enabled high-pass filter (100 Hz)");
    }
    
    // Important: disable the bit reservoir for lower latency
    lame_set_disable_reservoir(x->gfp, 1);
    
    if (lame_init_params(x->gfp) < 0) {
        // If LAME rejects the parameters (common with very low bitrates), try fallback
        if (QUALITY_BITRATES[x->quality] <= 16) {
            post("mp3codec~: LAME rejected %d kbps, trying 32 kbps fallback", QUALITY_BITRATES[x->quality]);
            lame_set_brate(x->gfp, 32);
            lame_set_lowpassfreq(x->gfp, 6000);  // Still keep aggressive filtering
            
            if (lame_init_params(x->gfp) < 0) {
                error("mp3codec~: Failed to initialize LAME even with 32 kbps fallback");
                lame_close(x->gfp);
                x->gfp = NULL;
                return -1;
            } else {
                post("mp3codec~: Successfully initialized with 32 kbps fallback");
            }
        } else {
            error("mp3codec~: Failed to set LAME parameters for %d kbps", QUALITY_BITRATES[x->quality]);
            lame_close(x->gfp);
            x->gfp = NULL;
            return -1;
        }
    }
    
    // Debug: Show actual LAME configuration
    post("mp3codec~: LAME configured - Quality: %d, Bitrate: %d, Mode: %d, Channels: %d", 
         lame_get_quality(x->gfp),
         lame_get_brate(x->gfp), 
         lame_get_mode(x->gfp),
         lame_get_num_channels(x->gfp));
    
    // Initialize decoder
    x->hip = hip_decode_init();
    if (!x->hip) {
        error("mp3codec~: Failed to initialize LAME hip decoder");
        lame_close(x->gfp);
        x->gfp = NULL;
        return -1;
    }
    
    // Allocate buffers
    x->encode_buffer_left = (float*)sysmem_newptrclear(PCM_BUFFER_SIZE * sizeof(float));
    x->encode_buffer_right = (float*)sysmem_newptrclear(PCM_BUFFER_SIZE * sizeof(float));
    x->mp3_accumulator = (unsigned char*)sysmem_newptrclear(DECODE_BUFFER_SIZE);
    x->decode_pcm_left = (short*)sysmem_newptrclear(PCM_BUFFER_SIZE * sizeof(short));
    x->decode_pcm_right = (short*)sysmem_newptrclear(PCM_BUFFER_SIZE * sizeof(short));
    
    // Ring buffer for output smoothing (holds 4 frames worth)
    x->ring_size = MP3_FRAME_SIZE * 4;  // 4608 samples
    x->output_ring_left = (float*)sysmem_newptrclear(x->ring_size * sizeof(float));
    x->output_ring_right = (float*)sysmem_newptrclear(x->ring_size * sizeof(float));
    
    // Reset buffer positions
    x->encode_buffer_fill = 0;
    x->mp3_accumulator_fill = 0;
    x->ring_write_pos = 0;
    x->ring_read_pos = 0;
    
    // Get actual LAME delays after initialization
    x->lame_encoder_delay = lame_get_encoder_delay(x->gfp);
    x->lame_decoder_delay = 528;  // Standard hip decoder delay
    x->buffer_latency_samples = MP3_FRAME_SIZE;  // Our frame buffering
    
    // Calculate total system latency
    x->total_latency_samples = x->lame_encoder_delay + x->lame_decoder_delay + x->buffer_latency_samples;
    x->total_latency_ms = (double)x->total_latency_samples / (double)x->sample_rate * 1000.0;
    x->decode_delay_compensation = 0;  // Disable delay compensation for debugging
    
    x->initialized = 1;
    
    post("mp3codec~: MP3 processor initialized - Quality %ld (%d kbps CBR), Total latency: %.1f ms (%d samples)", 
         x->quality, QUALITY_BITRATES[x->quality], x->total_latency_ms, x->total_latency_samples);
    
    return 0;
}

void mp3codec_cleanup_processor(t_mp3codec *x)
{
    if (!x) return;
    
    // Ensure processing is disabled first
    x->initialized = 0;
    
    // Clear LAME objects safely
    if (x->gfp) {
        lame_close(x->gfp);
        x->gfp = NULL;
    }
    
    if (x->hip) {
        hip_decode_exit(x->hip);
        x->hip = NULL;
    }
    
    // Free memory buffers safely
    if (x->encode_buffer_left) {
        sysmem_freeptr(x->encode_buffer_left);
        x->encode_buffer_left = NULL;
    }
    if (x->encode_buffer_right) {
        sysmem_freeptr(x->encode_buffer_right);
        x->encode_buffer_right = NULL;
    }
    if (x->mp3_accumulator) {
        sysmem_freeptr(x->mp3_accumulator);
        x->mp3_accumulator = NULL;
    }
    if (x->decode_pcm_left) {
        sysmem_freeptr(x->decode_pcm_left);
        x->decode_pcm_left = NULL;
    }
    if (x->decode_pcm_right) {
        sysmem_freeptr(x->decode_pcm_right);
        x->decode_pcm_right = NULL;
    }
    if (x->output_ring_left) {
        sysmem_freeptr(x->output_ring_left);
        x->output_ring_left = NULL;
    }
    if (x->output_ring_right) {
        sysmem_freeptr(x->output_ring_right);
        x->output_ring_right = NULL;
    }
    
    // Reset buffer state
    x->encode_buffer_fill = 0;
    x->mp3_accumulator_fill = 0;
    x->ring_write_pos = 0;
    x->ring_read_pos = 0;
}

void mp3codec_dsp64(t_mp3codec *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
    if (x->sample_rate != (long)samplerate) {
        x->sample_rate = (long)samplerate;
        mp3codec_init_processor(x);
    }
    
    object_method(dsp64, gensym("dsp_add64"), x, mp3codec_perform64, 0, NULL);
}

void mp3codec_perform64(t_mp3codec *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    static long debug_counter = 0;
    debug_counter++;
    
    // Critical safety checks first
    if (!x) {
        return;  // Silent fail for NULL object
    }
    
    // Check if we're in a valid state before proceeding
    if (!x->initialized) {
        // If not initialized, output silence and return
        for (long i = 0; i < sampleframes; i++) {
            outs[0][i] = 0.0;
            outs[1][i] = 0.0;
        }
        return;
    }
    
    if (debug_counter % 200 == 0) {
        post("mp3codec~: perform64 called - sampleframes: %ld, encode_buffer_fill: %d", sampleframes, x->encode_buffer_fill);
    }
    
    if (x->bypass) {
        // Bypass mode - pass input directly to output
        if (debug_counter % 1000 == 0) {
            post("mp3codec~: **BYPASS MODE** - passing input directly to output");
        }
        if (numins >= 2 && numouts >= 2) {
            for (long i = 0; i < sampleframes; i++) {
                outs[0][i] = ins[0][i] * x->input_gain * x->output_gain;
                outs[1][i] = ins[1][i] * x->input_gain * x->output_gain;
            }
        }
        return;
    }
    
    // Check buffer pointers
    if (!x->encode_buffer_left || !x->encode_buffer_right || !x->output_ring_left || !x->output_ring_right) {
        if (debug_counter % 100 == 0) {
            post("mp3codec~: ERROR - NULL buffer pointers (encode_l=%p, encode_r=%p, ring_l=%p, ring_r=%p)", 
                 x->encode_buffer_left, x->encode_buffer_right, x->output_ring_left, x->output_ring_right);
        }
        // Output silence if buffers not ready
        for (long i = 0; i < sampleframes; i++) {
            outs[0][i] = 0.0;
            outs[1][i] = 0.0;
        }
        return;
    }
    
    unsigned char mp3_buffer[MP3_BUFFER_SIZE];
    int mp3_bytes = 0;
    int samples_processed = 0;
    
    // Process input in chunks
    while (samples_processed < sampleframes) {
        int samples_to_copy = sampleframes - samples_processed;
        int buffer_space = MP3_FRAME_SIZE - x->encode_buffer_fill;
        
        if (samples_to_copy > buffer_space) {
            samples_to_copy = buffer_space;
        }
        
        // Copy samples to encode buffer with input gain
        for (int i = 0; i < samples_to_copy; i++) {
            x->encode_buffer_left[x->encode_buffer_fill + i] = 
                (float)(ins[0][samples_processed + i] * x->input_gain);
            x->encode_buffer_right[x->encode_buffer_fill + i] = 
                (float)(ins[1][samples_processed + i] * x->input_gain);
        }
        
        x->encode_buffer_fill += samples_to_copy;
        samples_processed += samples_to_copy;
        
        // If we have a full frame, encode it
        if (x->encode_buffer_fill >= MP3_FRAME_SIZE) {
            if (debug_counter % 200 == 0) {
                post("mp3codec~: About to encode frame (buffer_fill: %d)", x->encode_buffer_fill);
            }
            // Safety check LAME pointers and initialized state
            if (!x->initialized || !x->gfp || !x->hip) {
                if (debug_counter % 100 == 0) {
                    post("mp3codec~: ERROR - Invalid state (initialized=%ld, gfp=%p, hip=%p)", 
                         x->initialized, x->gfp, x->hip);
                }
                x->encode_buffer_fill = 0;  // Reset to prevent infinite loop
                break;
            }
            
            // Convert float to short for encoding
            short pcm_left[MP3_FRAME_SIZE];
            short pcm_right[MP3_FRAME_SIZE];
            
            for (int i = 0; i < MP3_FRAME_SIZE; i++) {
                pcm_left[i] = float_to_short(x->encode_buffer_left[i]);
                pcm_right[i] = float_to_short(x->encode_buffer_right[i]);
            }
            
            // Encode the frame
            mp3_bytes = lame_encode_buffer(x->gfp, 
                                         pcm_left, 
                                         pcm_right, 
                                         MP3_FRAME_SIZE, 
                                         mp3_buffer, 
                                         MP3_BUFFER_SIZE);
            
            // Debug encoding results with more detail
            if (debug_counter % 200 == 0) {
                post("mp3codec~: Encoded frame - MP3 bytes: %d, Quality: %ld, Expected bytes for %d kbps: ~%d", 
                     mp3_bytes, x->quality, QUALITY_BITRATES[x->quality], 
                     (QUALITY_BITRATES[x->quality] * 1152) / (8 * 44100 / 1000));
                     
                // For quality 9, show first few bytes of MP3 data to verify it's actually compressed
                if (x->quality >= 8 && mp3_bytes > 4) {
                    post("mp3codec~: MP3 header bytes: 0x%02X 0x%02X 0x%02X 0x%02X (verify sync and bitrate)", 
                         mp3_buffer[0], mp3_buffer[1], mp3_buffer[2], mp3_buffer[3]);
                }
            }
            
            // Reset encode buffer
            x->encode_buffer_fill = 0;
            
            // If we got MP3 data, decode it immediately
            if (mp3_bytes > 0) {
                // Add to accumulator
                if (x->mp3_accumulator_fill + mp3_bytes < DECODE_BUFFER_SIZE) {
                    memcpy(x->mp3_accumulator + x->mp3_accumulator_fill, 
                           mp3_buffer, 
                           mp3_bytes);
                    x->mp3_accumulator_fill += mp3_bytes;
                }
                
                // Try to decode (single attempt per encoded frame)
                int decoded_samples = hip_decode(x->hip, 
                                               x->mp3_accumulator, 
                                               x->mp3_accumulator_fill, 
                                               x->decode_pcm_left, 
                                               x->decode_pcm_right);
                
                if (decoded_samples > 0) {
                    // Add decoded samples to ring buffer with bounds checking
                    int old_write_pos = x->ring_write_pos;
                    for (int i = 0; i < decoded_samples; i++) {
                        x->output_ring_left[x->ring_write_pos] = 
                            short_to_float(x->decode_pcm_left[i]);
                        x->output_ring_right[x->ring_write_pos] = 
                            short_to_float(x->decode_pcm_right[i]);
                        x->ring_write_pos++;
                        if (x->ring_write_pos >= x->ring_size) {
                            x->ring_write_pos = 0;
                        }
                    }
                    
                    // Debug decode results
                    if (debug_counter % 200 == 0) {
                        post("mp3codec~: Decoded %d samples, write_pos %d->%d", decoded_samples, old_write_pos, x->ring_write_pos);
                    }
                    
                    // Clear accumulator after successful decode
                    x->mp3_accumulator_fill = 0;
                } else if (decoded_samples == 0) {
                    // Need more data - keep accumulating
                    if (debug_counter % 500 == 0) {
                        post("mp3codec~: Decoder needs more data (accumulator: %d bytes)", x->mp3_accumulator_fill);
                    }
                } else {
                    // Error - clear accumulator
                    if (debug_counter % 100 == 0) {
                        post("mp3codec~: Decode error: %d", decoded_samples);
                    }
                    x->mp3_accumulator_fill = 0;
                }
            }
        }
    }
    
    // Output from ring buffer
    for (long i = 0; i < sampleframes; i++) {
        // Check if we have enough samples in the ring buffer
        int available;
        if (x->ring_write_pos >= x->ring_read_pos) {
            available = x->ring_write_pos - x->ring_read_pos;
        } else {
            available = (x->ring_size - x->ring_read_pos) + x->ring_write_pos;
        }
        
        // Debug ring buffer status more frequently
        if (debug_counter % 200 == 0 && i == 0) {
            post("mp3codec~: Ring buffer - available: %d, threshold: %d, read_pos: %d, write_pos: %d", 
                 available, x->total_latency_samples, x->ring_read_pos, x->ring_write_pos);
        }
        
        if (available > x->total_latency_samples || !x->decode_delay_compensation) {
            // **OUTPUTTING MP3 PROCESSED AUDIO**
            outs[0][i] = (double)(x->output_ring_left[x->ring_read_pos] * x->output_gain);
            outs[1][i] = (double)(x->output_ring_right[x->ring_read_pos] * x->output_gain);
            x->ring_read_pos++;
            if (x->ring_read_pos >= x->ring_size) {
                x->ring_read_pos = 0;
            }
            
            // Debug confirmation every so often
            if (debug_counter % 1000 == 0 && i == 0) {
                post("mp3codec~: **OUTPUTTING MP3 PROCESSED AUDIO** from ring buffer");
            }
        } else {
            // Not enough samples yet - output silence
            if (debug_counter % 200 == 0 && i == 0) {
                post("mp3codec~: SILENCE - not enough samples (available: %d, need: %d)", available, x->total_latency_samples);
            }
            outs[0][i] = 0.0;
            outs[1][i] = 0.0;
        }
    }
}

void mp3codec_quality(t_mp3codec *x, long n)
{
    if (!x) return;
    
    // Disable processing FIRST and wait longer for audio thread to exit
    x->initialized = 0;
    
    // Longer delay to ensure audio thread exits perform routine
    systhread_sleep(50);
    
    // Update quality parameter
    long old_quality = x->quality;
    x->quality = CLAMP(n, 0, 9);
    
    // Only reinitialize if quality actually changed
    if (old_quality != x->quality) {
        // Reinitialize processor with new quality
        if (mp3codec_init_processor(x) < 0) {
            error("mp3codec~: Failed to change quality to %ld", x->quality);
            // Try to restore previous quality
            x->quality = old_quality;
            if (mp3codec_init_processor(x) < 0) {
                error("mp3codec~: Failed to restore previous quality - external may be unstable");
            }
        } else {
            post("mp3codec~: Quality changed to %ld (%d kbps CBR)", x->quality, QUALITY_BITRATES[x->quality]);
        }
    } else {
        // Quality unchanged, just re-enable
        x->initialized = 1;
        post("mp3codec~: Quality unchanged at %ld (%d kbps CBR)", x->quality, QUALITY_BITRATES[x->quality]);
    }
}

void mp3codec_bypass(t_mp3codec *x, long n)
{
    x->bypass = (n != 0);
}

void mp3codec_reset(t_mp3codec *x)
{
    if (!x) return;
    
    // Thread-safe reset: disable processing first
    x->initialized = 0;
    
    // Longer delay to ensure audio thread exits
    systhread_sleep(50);
    
    // Reinitialize processor
    if (mp3codec_init_processor(x) < 0) {
        error("mp3codec~: Reset failed - processor may be unstable");
    } else {
        post("mp3codec~: Processor reset successfully");
    }
}

// Individual compression toggle functions
void mp3codec_lowpass(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_lowpass = (n != 0);
    post("mp3codec~: Low-pass filter %s", x->enable_lowpass ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_highpass(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_highpass = (n != 0);
    post("mp3codec~: High-pass filter %s", x->enable_highpass ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_msstereo(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_ms_stereo = (n != 0);
    post("mp3codec~: Forced mid/side stereo %s", x->enable_ms_stereo ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_athonly(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_ath_only = (n != 0);
    post("mp3codec~: ATH-only psychoacoustic model %s", x->enable_ath_only ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_experimental(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_experimental = (n != 0);
    post("mp3codec~: Experimental compression modes %s", x->enable_experimental ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_emphasis(t_mp3codec *x, long n)
{
    if (!x) return;
    x->enable_emphasis = (n != 0);
    post("mp3codec~: Pre-emphasis %s", x->enable_emphasis ? "enabled" : "disabled");
    
    // Reinitialize to apply change
    x->initialized = 0;
    systhread_sleep(50);
    mp3codec_init_processor(x);
}

void mp3codec_latency(t_mp3codec *x)
{
    if (!x || !x->initialized) {
        post("mp3codec~: Not initialized - cannot report latency");
        return;
    }
    
    // Detailed latency breakdown
    post("mp3codec~: Latency Analysis:");
    post("  LAME Encoder Delay: %d samples (%.1f ms)", 
         x->lame_encoder_delay, 
         (double)x->lame_encoder_delay / (double)x->sample_rate * 1000.0);
    post("  LAME Decoder Delay: %d samples (%.1f ms)", 
         x->lame_decoder_delay,
         (double)x->lame_decoder_delay / (double)x->sample_rate * 1000.0);
    post("  Buffer Latency: %d samples (%.1f ms)", 
         x->buffer_latency_samples,
         (double)x->buffer_latency_samples / (double)x->sample_rate * 1000.0);
    post("  TOTAL LATENCY: %d samples (%.1f ms)", 
         x->total_latency_samples, x->total_latency_ms);
    post("  At %d Hz: %.1f audio frames delay", 
         x->sample_rate, (double)x->total_latency_samples / 512.0);
    
    // Send latency data to analysis outlet
    if (x->analysis_outlet) {
        t_atom latency_data[4];
        atom_setfloat(latency_data, x->total_latency_ms);
        atom_setlong(latency_data + 1, x->total_latency_samples);
        atom_setlong(latency_data + 2, x->lame_encoder_delay);
        atom_setlong(latency_data + 3, x->lame_decoder_delay);
        outlet_list(x->analysis_outlet, NULL, 4, latency_data);
    }
}

void mp3codec_assist(t_mp3codec *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET) {
        switch (a) {
            case 0: sprintf(s, "(signal) Left Audio Input"); break;
            case 1: sprintf(s, "(signal) Right Audio Input"); break;
            case 2: sprintf(s, "Control Messages"); break;
        }
    } else {
        switch (a) {
            case 0: sprintf(s, "(signal) Left Audio Output"); break;
            case 1: sprintf(s, "(signal) Right Audio Output"); break;
            case 2: sprintf(s, "Analysis Data"); break;
            case 3: sprintf(s, "Status Messages"); break;
        }
    }
}